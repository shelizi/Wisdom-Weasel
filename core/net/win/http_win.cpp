// Windows：WinHTTP
#include "../http.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <fstream>
#include <memory>

#pragma comment(lib, "winhttp.lib")

namespace net {

namespace {

using Clock = std::chrono::steady_clock;

std::wstring Wide(const std::string& s) {
  if (s.empty())
    return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

struct Handle {
  HINTERNET h = nullptr;
  Handle() = default;
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { Close(); }
  void Close() {
    if (h)
      WinHttpCloseHandle(h);
    h = nullptr;
  }
};

std::string ErrorText(DWORD err) {
  switch (err) {
    case ERROR_WINHTTP_TIMEOUT: return "連線逾時";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "無法連線到伺服器（服務沒開或網址錯誤）";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "找不到主機名稱";
    case ERROR_WINHTTP_SECURE_FAILURE: return "HTTPS 憑證驗證失敗";
    default: return "連線失敗（錯誤 " + std::to_string(err) + "）";
  }
}

struct Target {
  std::wstring host, object;
  INTERNET_PORT port = 0;
  bool https = false;
  bool local = false;
};

bool Crack(const std::string& url, Target* t, std::string* error) {
  const std::wstring wurl = Wide(url);
  URL_COMPONENTS uc = {0};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0}, path[4096] = {0}, extra[4096] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = _countof(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = _countof(path);
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = _countof(extra);
  if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
    *error = "網址格式不正確";
    return false;
  }
  t->host.assign(host, uc.dwHostNameLength);
  t->object = std::wstring(path) + extra;
  t->https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  t->port = uc.nPort ? uc.nPort : t->https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
  t->local = t->host == L"localhost" || t->host == L"127.0.0.1";
  return true;
}

bool OpenSession(const Target& t, const Request& r, Handle* session, Handle* connect, std::string* error) {
  session->h = WinHttpOpen(L"Wisdom-Weasel",
                           t.local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session->h)
    session->h = WinHttpOpen(L"Wisdom-Weasel", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session->h) {
    *error = "無法建立連線";
    return false;
  }
  WinHttpSetTimeouts(session->h, r.connect_timeout_ms, r.connect_timeout_ms, r.receive_timeout_ms,
                     r.receive_timeout_ms);
  connect->h = WinHttpConnect(session->h, t.host.c_str(), t.port, 0);
  if (!connect->h) {
    *error = "無法連線到伺服器";
    return false;
  }
  return true;
}

DWORD StatusCode(HINTERNET request) {
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  return status;
}

// 在已建立的連線上送出請求，讀到的資料交給 on_data
bool Exchange(HINTERNET connect, const Target& t, const Request& r, const OnData& on_data, Response* response,
              std::string* error) {
  Handle request;
  request.h = WinHttpOpenRequest(connect, Wide(r.method).c_str(), t.object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, t.https ? WINHTTP_FLAG_SECURE : 0);
  if (!request.h) {
    *error = "無法連線到伺服器";
    return false;
  }
  std::wstring headers;
  for (const auto& [name, value] : r.headers)
    headers += Wide(name) + L": " + Wide(value) + L"\r\n";
  if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : (DWORD)-1,
                          r.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)r.body.data(), (DWORD)r.body.size(),
                          (DWORD)r.body.size(), 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    *error = ErrorText(GetLastError());
    return false;
  }
  response->status = StatusCode(request.h);
  response->timed_out = false;
  const auto deadline = Clock::now() + std::chrono::milliseconds(r.total_timeout_ms);
  DWORD avail = 0;
  std::string buf;
  while (true) {
    // 讀取失敗（例如太久沒收到資料）是錯誤，不是讀完
    if (!WinHttpQueryDataAvailable(request.h, &avail)) {
      const DWORD err = GetLastError();
      response->timed_out = err == ERROR_WINHTTP_TIMEOUT;
      *error = ErrorText(err);
      return false;
    }
    if (avail == 0)
      break;
    buf.resize(avail);
    DWORD read = 0;
    if (!WinHttpReadData(request.h, &buf[0], avail, &read)) {
      const DWORD err = GetLastError();
      response->timed_out = err == ERROR_WINHTTP_TIMEOUT;
      *error = ErrorText(err);
      return false;
    }
    if (!on_data(buf.data(), read))
      break;
    if (r.total_timeout_ms > 0 && Clock::now() > deadline) {
      response->timed_out = true;
      *error = "連線逾時";
      return false;
    }
  }
  return true;
}

