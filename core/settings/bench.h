#pragma once

// 模型測試（設定程式的「模型測試」頁）：用各組模型設定跑一組預測與整句校正的題目，
// 比較命中率與延遲。設定程式自己啟動推理行程（WisdomLLMHost）載入模型，不必先套用設定，
// 也不影響正在打字的輸入法；結果經過與輸入法相同的整理（CleanCandidates／CleanCorrection）。
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "../third_party/nlohmann/json.hpp"

namespace settings_bench {

struct Profile {
  std::string name;
  bool remote = false;
  std::string model_path;  // 本機
  bool instruct = true;
  std::string api_url, api_key, model;  // API
  bool no_think = false;
  int think_tokens = 2048;
};

struct Options {
  std::wstring prompt;       // llm/prompt：預測的提示詞
  std::wstring typo_prompt;  // 整句校正的指令（空 = 預設）
  // 本機模型：和輸入法載入時相同的設定
  int n_ctx = 2048;
  int n_gpu_layers = 0;
  int n_threads = 4;
  double temperature = 0.8;
};

struct PredictCase {
  std::wstring context, expected;
};
struct CorrectCase {
  std::wstring context, zhuyin, draft, expected;  // expected 和 draft 相同 = 不該改
};

// 題目：預測一行「前文|期望的詞」；校正一行「前文|注音|初稿|正確的句子」（前文可空白）。
// 空行與 # 開頭的行略過；格式不對的行回報在 errors
std::vector<PredictCase> ParsePredictCases(const std::wstring& text, std::vector<std::wstring>* errors);
std::vector<CorrectCase> ParseCorrectCases(const std::wstring& text, std::vector<std::wstring>* errors);

// 前 top 個候選裡有沒有期望的詞（候選以期望的詞開頭，或是期望的詞的開頭且至少兩個字）
bool PredictionHit(const std::vector<std::wstring>& candidates, const std::wstring& expected,
                   size_t top);
// 校正的結果（已經過 CleanCorrection：空字串表示不改）是否正確
bool CorrectionOk(const std::wstring& cleaned, const CorrectCase& c);

// 依序測每組模型；每完成一步呼叫 report（在呼叫端的執行緒）：
//   {state: "load", profile, ok, ms, error}      載入模型（API 為設定連線）
//   {state: "predict", profile, index, ok, top1, ms, candidates}
//   {state: "correct", profile, index, ok, ms, output}
// cancel 設為 true 時盡快停止（進行中的推理會中斷）
void Run(const std::vector<Profile>& profiles, const Options& options,
         const std::vector<PredictCase>& predict, const std::vector<CorrectCase>& correct,
         const std::atomic<bool>& cancel, const std::function<void(const nlohmann::json&)>& report);

}  // namespace settings_bench
