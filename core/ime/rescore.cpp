#include "rescore.h"

#include <algorithm>
#include <cmath>

#include "../base/devlog.h"
#include "../llm/LLMProvider.h"
#include "ZhuyinPreview.h"

namespace ime {

std::wstring RescoreSentence(LLMProvider* scorer,
                             const std::wstring& context,
                             const std::vector<std::wstring>& units,
                             const std::vector<std::vector<std::wstring>>& homophones,
                             double* gain,
                             const RescoreOptions& options) {
  // 只看完整的字：遇到還在拼的注音就停，後面照原樣接回去
  size_t n = 0;
  while (n < units.size() && units[n].size() >= 1 && units[n][0] >= 0x3400 &&
         !zhuyin_preview::IsBopomofo(units[n][0]))
    ++n;
  if (!scorer || n < 2 || options.positions == 0 || options.alternatives == 0 || LLMCancelled())
    return L"";
  std::vector<std::wstring> base(units.begin(), units.begin() + n);
  const std::wstring rest = zhuyin_preview::Join(units, n);
  double base_total = 0;
  std::vector<double> per_char;
  const std::wstring base_text = zhuyin_preview::Join(base);
  if (!scorer->ScoreText(context, base_text, &base_total, &per_char) ||
      !std::isfinite(base_total) || LLMCancelled())
    return L"";
  // 每個字的機率（per_char 一個 wchar_t 一個：UTF-16 的擴充字元佔兩個，取第一個）
  std::vector<std::pair<double, size_t>> order;
  for (size_t i = 0, w = 0; i < n; w += base[i].size(), ++i) {
    if (homophones.size() > i && homophones[i].size() > 1 && w < per_char.size() &&
        std::isfinite(per_char[w]))
      order.emplace_back(per_char[w], i);
  }
  std::sort(order.begin(), order.end());
  if (order.size() > options.positions)
    order.resize(options.positions);
  struct Change {
    double gain;
    size_t pos;
    std::wstring text;
  };
  struct Trial {
    size_t pos;
    size_t score_index;
    std::wstring text;
  };
  std::vector<Trial> trials;
  std::vector<std::wstring> texts;
  // 先收集單點替換，一次交給 ScoreBatch，讓本機模型重用前文、減少 IPC 往返。
  // 位置數與每個位置的候選數仍沿用原本上限，不縮小選字範圍。
  for (const auto& [lp, pos] : order) {
    size_t tried = 0;
    for (const auto& alt : homophones[pos]) {
      if (alt.empty() || alt == base[pos])
        continue;
      std::vector<std::wstring> variant = base;
      variant[pos] = alt;
      const std::wstring text = zhuyin_preview::Join(variant);
      if (std::find(texts.begin(), texts.end(), text) != texts.end())
        continue;
      if (++tried > options.alternatives)
        break;
      trials.push_back({pos, texts.size(), alt});
      texts.push_back(text);
    }
  }
  std::vector<double> totals;
  if (texts.empty() || LLMCancelled() || !scorer->ScoreBatch(context, texts, &totals) ||
      totals.size() != texts.size() || LLMCancelled())
    return L"";
  std::vector<Change> changes;
  for (const auto& [lp, pos] : order) {
    Change best{options.margin, pos, L""};
    for (const auto& trial : trials) {
      if (trial.pos != pos)
        continue;
      const double total = totals[trial.score_index];
      if (std::isfinite(total) && total - base_total > best.gain)
        best = {total - base_total, pos, trial.text};
    }
    if (!best.text.empty())
      changes.push_back(best);
    if (LLMCancelled())
      return L"";
  }
  if (changes.empty())
    return L"";
  // 由改善最多的開始逐一套用，每次都要比目前更好才留下
  std::sort(changes.begin(), changes.end(),
            [](const Change& a, const Change& b) { return a.gain > b.gain; });
  std::vector<std::wstring> current = base;
  double current_total = base_total;
  for (const auto& change : changes) {
    if (LLMCancelled())
      return L"";
    std::vector<std::wstring> variant = current;
    variant[change.pos] = change.text;
    double total = 0;
    if (current == base) {
      current = variant;
      current_total = base_total + change.gain;
      continue;
    }
    if (scorer->ScoreText(context, zhuyin_preview::Join(variant), &total, nullptr) &&
        std::isfinite(total) && total > current_total) {
      current = variant;
      current_total = total;
    }
  }
  if (LLMCancelled())
    return L"";
  if (g_dev_console && g_dev_console->IsEnabled())
    g_dev_console->WriteLine(L"[LLM] 推薦：" + base_text + L" → " + zhuyin_preview::Join(current) +
                             L"（改善 " + std::to_wstring(current_total - base_total) + L"）");
  if (gain)
    *gain = current_total - base_total;
  return zhuyin_preview::Join(current) + rest;
}

}  // namespace ime
