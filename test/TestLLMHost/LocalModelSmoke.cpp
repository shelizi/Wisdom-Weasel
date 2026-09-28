// 本機模型的端到端測試：透過 RemoteLLMProvider 啟動真正的 WisdomLLMHost（同一資料夾），
// 在推理行程裡用 llama.cpp 載入 GGUF 模型，預測下一個詞並比較句子的分數。
// 預測的原始輸出經過與預測引擎相同的整理（ime::CleanCandidates），才是使用者看到的候選。
// 需要模型檔，不在 ctest 裡自動執行：
//   LocalModelSmoke <model.gguf> [n_gpu_layers]
#include "../../core/base/utf8.h"
#include "../../core/ime/text_rules.h"
#include "../../core/llm/RemoteLLMProvider.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <model.gguf> [n_gpu_layers]\n", argv[0]);
    return 2;
  }
  int failures = 0;
  LLMLocalModelSpec spec;
  spec.model_path = argv[1];
  spec.n_ctx = 2048;
  spec.n_gpu_layers = argc > 2 ? std::atoi(argv[2]) : 0;
  spec.disable_thinking = true;

  using clock = std::chrono::steady_clock;
  auto ms = [](clock::time_point since) {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - since).count();
  };

  RemoteLLMProvider provider("llamacpp");
  auto start = clock::now();
  if (!provider.LoadModelDirect(spec, 0.0) || !provider.IsAvailable()) {
    std::printf("FAIL model not loaded: %s\n", spec.model_path.c_str());
    return 1;
  }
  std::printf("loaded %s (n_gpu_layers=%d) in %lld ms\n", provider.GetProviderName().c_str(),
              spec.n_gpu_layers, ms(start));

  start = clock::now();
  const auto raw = provider.PredictCandidates(L"今天天氣很好，我們一起去", L"", 5);
  std::printf("predict: %zu raw candidates in %lld ms\n", raw.size(), ms(start));
  const auto candidates = ime::CleanCandidates(raw, L"");
  std::printf("candidates:");
  for (const auto& c : candidates)
    std::printf(" [%s]", utf8::FromWide(c).c_str());
  std::printf("\n");
  if (candidates.empty()) {
    std::printf("FAIL no candidates\n");
    ++failures;
  }

  // 自然的句子分數（log 機率總和）要比同樣的字打亂後高
  double natural = 0, shuffled = 0;
  start = clock::now();
  const bool ok1 = provider.ScoreText(L"", L"今天天氣很好", &natural, nullptr);
  const bool ok2 = provider.ScoreText(L"", L"好氣今很天天", &shuffled, nullptr);
  std::printf("score: natural %.2f, shuffled %.2f in %lld ms\n", natural, shuffled, ms(start));
  if (!ok1 || !ok2) {
    std::printf("FAIL scoring failed\n");
    ++failures;
  } else if (!(natural > shuffled)) {
    std::printf("FAIL natural text did not score higher\n");
    ++failures;
  }

  std::printf(failures ? "local model smoke: %d FAILED\n" : "local model smoke: all passed\n", failures);
  return failures ? 1 : 0;
}
