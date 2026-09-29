#include "candidate_reranker.h"

#include <algorithm>

#include "../base/devlog.h"
#include "../llm/LLMProvider.h"

namespace ime {

RerankResult RerankSentences(LLMProvider* scorer,
                             const std::wstring& context,
                             const std::vector<std::wstring>& sentences,
                             const RerankOptions& options) {
  RerankResult result;
  if (!scorer || sentences.size() < 2 || sentences[0].empty())
    return result;
  std::vector<std::wstring> texts(
      sentences.begin(), sentences.begin() + (std::min)(sentences.size(), options.max_sentences));
  std::vector<double> scores;
  if (!scorer->ScoreBatch(context, texts, &scores) || scores.size() != texts.size() ||
      std::isnan(scores[0]))
    return result;
  result.texts = texts;
  result.scores = scores;
  size_t best = 0;
  for (size_t i = 1; i < scores.size(); ++i) {
    if (!std::isnan(scores[i]))
      ++result.scored;
    if (!std::isnan(scores[i]) && scores[i] > scores[best])
      best = i;
  }
  ++result.scored;
  if (best == 0)
    return result;
  result.gain = scores[best] - scores[0];
  result.index = best;
  result.best = texts[best];
  if (result.gain > options.margin)
    result.text = texts[best];
  if (g_dev_console && g_dev_console->IsEnabled())
    g_dev_console->WriteLine(L"[LLM] 整句重排：" + texts[0] + L" → " + texts[best] + L"（第 " +
                             std::to_wstring(best + 1) + L" 句，改善 " +
                             std::to_wstring(result.gain) +
                             (result.text.empty() ? L"，未過門檻）" : L"）"));
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
