#pragma once

// LLM 推理行程的服務：收輸入法的請求、呼叫 provider、回傳結果。
// 各平台的推理程式（Windows 的 WisdomLLMHost.exe、mac 的 WisdomLLMHost）只負責取得管道與
// Rime 資料夾，再呼叫 RunHost。
#include <string>

#include "../platform/process.h"

namespace llm_host {

struct RimeDirs {
  std::string shared_dir;  // Rime 共用資料夾（UTF-8）
  std::string user_dir;    // Rime 使用者資料夾
  std::string log_dir;
  std::string distribution_code_name;
  std::string distribution_version;
};

// 服務到輸入法關閉管道為止。dev：把開發終端的日誌轉給輸入法顯示
void RunHost(platform::Pipe& pipe, const RimeDirs& dirs, bool dev);

}  // namespace llm_host
