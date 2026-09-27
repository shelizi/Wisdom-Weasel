// Windows：WinHTTP
#include "../http.h"

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <memory>

#pragma comment(lib, "winhttp.lib")

namespace net {

namespace {

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
  ~Handle() {
    if (h)
      WinHttpCloseHandle(h);
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

// 建立連線與請求；失敗時設定 error
bool Open(const std::string& method, const std::string& url, int connect_ms, int receive_ms,
          Handle* session, Handle* connect, Handle* request, std::string* error) {
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
  const std::wstring host_s(host, uc.dwHostNameLength);
  const bool local = host_s == L"localhost" || host_s == L"127.0.0.1";
  session->h = WinHttpOpen(L"Wisdom-Weasel",
                           local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session->h)
    session->h = WinHttpOpen(L"Wisdom-Weasel", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session->h) {
    *error = "無法建立連線";
    return false;
  }
  WinHttpSetTimeouts(session->h, connect_ms, connect_ms, receive_ms, receive_ms);
  connect->h = WinHttpConnect(session->h, host_s.c_str(), uc.nPort, 0);
  const std::wstring object = std::wstring(path) + extra;
  request->h = connect->h ? WinHttpOpenRequest(connect->h, Wide(method).c_str(), object.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                          : nullptr;
  if (!request->h) {
    *error = "無法連線到伺服器";
    return false;
  }
  return true;
}

DWORD StatusCode(HINTERNET request) {
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  return status;
}

}  // namespace

bool Fetch(const Request& r, Response* response, std::string* error) {
  Handle session, connect, request;
  if (!Open(r.method, r.url, r.connect_timeout_ms, r.receive_timeout_ms, &session, &connect, &request,
            error))
    return false;
  std::wstring headers;
  for (const auto& [name, value] : r.headers)
    headers += Wide(name) + L": " + Wide(value) + L"\r\n";
  if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : (DWORD)-1,
                          r.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)r.body.data(),
                          (DWORD)r.body.size(), (DWORD)r.body.size(), 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    *error = ErrorText(GetLastError());
    return false;
  }
  response->status = StatusCode(request.h);
  response->body.clear();
  DWORD avail = 0;
  while (WinHttpQueryDataAvailable(request.h, &avail) && avail > 0 &&
         response->body.size() < r.max_response) {
    std::string buf(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request.h, &buf[0], avail, &read))
      break;
    response->body.append(buf.data(), read);
  }
  return true;
}

bool Download(const std::string& url, const std::filesystem::path& dest, const Progress& progress,
              std::string* error) {
  Handle session, connect, request;
  if (!Open("GET", url, 30000, 60000, &session, &connect, &request, error))
    return false;
  if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                          0) ||
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