OnData Collect(Response* response, size_t max) {
  response->body.clear();
  return [response, max](const char* data, size_t size) {
    response->body.append(data, size);
    return response->body.size() < max;
  };
}

}  // namespace

bool Stream(const Request& r, const OnData& on_data, Response* response, std::string* error) {
  Target t;
  Handle session, connect;
  return Crack(r.url, &t, error) && OpenSession(t, r, &session, &connect, error) &&
         Exchange(connect.h, t, r, on_data, response, error);
}

bool Fetch(const Request& r, Response* response, std::string* error) {
  return Stream(r, Collect(response, r.max_response), response, error);
}

struct Session::Impl {
  Handle session, connect;
  std::wstring key;  // 主機、埠與協定
};

Session::Session() : impl_(new Impl) {}

Session::~Session() = default;

void Session::Reset() {
  impl_->connect.Close();
  impl_->session.Close();
  impl_->key.clear();
}

bool Session::Fetch(const Request& r, Response* response, std::string* error) {
  Target t;
  if (!Crack(r.url, &t, error))
    return false;
  const std::wstring key = (t.https ? L"https://" : L"http://") + t.host + L":" + std::to_wstring(t.port);
  if (key != impl_->key || !impl_->connect.h) {
    Reset();
    if (!OpenSession(t, r, &impl_->session, &impl_->connect, error)) {
      Reset();
      return false;
    }
    impl_->key = key;
  }
  if (!Exchange(impl_->connect.h, t, r, Collect(response, r.max_response), response, error)) {
    Reset();  // 連線可能已經斷了：下次重連
    return false;
  }
  return true;
}

bool Download(const std::string& url, const std::filesystem::path& dest, const Progress& progress,
              std::string* error) {
  Request r;
  r.url = url;
  r.connect_timeout_ms = 30000;
  r.receive_timeout_ms = 60000;
  Target t;
  Handle session, connect, request;
  if (!Crack(url, &t, error) || !OpenSession(t, r, &session, &connect, error))
    return false;
  request.h = WinHttpOpenRequest(connect.h, L"GET", t.object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, t.https ? WINHTTP_FLAG_SECURE : 0);
  if (!request.h) {
    *error = "無法連線到伺服器";
    return false;
  }
  if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    *error = "連線失敗（錯誤 " + std::to_string(GetLastError()) + "）";
    return false;
  }
  const DWORD status = StatusCode(request.h);
  ULONGLONG total = 0;
  DWORD size = sizeof(total);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                      WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    *error = "伺服器回應 HTTP " + std::to_string(status) +
             (status == 401 || status == 403 ? "（可能需要登入或同意授權條款）" : "");
    return false;
  }
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "無法寫入檔案";
    return false;
  }
  std::unique_ptr<char[]> buf(new char[1 << 20]);
  ULONGLONG done = 0;
  while (true) {
    DWORD read = 0;
    if (!WinHttpReadData(request.h, buf.get(), 1 << 20, &read)) {
      *error = "下載中斷（錯誤 " + std::to_string(GetLastError()) + "）";
      return false;
    }
    if (read == 0)
      break;
    out.write(buf.get(), read);
    done += read;
    if (!progress(done, total)) {
      *error = "已取消";
      return false;
    }
  }
  if (total && done != total) {
    *error = "下載不完整";
    return false;
  }
  return true;
}

}  // namespace net
