#pragma once

// 在獨立的推理行程（WisdomLLMHost.exe）裡執行的 LLM provider。
// 輸入法服務只保留這個代理：模型載入與推理都在推理行程裡，它當掉或卡住時
// 這裡的呼叫回傳空結果，打字不受影響；下次呼叫時自動重新啟動推理行程。
// IsAvailable、GetProviderName 不經過推理行程，打字的執行緒可以隨時呼叫。
#include "LLMProvider.h"

#include <atomic>
#include <memory>
#include <string>

namespace llm_ipc {
class Client;
}

class RemoteLLMProvider : public LLMProvider {
 public:
  // kind：llamacpp、openai、hf_constraint（與 llm/provider_type 相同）
  explicit RemoteLLMProvider(const std::string& kind);
  ~RemoteLLMProvider() override;

  bool LoadConfig(const std::string& config_name) override;
  std::vector<std::wstring> PredictCandidates(const std::wstring& context,
                                              const std::wstring& current_input,
                                              size_t max_candidates) override;
  std::wstring CorrectSentence(const std::wstring& context, const std::wstring& zhuyin,
                               const std::wstring& draft,
                               const std::wstring& instruction) override;
  bool ScoreText(const std::wstring& context, const std::wstring& text, double* total,
                 std::vector<double>* per_char) override;
  bool IsAvailable() const override;
  std::string GetProviderName() const override;

  // 對應 LlamaCppProvider::LoadModelDirect / SetPromptPrefix（kind 為 llamacpp）
  bool LoadModelDirect(const LLMLocalModelSpec& spec, double temperature);
  void SetPromptPrefix(const std::wstring& prompt);
  // 對應 OpenAICompatibleProvider::ConfigureDirect（kind 為 openai）
  void ConfigureDirect(const std::string& api_url, const std::string& api_key,
                       const std::string& model, const std::wstring& prompt,
                       bool disable_thinking, int think_tokens);

 private:
  void RefreshAvailable();

  const std::string kind_;
  std::unique_ptr<llm_ipc::Client> client_;
  std::atomic<bool> available_{false};
};

// 啟動推理行程的共用設定（RemoteLLMProvider 與 LLMLocalChatSession 共用）
std::unique_ptr<llm_ipc::Client> CreateLLMHostClient(const std::string& label);
