#pragma once

// 預測引擎：打字停頓或送出後，依序產生候選並分段發布
//   1) 個人詞庫（呼叫端先查好，幾乎零延遲）
//   2) 推薦：本機模型在同音字裡挑更通順的整句
//   3) 注音整句校正：LLM 依前文與注音推測真正要打的句子
//   4) LLM 續寫
//   推薦與校正各有「比原句好多少」（本機模型的 log 機率差），經校準換成採用機率後決定先後、太低就不顯示
// 每次請求有序號：有更新的請求（繼續打字）或取消時，舊請求的結果直接丟掉、本機模型立即停止。
// 推理一次只跑一個（llama.cpp 的 context 不能同時給多個執行緒用）。
// 需要 Rime 或介面的部分（查同音字、更新候選窗）由呼叫端以回呼提供，Windows 與 mac 共用。
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "calibration.h"

class LLMProvider;

namespace ime {

enum class CandidateKind : uint8_t { kPrediction, kRecommend, kCorrection };

// 一組候選：推薦、整句校正排最前（候選窗標示「推薦」「校正」），兩者的先後依校準過的採用機率
struct PredictionSet {
  std::vector<std::wstring> candidates;
  std::vector<CandidateKind> kinds;  // 與 candidates 一一對應
  std::vector<double> gains;         // 推薦／校正比原句好多少（log 機率差），沒有是 NaN
  std::vector<double> confidences;   // 校準過的採用機率，還沒校準是 NaN

  CandidateKind Kind(size_t i) const { return i < kinds.size() ? kinds[i] : CandidateKind::kPrediction; }
  bool IsRecommend(size_t i) const { return Kind(i) == CandidateKind::kRecommend; }
  bool IsCorrection(size_t i) const { return Kind(i) == CandidateKind::kCorrection; }
  double Gain(size_t i) const;
  double Confidence(size_t i) const;
  size_t Count(CandidateKind kind) const;
  // 候選窗的註解：「推薦」「校正」，校準過的加上採用機率（例如「推薦 82%」）
  std::wstring Comment(size_t i) const;
};

// 目前可用的模型（呼叫端擁有）；在推理鎖下取用，沒有的是 nullptr
struct PredictionModels {
  LLMProvider* predict = nullptr;  // 智慧預測（續寫）
  LLMProvider* typo = nullptr;     // 整句校正
  LLMProvider* rescore = nullptr;  // 推薦（本機模型）
};

struct PredictionRequest {
  uint64_t tag = 0;                    // 呼叫端的識別（例如哪個 session），回呼時帶回
  std::wstring history;                // 前文
  std::wstring prefix;                 // 打字中的補全：Rime 目前的轉換結果；空 = 送出後預測下一個詞
  std::wstring current_input;          // 目前的按鍵（給模型參考）
  std::vector<std::wstring> personal;  // 個人詞庫的候選
  bool predict = false;                // 要用 LLM 續寫
  bool correct = false;                // 要整句校正（需要 zhuyin 與 prefix）
  bool rescore = false;                // 要推薦（需要 prefix）
  std::wstring zhuyin;                 // 組字的注音
  std::wstring typo_context;           // 校正的前文（組字區裡已確定的部分）
  std::wstring typo_prompt;            // 自訂校正指令，空字串用預設
  double min_confidence = 0;           // 推薦／校正校準過的採用機率低於這個就不顯示（0 = 都顯示）
  unsigned delay_ms = 0;               // 防抖：等這麼久後若已有更新的請求就放棄
};

class PredictionEngine {
 public:
  struct Hooks {
    // 推理鎖下取用目前的模型
    std::function<PredictionModels()> models;
    // 背景執行緒：準備推薦要的各音節的字與同音字（呼叫端自己加鎖，並用 IsCurrent 確認）；
    // 回傳 false 表示不推薦
    std::function<bool(uint64_t tag, uint64_t seq, std::vector<std::wstring>* units,
                       std::vector<std::vector<std::wstring>>* homophones)>
        rescore_input;
    // 背景執行緒：候選更新了。呼叫端加鎖後用 IsCurrent(seq) 確認仍是最新的再更新介面
    std::function<void(uint64_t tag, uint64_t seq, const PredictionSet& set)> on_update;
    // 背景執行緒：把 gain 換成校準過的採用機率（呼叫端自己加鎖）；沒有或還沒校準回傳 NaN
    std::function<double(SuggestionKind kind, double gain)> confidence;
  };

  explicit PredictionEngine(Hooks hooks);
  ~PredictionEngine();
  PredictionEngine(const PredictionEngine&) = delete;
  PredictionEngine& operator=(const PredictionEngine&) = delete;

  // 開始一次預測（在背景執行緒），回傳請求序號
  uint64_t Request(PredictionRequest request);
  // 作廢進行中的請求並清掉候選
  void Cancel();
  bool IsCurrent(uint64_t seq) const;

  PredictionSet Snapshot() const;
  bool HasCandidates() const;
  // 取出第 index 個候選並清掉候選；沒有時回傳 false
  bool Take(size_t index, std::wstring* text, bool* recommend, bool* correction);

  // 更換模型（例如重新部署）時要持有這個鎖，避免背景推理用到被釋放的模型
  std::mutex& InferMutex();

 private:
  struct State;
  static void Run(const std::shared_ptr<State>& state, uint64_t seq, const PredictionRequest& req);
  std::shared_ptr<State> state_;  // 背景執行緒也持有，引擎先解構也安全
};

}  // namespace ime
