// 測試用的推理行程：用和 WisdomLLMHost 相同的
// llm_ipc::Serve，依請求內容模擬各種狀況。 kPredict 的「目前輸入」決定行為：
//   echo    回覆前文
//   wait    一直等到被取消，回覆 "cancelled"
//   crash   直接當掉
//   hang    不理會取消，永遠不回覆
//   log     送一行記錄後回覆
//   setups  回覆目前收到過幾個設定請求（用來確認重新啟動後有重送）
//   exit    回覆後不久自行結束（模擬兩次請求之間當掉）
#include "../../core/llm_ipc/host.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  auto pipe = platform::OpenParentPipe(argc, argv);
  if (!pipe)
    return 2;
  int setups = 0;
  llm_ipc::Serve(*pipe, [&](llm_ipc::Reader& in, llm_ipc::Writer& out,
                            const std::function<bool()>& cancelled) {
    switch (in.op()) {
      case llm_ipc::Op::kCreate:
      case llm_ipc::Op::kLoadConfig:
        ++setups;
        out.Flag(true);
        break;
      case llm_ipc::Op::kPredict: {
        const std::string context = in.Str(), mode = in.Str();
        if (mode == "echo") {
          out.StrList({context});
        } else if (mode == "wait") {
          while (!cancelled())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          out.StrList({"cancelled"});
        } else if (mode == "crash") {
          std::abort();
        } else if (mode == "hang") {
          while (true)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } else if (mode == "log") {
          llm_ipc::Log("hello from host");
          out.StrList({"logged"});
        } else if (mode == "setups") {
          out.StrList({std::to_string(setups)});
        } else if (mode == "exit") {
          out.StrList({"bye"});
          std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::abort();
          }).detach();
        }
        break;
      }
      default:
        break;
    }
  });
  return 0;
}
