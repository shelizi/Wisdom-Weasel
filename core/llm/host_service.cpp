#include "host_service.h"

#include <rime_api.h>

#include <functional>
#include <memory>

#include "../base/devlog.h"
#include "../base/utf8.h"
#include "../llm_ipc/host.h"
#include "LLMProvider.h"
#include "LLMSpecWire.h"

namespace llm_host {

namespace {

using llm_ipc::Op;
using llm_ipc::Reader;
using llm_ipc::Writer;

// provider 讀 weasel.yaml 用的是 librime 的設定 API；只載入 core 模組，不做部署
void SetupRimeConfig(const RimeDirs& dirs) {
  RimeApi* rime_api = rime_get_api();
  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = dirs.shared_dir.c_str();
  traits.user_data_dir = dirs.user_dir.c_str();
  traits.prebuilt_data_dir = traits.shared_data_dir;
  traits.distribution_code_name = dirs.distribution_code_name.c_str();
  traits.distribution_version = dirs.distribution_version.c_str();
  traits.app_name = "rime.wisdom-llm";
  traits.log_dir = dirs.log_dir.c_str();
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
          api->ConfigureDirect(api_url, api_key, model, utf8::ToWide(prompt), disable_thinking,
                               think_tokens);
        break;
      }
      case Op::kSetPromptPrefix: {
        const std::string prompt = in.Str();
        if (auto* llama = dynamic_cast<LlamaCppProvider*>(provider.get()))
          llama->SetPromptPrefix(utf8::ToWide(prompt));
        break;
      }
      case Op::kPredict: {
        const std::wstring context = utf8::ToWide(in.Str()), input = utf8::ToWide(in.Str());
        const uint32_t max_candidates = in.U32();
        std::vector<std::string> list;
        if (provider && in.Ok()) {
          for (const auto& c : provider->PredictCandidates(context, input, max_candidates))
            list.push_back(utf8::FromWide(c));
        }
        out.StrList(list);
        break;
      }
      case Op::kCorrect: {
        const std::wstring context = utf8::ToWide(in.Str()), zhuyin = utf8::ToWide(in.Str());
        const std::wstring draft = utf8::ToWide(in.Str()), instruction = utf8::ToWide(in.Str());
        out.Str(provider && in.Ok()
                    ? utf8::FromWide(provider->CorrectSentence(context, zhuyin, draft, instruction))
                    : std::string());
        break;
      }
      case Op::kScore: {
        const std::wstring context = utf8::ToWide(in.Str()), text = utf8::ToWide(in.Str());
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
        out.Str(utf8::FromWide(error));
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
        out.Str(utf8::FromWide(error));
        break;
      }
      case Op::kChat: {
        // 精煉借用這個行程已載入的模型：要是同一個模型檔與設定、提示放得下
        const std::string model_path = in.Str();
        const bool instruct = in.Flag();
        const std::string system = in.Str(), user = in.Str();
        const int max_tokens = in.I32();
        auto* llama = dynamic_cast<LlamaCppProvider*>(provider.get());
        uint8_t result = 1;
        std::string output;
        std::wstring error;
        if (llama && in.Ok() && llama->IsModel(model_path, instruct)) {
          bool too_long = false;
          if (llama->ChatWithin(system, user, max_tokens, true, &output, &error, &too_long))
            result = 0;
          else if (!too_long)
            result = 2;
        }
        out.U8(result);
        out.Str(output);
        out.Str(utf8::FromWide(error));
        break;
      }
      default:
        break;
    }
  }
};

// 推理行程裡的開發終端：不開自己的主控台，把文字原樣轉給輸入法的開發終端顯示
class RelayLog : public DevLog {
 public:
  bool IsEnabled() const override { return true; }
  void Write(const std::string& text) override {
    if (!text.empty())
      llm_ipc::Log(text);
  }
};

}  // namespace

void RunHost(platform::Pipe& pipe, const RimeDirs& dirs, bool dev) {
  static RelayLog relay;
  if (dev)
    g_dev_console = &relay;
  SetupRimeConfig(dirs);
  Host host;
  llm_ipc::Serve(pipe, [&host](Reader& in, Writer& out, const std::function<bool()>& cancelled) {
    LLMCancelScope cancel(cancelled);
    host.Handle(in, out);
  });
}

}  // namespace llm_host
