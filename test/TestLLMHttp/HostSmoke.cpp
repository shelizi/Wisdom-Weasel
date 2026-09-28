// 推理行程的端到端測試：透過 RemoteLLMProvider 啟動真正的
// WisdomLLMHost.exe（同一資料夾）， 在推理行程裡用 OpenAI 相容 API 向
// mock_server.py 預測。由 run.bat 在主測試之後執行
#include "../../core/llm/RemoteLLMProvider.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  const std::string base =
      std::string("http://127.0.0.1:") + (argc > 1 ? argv[1] : "18765");
  int failures = 0;
  RemoteLLMProvider provider("openai");
  provider.ConfigureDirect(base + "/v1/chat/completions", "k", "m", L"", false,
                           0);
  if (!provider.IsAvailable()) {
    std::printf("FAIL host provider not available\n");
    ++failures;
  }
  const auto candidates = provider.PredictCandidates(L"前文", L"", 5);
  if (candidates.size() != 4 || candidates[0] != L"候選一") {
    std::printf("FAIL host prediction: %zu candidates\n", candidates.size());
    ++failures;
  }
  std::printf(failures ? "host smoke: %d FAILED\n" : "host smoke: all passed\n",
              failures);
  return failures ? 1 : 0;
}
