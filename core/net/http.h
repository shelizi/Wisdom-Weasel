#pragma once

// HTTP 請求與下載。每個平台各一份實作（net/win/…、net/mac/…）；錯誤訊息是給使用者看的中文。
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace net {

struct Request {
  std::string method = "GET";
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int connect_timeout_ms = 10000;
  int receive_timeout_ms = 60000;  // 多久沒收到資料就放棄
  // 整個請求的總時間上限（0 = 不限）。有些服務在模型還沒算完時會一直送空白保持連線，
  // 只靠 receive_timeout_ms 永遠不會逾時
  int total_timeout_ms = 0;
  size_t max_response = 1 << 20;  // 回應超過就截斷（Fetch）
};

struct Response {
  long status = 0;
  std::string body;
  bool timed_out = false;  // 超過 total_timeout_ms
};

// 送出請求並讀回回應；連線失敗或逾時時回傳 false（HTTP 錯誤碼不算失敗，看 status）。
// 連到 localhost 時不經過系統代理。
bool Fetch(const Request& request, Response* response, std::string* error);

// 串流：收到一段資料就呼叫 on_data，回傳 false 時中止（此時仍回傳 true）。
// response 只填 status 與 timed_out
using OnData = std::function<bool(const char* data, size_t size)>;
bool Stream(const Request& request, const OnData& on_data, Response* response, std::string* error);

// 重用連線：同一台主機的連續請求（例如打字時的預測）不必每次重新連線。
// 主機換了會自動重連；一個 Session 同時只能給一個執行緒用
class Session {
 public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  bool Fetch(const Request& request, Response* response, std::string* error);
  void Reset();  // 關掉目前的連線

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 進度回呼回傳 false 時中止；total 為 0 表示不知道大小
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// 下載到檔案（自動跟隨轉址）；HTTP 不是 200、中斷或取消時回傳 false
bool Download(const std::string& url, const std::filesystem::path& dest, const Progress& progress,
              std::string* error);

}  // namespace net
