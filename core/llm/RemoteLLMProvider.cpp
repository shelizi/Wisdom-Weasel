#include "RemoteLLMProvider.h"
#include "../base/devlog.h"
#include "LLMSpecWire.h"
#include "../llm_ipc/client.h"
#include "../base/utf8.h"
#include <logging.h>


namespace {

using llm_ipc::Op;
using llm_ipc::Writer;
using namespace std::chrono_literals;

// 推理請求的時間上限：取消後沒有回應另有較短的限制（Client::Options::cancel_grace）
constexpr auto kInferenceTimeout = std::chrono::milliseconds(5min);

bool Cancelled() {
  return LLMCancelled();
}

}  // namespace

std::unique_ptr<llm_ipc::Client> CreateLLMHostClient(const std::string& label) {
  llm_ipc::Client::Options options;
#ifdef _WIN32
  options.exe = platform::ExecutableDir() / L"WisdomLLMHost.exe";
#else
  options.exe = platform::ExecutableDir() / "WisdomLLMHost";
#endif
  if (g_dev_console && g_dev_console->IsEnabled())
    options.args.push_back("--dev");
  options.on_log = [](const std::string& text) {
    if (g_dev_console && g_dev_console->IsEnabled())
      g_dev_console->Write(text);
  };
  options.on_event = [label](const std::string& text) {
    LOG(WARNING) << "LLM host (" << label << "): " << text;
    if (g_dev_console && g_dev_console->IsEnabled())
      g_dev_console->WriteLine(utf8::ToWide("[LLM] " + label + "：" + text));
  };
  return std::make_unique<llm_ipc::Client>(std::move(options));
}

RemoteLLMProvider::RemoteLLMProvider(const std::string& kind)
    : kind_(kind), client_(CreateLLMHostClient(kind)) {
  Writer create(Op::kCreate, 0);
  create.Str(kind_);
  client_->Setup(std::move(create));
}

RemoteLLMProvider::~RemoteLLMProvider() = default;

void RemoteLLMProvider::RefreshAvailable() {
  auto reply = client_->Call(Writer(Op::kIsAvailable, 0), nullptr);
  available_ = reply && reply->Flag() && reply->Ok();
}

bool RemoteLLMProvider::LoadConfig(const std::string& config_name) {
  Writer request(Op::kLoadConfig, 0);
  request.Str(config_name);
  auto reply = client_->Setup(std::move(request));
  const bool ok = reply && reply->Flag() && reply->Ok();
  RefreshAvailable();
  return ok;
}

bool RemoteLLMProvider::LoadModelDirect(const LLMLocalModelSpec& spec, double temperature) {
  Writer request(Op::kLoadModelDirect, 0);
  ToWire(spec).Write(request);
  request.F64(temperature);
  auto reply = client_->Setup(std::move(request));
  const bool ok = reply && reply->Flag() && reply->Ok();
  RefreshAvailable();
  return ok;
}

void RemoteLLMProvider::SetPromptPrefix(const std::wstring& prompt) {
  Writer request(Op::kSetPromptPrefix, 0);
  request.Str(utf8::FromWide(prompt));
  client_->Setup(std::move(request));
}

void RemoteLLMProvider::ConfigureDirect(const std::string& api_url, const std::string& api_key,
                                        const std::string& model, const std::wstring& prompt,
                                        bool disable_thinking, int think_tokens) {
  Writer request(Op::kConfigureDirect, 0);
  request.Str(api_url);
  request.Str(api_key);
  request.Str(model);
  request.Str(utf8::FromWide(prompt));
  request.Flag(disable_thinking);
  request.I32(think_tokens);
  client_->Setup(std::move(request));
  RefreshAvailable();
}

std::vector<std::wstring> RemoteLLMProvider::PredictCandidates(const std::wstring& context,
                                                               const std::wstring& current_input,
                                                               size_t max_candidates) {
  std::vector<std::wstring> result;
  if (!IsAvailable())
    return result;
  Writer request(Op::kPredict, 0);
  request.Str(utf8::FromWide(context));
  request.Str(utf8::FromWide(current_input));
  request.U32((uint32_t)max_candidates);
  auto reply = client_->Call(std::move(request), Cancelled, kInferenceTimeout);
  if (!reply)
    return result;
  for (const auto& c : reply->StrList())
    result.push_back(utf8::ToWide(c));
  if (!reply->Ok())
    result.clear();
  return result;
}

std::wstring RemoteLLMProvider::CorrectSentence(const std::wstring& context,
                                                const std::wstring& zhuyin,
                                                const std::wstring& draft,
                                                const std::wstring& instruction) {
  if (!IsAvailable())
    return std::wstring();
  Writer request(Op::kCorrect, 0);
  request.Str(utf8::FromWide(context));
  request.Str(utf8::FromWide(zhuyin));
  request.Str(utf8::FromWide(draft));
  request.Str(utf8::FromWide(instruction));
  auto reply = client_->Call(std::move(request), Cancelled, kInferenceTimeout);
  if (!reply)
    return std::wstring();
  const std::string text = reply->Str();
  return reply->Ok() ? utf8::ToWide(text) : std::wstring();
}

