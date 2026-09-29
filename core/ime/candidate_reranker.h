#pragma once

// 整句重排（llm/choice/rerank）：Rime 給出符合注音的前幾個整句（translator/max_sentences），
// 本機模型依前文評每一句的通順度，比 Rime 第一句好超過門檻才推薦。
// 候選一律來自 Rime（注音約束），模型只負責排序；評分失敗就不推薦（fail-open）。
// 參數由 test/TuneChoice --topk-rerank 在開放 zh-TW 測試集上評估
//（docs/zhuyin-reranker-evaluation-2026-09.md）。
#include <cmath>
#include <limits>
#include <string>
#include <vector>

class LLMProvider;

namespace ime {

struct RerankOptions {
  double margin = 2.0;           // 要比 Rime 第一句好超過這麼多（整句 log 機率）
  size_t max_sentences = 10;     // 最多評幾句
};

struct RerankResult {
  std::wstring text;  // 推薦的句子；沒有更好的是空字串
  std::wstring best;  // 分數最高的非第一句（沒過門檻也有；shadow 模式拿來對照使用者送出的句子）
  double gain = std::numeric_limits<double>::quiet_NaN();  // 最好的一句比第一句好多少（給信心校準）
  size_t index = 0;   // 在 Rime 整句裡的名次（0 = 第一句）
  size_t scored = 0;  // 評了幾句
};

// sentences[0] 必須是 Rime 目前的第一句（使用者看到的轉換結果）
RerankResult RerankSentences(LLMProvider* scorer,
                             const std::wstring& context,
                             const std::vector<std::wstring>& sentences,
                             const RerankOptions& options = RerankOptions());

}  // namespace ime
