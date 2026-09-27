// macOS／Linux：libcurl（macOS 內建）。錯誤訊息與 Windows 版相同。
// 代理伺服器依 libcurl 的慣例讀環境變數（http_proxy 等），不讀 macOS 系統偏好設定的代理；
// 連到 localhost 時一律不經代理。
// 注意：尚未在 macOS 上編譯驗證。
#include "../http.h"

#include <curl/curl.h>

#include <chrono>
#include <fstream>
#include <mutex>

namespace net {

namespace {

void GlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string ErrorText(CURLcode code) {
  switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
      return "連線逾時";
    case CURLE_COULDNT_CONNECT:
      return "無法連線到伺服器（服務沒開或網址錯誤）";
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
      return "找不到主機名稱";
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE:
      return "HTTPS 憑證驗證失敗";
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
      return "網址格式不正確";
    default:
      return "連線失敗（錯誤 " + std::to_string((int)code) + "）";
  }
}

// 一次請求的狀態（給 libcurl 的回呼）
struct Transfer {
  CURL* curl = nullptr;
  const OnData* on_data = nullptr;
  Response* response = nullptr;
  bool stopped = false;  // on_data 要求中止
};

size_t WriteCallback(char* data, size_t size, size_t count, void* user) {
  Transfer* t = static_cast<Transfer*>(user);
  const size_t n = size * count;
  // 讀到內容時標頭已經收完：先填好狀態碼，on_data 可能依狀態碼決定怎麼處理
  if (t->response->status == 0) {
    long status = 0;
    curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
    t->response->status = status;
  }
  if (!(*t->on_data)(data, n)) {
    t->stopped = true;
    return 0;  // libcurl 以 CURLE_WRITE_ERROR 結束
  }
  return n;
}

// 在 curl 上執行請求。headers 由呼叫端釋放
bool Perform(CURL* curl, const Request& r, const OnData& on_data, Response* response,
             std::string* error) {
  GlobalInit();
  curl_easy_setopt(curl, CURLOPT_URL, r.url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Wisdom-Weasel");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)r.connect_timeout_ms);
  // 多久沒收到資料就放棄：每秒少於 1 位元組持續這麼久
  const long idle_s = r.receive_timeout_ms > 0 ? (r.receive_timeout_ms + 999) / 1000 : 0;
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, idle_s > 0 ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle_s);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(r.total_timeout_ms > 0 ? r.total_timeout_ms : 0));
  if (r.method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
  } else if (r.method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, r.method.c_str());
  }
  if (r.method != "GET") {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)r.body.size());
  }
  curl_slist* headers = nullptr;
  for (const auto& [name, value] : r.headers)
    headers = curl_slist_append(headers, (name + ": " + value).c_str());
  // libcurl 送 POST 時預設加 Expect: 100-continue，有些本機服務不回應，會多等一秒
  headers = curl_slist_append(headers, "Expect:");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  Transfer t;
  t.curl = curl;
  t.on_data = &on_data;
  t.response = response;
  response->status = 0;
  response->timed_out = false;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);

  const CURLcode code = curl_easy_perform(curl);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
  curl_slist_free_all(headers);
  if (response->status == 0) {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response->status = status;
  }
  if (code == CURLE_OK || (code == CURLE_WRITE_ERROR && t.stopped))
    return true;
  response->timed_out = code == CURLE_OPERATION_TIMEDOUT;
  *error = ErrorText(code);
  return false;
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
  CURL* curl = curl_easy_init();
  if (!curl) {
    *error = "無法建立連線";
    return false;
  }
  const bool ok = Perform(curl, r, on_data, response, error);
  curl_easy_cleanup(curl);
  return ok;
}

bool Fetch(const Request& r, Response* response, std::string* error) {
  return Stream(r, Collect(response, r.max_response), response, error);
}

// 同一個 curl handle 會保留連線給之後的請求重用
struct Session::Impl {
  CURL* curl = nullptr;
  ~Impl() {
    if (curl)
      curl_easy_cleanup(curl);
  }
};

Session::Session() : impl_(new Impl) {}

Session::~Session() = default;

void Session::Reset() {
  if (impl_->curl)
    curl_easy_cleanup(impl_->curl);
  impl_->curl = nullptr;
}

bool Session::Fetch(const Request& r, Response* response, std::string* error) {
  GlobalInit();
  if (!impl_->curl)
    impl_->curl = curl_easy_init();
  if (!impl_->curl) {
    *error = "無法建立連線";
    return false;
  }
  if (!Perform(impl_->curl, r, Collect(response, r.max_response), response, error)) {
    Reset();  // 連線可能已經斷了：下次重連
    return false;
  }
  return true;
}

namespace {

struct DownloadState {
  std::ofstream* out = nullptr;
  const Progress* progress = nullptr;
  CURL* curl = nullptr;
  long status = 0;
  bool write_failed = false;
  bool cancelled = false;
};

size_t DownloadWrite(char* data, size_t size, size_t count, void* user) {
  DownloadState* d = static_cast<DownloadState*>(user);
  if (d->status == 0)
    curl_easy_getinfo(d->curl, CURLINFO_RESPONSE_CODE, &d->status);
  if (d->status != 200)
    return 0;  // 錯誤頁不寫進檔案
  const size_t n = size * count;
  d->out->write(data, (std::streamsize)n);
  if (!*d->out) {
    d->write_failed = true;
    return 0;
  }
  return n;
}

int DownloadProgress(void* user, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t) {
  DownloadState* d = static_cast<DownloadState*>(user);
  if (d->status != 200 || done <= 0)
    return 0;
  if (!(*d->progress)((uint64_t)done, (uint64_t)(total > 0 ? total : 0))) {
    d->cancelled = true;
    return 1;
  }
  return 0;
}

}  // namespace

bool Download(const std::string& url, const std::filesystem::path& dest, const Progress& progress,
              std::string* error) {
  GlobalInit();
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "無法寫入檔案";
    return false;
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    *error = "無法建立連線";
    return false;
  }
  DownloadState d;
  d.out = &out;
  d.progress = &progress;
  d.curl = curl;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Wisdom-Weasel");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &DownloadWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &DownloadProgress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &d);
  const CURLcode code = curl_easy_perform(curl);
  if (d.status == 0)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &d.status);
  curl_easy_cleanup(curl);
  out.close();
  if (d.cancelled) {
    *error = "已取消";
    return false;
  }
  if (d.write_failed) {
    *error = "無法寫入檔案";
    return false;
  }
  if (d.status != 0 && d.status != 200) {
    *error = "伺服器回應 HTTP " + std::to_string(d.status) +
             (d.status == 401 || d.status == 403 ? "（可能需要登入或同意授權條款）" : "");
    return false;
  }
  if (code == CURLE_PARTIAL_FILE) {
    *error = "下載不完整";
    return false;
  }
  if (code != CURLE_OK) {
    *error = code == CURLE_OPERATION_TIMEDOUT || code == CURLE_COULDNT_CONNECT ||
                     code == CURLE_COULDNT_RESOLVE_HOST
                 ? ErrorText(code)
                 : "下載中斷（錯誤 " + std::to_string((int)code) + "）";
    return false;
  }
  return true;
}

}  // namespace net
