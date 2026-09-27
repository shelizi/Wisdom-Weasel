#pragma once

// HTTP 請求與下載。每個平台各一份實作（net/win/…、net/mac/…）；錯誤訊息是給使用者看的中文。
#include <cstdint>
#include <filesystem>
#include <functional>
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
  int receive_timeout_ms = 60000;
  size_t max_response = 1 << 20;  // 回應超過就截斷
};

struct Response {
  long status = 0;
  std::string body;
};

// 送出請求並讀回回應；連線失敗時回傳 false（HTTP 錯誤碼不算失敗，看 status）。
// 連到 localhost 時不經過系統代理。
bool Fetch(const Request& request, Response* response, std::string* error);

// 進度回呼回傳 false 時中止；total 為 0 表示不知道大小
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// 下載到檔案（自動跟隨轉址）；HTTP 不是 200、中斷或取消時回傳 false
bool Download(const std::string& url, const std::filesystem::path& dest, const Progress& progress,
              std::string* error);

}  // namespace net
