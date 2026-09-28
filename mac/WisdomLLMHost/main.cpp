// macOS 的 LLM 推理行程：鼠鬚管（輸入法）為每個模型啟動一個，透過管道收發請求。
// 放在 Squirrel.app/Contents/MacOS/WisdomLLMHost；服務本身在 core/llm/host_service。
#include "../../core/llm/host_service.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  // 輸入法已結束時寫入管道會收到 SIGPIPE：當成一般錯誤處理
  signal(SIGPIPE, SIG_IGN);
  std::unique_ptr<platform::Pipe> pipe = platform::OpenParentPipe(argc, argv);
  if (!pipe)
    return 2;
  bool dev = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dev") == 0)
      dev = true;
  }
  // 鼠鬚管的資料夾：共用資料在 app 的 SharedSupport，使用者資料在 ~/Library/Rime
  llm_host::RimeDirs dirs;
  dirs.shared_dir = (platform::ExecutableDir().parent_path() / "SharedSupport").string();
  const char* home = getenv("HOME");
  dirs.user_dir = (fs::path(home ? home : "") / "Library" / "Rime").string();
  const char* tmp = getenv("TMPDIR");
  dirs.log_dir = (fs::path(tmp ? tmp : "/tmp") / "rime.squirrel").string();
  dirs.distribution_code_name = "Squirrel";
  dirs.distribution_version = "wisdom";
  llm_host::RunHost(*pipe, dirs, dev);
  // 輸入法關閉了管道：直接結束，不等模型與 GPU 資源逐一釋放（系統會回收）
  _exit(0);
}
