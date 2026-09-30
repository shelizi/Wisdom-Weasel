#include "candidate_reranker.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include "../base/devlog.h"
#include "../llm/LLMProvider.h"
#include "ZhuyinPreview.h"

namespace ime {

double RerankScore(double lm_score, const std::wstring& text, const RerankOptions& options) {
  if (std::isnan(lm_score))
    return lm_score;
  double score = lm_score;
  if (options.length_alpha > 0) {
    const double chars = (double)(std::max)((size_t)1, zhuyin_preview::SplitChars(text).size());
    score /= std::pow(chars, options.length_alpha);
  }
  if (options.personal_weight > 0 && options.personal)
    score += options.personal_weight * options.personal(text);
  return score;
}

RerankResult RerankSentences(LLMProvider* scorer,
                             const std::wstring& context,
                             const std::vector<std::wstring>& sentences,
                             const RerankOptions& options) {
  RerankResult result;
  if (!scorer || sentences.size() < 2 || sentences[0].empty())
    return result;
  std::vector<std::wstring> texts(
      sentences.begin(), sentences.begin() + (std::min)(sentences.size(), options.max_sentences));
  std::vector<double> lm;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = scorer->ScoreBatch(context, texts, &lm);
  result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!ok || lm.size() != texts.size() || std::isnan(lm[0]))
    return result;
  std::vector<double> scores(lm.size());
  for (size_t i = 0; i < lm.size(); ++i)
    scores[i] = RerankScore(lm[i], texts[i], options);
  result.texts = texts;
  result.scores = scores;
  result.lm_scores = lm;
  size_t best = 0;
  for (size_t i = 1; i < scores.size(); ++i) {
    if (!std::isnan(scores[i]))
      ++result.scored;
    if (!std::isnan(scores[i]) && scores[i] > scores[best])
      best = i;
  }
  ++result.scored;
  if (best > 0) {
    result.gain = scores[best] - scores[0];
    result.index = best;
    result.best = texts[best];
    if (result.gain > options.margin)
      result.text = texts[best];
  }
  if (g_dev_console && g_dev_console->IsEnabled()) {
    // 句數、耗時、第一句與最好一句的分數與差距（原方案 §18 的 dev console 項目）
    std::wostringstream line;
    line.precision(2);
    line << std::fixed << L"[LLM] 整句重排：" << result.scored << L" 句 " << result.ms << L" ms；第一句 "
         << texts[0] << L" " << scores[0];
    if (best == 0)
      line << L"（最好，不換）";
    else
      line << L" → 第 " << best + 1 << L" 句 " << texts[best] << L" " << scores[best] << L"（+" << result.gain
           << (result.text.empty() ? L"，未過門檻 " : L"，推薦；門檻 ") << options.margin << L"）";
    g_dev_console->WriteLine(line.str());
  }
  return result;
}

std::vector<double> SentenceProbabilities(const std::vector<double>& scores) {
  std::vector<double> probs(scores.size(), std::numeric_limits<double>::quiet_NaN());
  double best = -std::numeric_limits<double>::infinity();
  for (double s : scores) {
    if (!std::isnan(s))
      best = (std::max)(best, s);
  }
  if (std::isinf(best))
    return probs;
  double sum = 0;
  for (size_t i = 0; i < scores.size(); ++i) {
    if (!std::isnan(scores[i]))
      sum += probs[i] = std::exp(scores[i] - best);  // 減掉最大的，避免 exp 溢位
  }
  for (double& p : probs) {
    if (!std::isnan(p))
      p /= sum;
  }
  return probs;
}

}  // namespace ime
