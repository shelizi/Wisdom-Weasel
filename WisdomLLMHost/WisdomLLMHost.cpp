// LLM 推理行程：輸入法服務（WeaselServer）為每個模型啟動一個，透過管道收發請求。
// 模型載入、推理都在這裡跑；這個行程當掉或卡住時，輸入法照常打字，只是少了 LLM 候選。
// 服務本身在 core/llm/host_service（與 mac 共用），這裡只取得管道與 Rime 資料夾。
#include "stdafx.h"
#include "../core/llm/host_service.h"
#include <WeaselConstants.h>
#include <WeaselUtility.h>

#include <cstring>
#include <memory>

CAppModule _Module;

int main(int argc, char** argv) {
  // 當掉時不跳錯誤對話框：馬上結束，輸入法才能立刻察覺並在下次請求時重新啟動
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

  std::unique_ptr<platform::Pipe> pipe = platform::OpenParentPipe(argc, argv);
  if (!pipe)
    return 2;
  bool dev = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dev") == 0)
      dev = true;
  }
  llm_host::RimeDirs dirs;
  dirs.shared_dir = wtou8(WeaselSharedDataPath().wstring());
  dirs.user_dir = wtou8(WeaselUserDataPath().wstring());
  dirs.log_dir = WeaselLogPath().u8string();
  dirs.distribution_code_name = WEASEL_CODE_NAME;
  dirs.distribution_version = WEASEL_VERSION;
  llm_host::RunHost(*pipe, dirs, dev);
  // 輸入法關閉了管道：直接結束，不等模型與 GPU 資源逐一釋放（系統會回收）
  TerminateProcess(GetCurrentProcess(), 0);
  return 0;
}
