#pragma once

// 整句重排（llm/choice/rerank）：Rime 給出符合注音的前幾個整句（translator/max_sentences），
// 本機模型依前文評每一句的通順度，比 Rime 第一句好超過門檻才推薦。
// 候選一律來自 Rime（注音約束），模型只負責排序；評分失敗就不推薦（fail-open）。
// 參數由 test/TuneChoice --topk-rerank 在開放 zh-TW 測試集上評估
//（docs/zhuyin-reranker-evaluation-2026-09.md）。
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

class LLMProvider;

namespace ime {

struct RerankOptions {
  double margin = 2.0;           // 要比 Rime 第一句好超過這麼多（整句 log 機率）
  size_t max_sentences = 10;     // 最多評幾句
  size_t context_chars = 30;     // 給模型的前文最多幾個字（呼叫端截好；這裡只是傳遞設定）
  // 長度正規化：分數除以字數的 alpha 次方（0 = 不正規化）。Rime 的整句字數多半相同，差異在拆詞不同時
  double length_alpha = 0.0;
  // 個人詞庫：句子裡常打的詞加分（呼叫端提供，回傳 ≥ 0）；權重 0 = 不用
  std::function<double(const std::wstring&)> personal;
  double personal_weight = 0.0;
};

// 排序用的分數：模型分數（可正規化）加上個人詞庫；評分失敗（NaN）照樣是 NaN
double RerankScore(double lm_score, const std::wstring& text, const RerankOptions& options);

struct RerankResult {
  std::wstring text;  // 推薦的句子；沒有更好的是空字串
  std::wstring best;  // 分數最高的非第一句（沒過門檻也有；shadow 模式拿來對照使用者送出的句子）
  double gain = std::numeric_limits<double>::quiet_NaN();  // 最好的一句比第一句好多少（給信心校準）
  size_t index = 0;   // 在 Rime 整句裡的名次（0 = 第一句）
  size_t scored = 0;  // 評了幾句
  // 評過的每一句與分數（排序用的分數，評分失敗是 NaN）；候選窗顯示機率用
  std::vector<std::wstring> texts;
  std::vector<double> scores;
  std::vector<double> lm_scores;  // 模型原始的整句 log 機率
  // 查到但沒評分的尾巴（選字紀錄要記超過評分上限的候選時）
  std::vector<std::wstring> unscored;
  double ms = 0;                  // 評分花的時間
};

// 評過的整句彼此比較的機率（softmax，加起來是 1）；評分失敗的句子是 NaN
std::vector<double> SentenceProbabilities(const std::vector<double>& scores);

// sentences[0] 必須是 Rime 目前的第一句（使用者看到的轉換結果）
RerankResult RerankSentences(LLMProvider* scorer,
                             const std::wstring& context,
                             const std::vector<std::wstring>& sentences,
                             const RerankOptions& options = RerankOptions());

}  // namespace ime
