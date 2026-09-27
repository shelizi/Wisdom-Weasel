// LLM 推理行程：輸入法服務（WeaselServer）為每個模型啟動一個，透過管道收發請求。
// 模型載入、推理都在這裡跑；這個行程當掉或卡住時，輸入法照常打字，只是少了 LLM 候選。
#include "stdafx.h"
#include "../WeaselServer/DevConsole.h"
#include "../WeaselServer/LLMProvider.h"
#include "../WeaselServer/LLMSpecWire.h"
#include "../core/llm_ipc/host.h"
#include <WeaselConstants.h>
#include <WeaselUtility.h>
#include <rime_api.h>

#include <cstdlib>
#include <cstring>
#include <memory>

CAppModule _Module;
extern DevConsole* g_dev_console;

namespace {

using llm_ipc::Op;
using llm_ipc::Reader;
using llm_ipc::Writer;

// provider 讀 weasel.yaml 用的是 librime 的設定 API；只載入 core 模組，不做部署
void SetupRimeConfig() {
  RimeApi* rime_api = rime_get_api();
  RIME_STRUCT(RimeTraits, traits);
  const std::string shared_dir = wtou8(WeaselSharedDataPath().wstring());
  const std::string user_dir = wtou8(WeaselUserDataPath().wstring());
  const std::string log_dir = WeaselLogPath().u8string();
  traits.shared_data_dir = shared_dir.c_str();
  traits.user_data_dir = user_dir.c_str();
  traits.prebuilt_data_dir = traits.shared_data_dir;
  traits.distribution_code_name = WEASEL_CODE_NAME;
  traits.distribution_version = WEASEL_VERSION;
  traits.app_name = "rime.wisdom-llm";
  traits.log_dir = log_dir.c_str();
  static const char* modules[] = {"core", nullptr};
  traits.modules = modules;
  rime_api->setup(&traits);
  rime_api->initialize(&traits);
}

struct Host {
  std::unique_ptr<LLMProvider> provider;
  std::unique_ptr<LLMLocalChatSession> session;

  void Handle(Reader& in, Writer& out) {
    switch (in.op()) {
      case Op::kCreate: {
        const std::string kind = in.Str();
        if (kind == "llamacpp")
          provider = std::make_unique<LlamaCppProvider>();
        else if (kind == "hf_constraint")
          provider = std::make_unique<HFConstraintProvider>();
        else if (kind == "openai")
          provider = std::make_unique<OpenAICompatibleProvider>();
        else if (kind == "session")
          session = std::make_unique<LLMLocalChatSession>();
        out.Flag(provider || session);
        break;
      }
      case Op::kLoadConfig: {
        const std::string name = in.Str();
        out.Flag(provider && provider->LoadConfig(name));
        break;
      }
      case Op::kLoadModelDirect: {
        llm_ipc::ModelSpecFields spec;
        spec.Read(in);
        const double temperature = in.F64();
        auto* llama = dynamic_cast<LlamaCppProvider*>(provider.get());
        out.Flag(llama && llama->LoadModelDirect(FromWire(spec), temperature));
        break;
      }
      case Op::kConfigureDirect: {
        const std::string api_url = in.Str(), api_key = in.Str(), model = in.Str();
        const std::string prompt = in.Str();
        const bool disable_thinking = in.Flag();
        const int think_tokens = in.I32();
        if (auto* api = dynamic_cast<OpenAICompatibleProvider*>(provider.get()))
          api->ConfigureDirect(api_url, api_key, model, u8tow(prompt), disable_thinking,
                               think_tokens);
        break;
      }
      case Op::kSetPromptPrefix: {
        const std::string prompt = in.Str();
        if (auto* llama = dynamic_cast<LlamaCppProvider*>(provider.get()))
          llama->SetPromptPrefix(u8tow(prompt));
        break;
      }
      case Op::kPredict: {
        const std::wstring context = u8tow(in.Str()), input = u8tow(in.Str());
        const uint32_t max_candidates = in.U32();
        std::vector<std::string> list;
        if (provider && in.Ok()) {
          for (const auto& c : provider->PredictCandidates(context, input, max_candidates))
            list.push_back(wtou8(c));
        }
        out.StrList(list);
        break;
      }
      case Op::kCorrect: {
        const std::wstring context = u8tow(in.Str()), zhuyin = u8tow(in.Str());
        const std::wstring draft = u8tow(in.Str()), instruction = u8tow(in.Str());
        out.Str(provider && in.Ok()
                    ? wtou8(provider->CorrectSentence(context, zhuyin, draft, instruction))
                    : std::string());
        break;
      }
      case Op::kScore: {
        const std::wstring context = u8tow(in.Str()), text = u8tow(in.Str());
        const bool want_per_char = in.Flag();
        double total = 0;
        std::vector<double> per_char;
        const bool ok = provider && in.Ok() &&
                        provider->ScoreText(context, text, &total,
                                            want_per_char ? &per_char : nullptr);
        out.Flag(ok);
        out.F64(total);
        out.F64List(per_char);
        break;
      }
      case Op::kIsAvailable:
        out.Flag(provider && provider->IsAvailable());
        break;
      case Op::kSessionOpen: {
        llm_ipc::ModelSpecFields spec;
        spec.Read(in);
        std::wstring error;
        const bool ok = session && in.Ok() && session->Open(FromWire(spec), &error);
        out.Flag(ok);
        out.Str(wtou8(error));
        break;
      }
      case Op::kSessionChat: {
        const std::string system = in.Str(), user = in.Str();
        const int max_tokens = in.I32();
        std::string output;
        std::wstring error;
        const bool ok =
            session && in.Ok() && session->Chat(system, user, max_tokens, &output, &error);
        out.Flag(ok);
        out.Str(output);
        out.Str(wtou8(error));
        break;
      }
      default:
        break;
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  // 當掉時不跳錯誤對話框：馬上結束，輸入法才能立刻察覺並在下次請求時重新啟動
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

  std::unique_ptr<platform::Pipe> pipe = platform::OpenParentPipe(argc, argv);
  if (!pipe)
    return 2;
  static DevConsole relay;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dev") == 0 && relay.Initialize())
      g_dev_console = &relay;
  }
  SetupRimeConfig();

  Host host;
  llm_ipc::Serve(*pipe, [&host](Reader& in, Writer& out, const std::function<bool()>& cancelled) {
    LLMCancelScope cancel(cancelled);
    host.Handle(in, out);
  });
  // 輸入法關閉了管道：直接結束，不等模型與 GPU 資源逐一釋放（系統會回收）
  TerminateProcess(GetCurrentProcess(), 0);
  return 0;
}