bool RemoteLLMProvider::ScoreText(const std::wstring& context, const std::wstring& text,
                                  double* total, std::vector<double>* per_char) {
  // 只有本機模型能算機率，其他種類不必問推理行程
  if (kind_ != "llamacpp" || !IsAvailable())
    return false;
  Writer request(Op::kScore, 0);
  request.Str(utf8::FromWide(context));
  request.Str(utf8::FromWide(text));
  request.Flag(per_char != nullptr);
  auto reply = client_->Call(std::move(request), Cancelled, kInferenceTimeout);
  if (!reply)
    return false;
  const bool ok = reply->Flag();
  const double value = reply->F64();
  std::vector<double> chars = reply->F64List();
  if (!ok || !reply->Ok())
    return false;
  *total = value;
  if (per_char)
    *per_char = std::move(chars);
  return true;
}

RemoteLLMProvider::ChatResult RemoteLLMProvider::ChatShared(const LLMLocalModelSpec& spec,
                                                            const std::string& system_utf8,
                                                            const std::string& user_utf8,
                                                            int max_tokens, std::string* output,
                                                            std::wstring* error) {
  output->clear();
  if (kind_ != "llamacpp" || !IsAvailable())
    return ChatResult::kNotApplicable;
  Writer request(Op::kChat, 0);
  request.Str(spec.model_path);
  request.Flag(spec.instruct);
  request.Str(system_utf8);
  request.Str(user_utf8);
  request.I32(max_tokens);
  // 精煉可能要跑很久：不限時，只在取消時停止
  auto reply = client_->Call(std::move(request), Cancelled);
  if (!reply) {
    *error = Cancelled() ? L"已中止" : L"推理行程意外結束";
    return ChatResult::kFailed;
  }
  const uint8_t result = reply->U8();
  *output = reply->Str();
  *error = utf8::ToWide(reply->Str());
  if (!reply->Ok()) {
    output->clear();
    *error = L"推理行程的回覆格式不對";
    return ChatResult::kFailed;
  }
  // 中途取消時推理行程會回傳已產生的部分：不完整，不能當結果
  if (Cancelled()) {
    output->clear();
    *error = L"已中止";
    return ChatResult::kFailed;
  }
  return result == 0 ? ChatResult::kOk
                     : result == 1 ? ChatResult::kNotApplicable : ChatResult::kFailed;
}

bool RemoteLLMProvider::IsAvailable() const {
  return available_ && !client_->GaveUp();
}

std::string RemoteLLMProvider::GetProviderName() const {
  // 與推理行程裡實際 provider 的名稱相同（呼叫端會拿來判斷種類）
  if (kind_ == "llamacpp")
    return "llama.cpp Local";
  if (kind_ == "hf_constraint")
    return "HF Constraint";
  return "OpenAI Compatible";
}

// ---------------------------------------------------------------------------
// 本機模型的對話工作階段：同樣在推理行程裡執行

struct LLMLocalChatSession::Impl {
  std::unique_ptr<llm_ipc::Client> client = CreateLLMHostClient("session");
};

LLMLocalChatSession::LLMLocalChatSession() = default;

LLMLocalChatSession::~LLMLocalChatSession() = default;

bool LLMLocalChatSession::Open(const LLMLocalModelSpec& spec, std::wstring* error) {
  impl_ = std::make_unique<Impl>();
  Writer create(Op::kCreate, 0);
  create.Str("session");
  Writer open(Op::kSessionOpen, 0);
  ToWire(spec).Write(open);
  if (!impl_->client->Setup(std::move(create), Cancelled)) {
    *error = L"無法啟動推理行程（WisdomLLMHost）";
    impl_.reset();
    return false;
  }
  auto reply = impl_->client->Setup(std::move(open), Cancelled);
  if (!reply) {
    *error = L"推理行程載入模型時結束了（可能是記憶體不足或模型檔損毀）";
    impl_.reset();
    return false;
  }
  const bool ok = reply->Flag();
  const std::string message = reply->Str();
  if (!ok || !reply->Ok()) {
    *error = utf8::ToWide(message);
    impl_.reset();
    return false;
  }
  return true;
}

bool LLMLocalChatSession::Chat(const std::string& system_utf8, const std::string& user_utf8,
                               int max_tokens, std::string* output, std::wstring* error) {
  output->clear();
  if (!impl_) {
    *error = L"模型尚未載入";
    return false;
  }
  Writer request(Op::kSessionChat, 0);
  request.Str(system_utf8);
  request.Str(user_utf8);
  request.I32(max_tokens);
  // 精煉可能要跑很久：不限時，只在要求停止時取消
  auto reply = impl_->client->Call(std::move(request), Cancelled);
  if (!reply) {
    *error = L"推理行程意外結束";
    return false;
  }
  const bool ok = reply->Flag();
  *output = reply->Str();
  *error = utf8::ToWide(reply->Str());
  if (!reply->Ok()) {
    output->clear();
    *error = L"推理行程的回覆格式不對";
    return false;
  }
  // 中途取消（要求停止）時是不完整的部分結果，不能套用
  if (Cancelled()) {
    output->clear();
    *error = L"已中止";
    return false;
  }
  return ok;
}
