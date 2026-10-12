#include "rerank_tune.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>

#include <PersonalCrypto.h>

#include "../base/utf8.h"
#include "../ime/ZhuyinPreview.h"
#include "../llm/LLMProvider.h"

namespace settings_rerank {

namespace {

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::wstring Tail(const std::wstring& s, size_t chars) {
  return s.size() > chars ? s.substr(s.size() - chars) : s;
}

// 逗號分隔的數字欄
std::vector<double> Doubles(const std::wstring& s) {
  std::vector<double> out;
  for (const auto& part : Split(s, L',')) {
    if (!part.empty())
      out.push_back(std::wcstod(part.c_str(), nullptr));
  }
  return out;
}

constexpr size_t kMaxTop = 20;                          // 自動調整評分的句數上限（延遲隨句數線性增加）
const size_t kTops[] = {3, 5, 10, 15, 20};              // 「評幾句」的候選
const double kMargins[] = {0.5, 1, 2, 3, 4, 6, 8, 12};  // 門檻的候選

// 一種組合選出的句子：非第一句比第一句好超過 margin 才換；換不了就維持初稿。
// 計分比照線上的 RerankScore（長度正規化＋個人詞庫加分）；第一句不是預設轉換時不換（保守）
const std::wstring& Choose(const LoggedCase& c, const std::vector<double>& lm, size_t top,
                           double margin, double alpha, double personal_weight) {
  const size_t n = (std::min)(top, (std::min)(lm.size(), c.sentences.size()));
  const auto score = [&](size_t i) {
    double s = lm[i];
    if (!std::isfinite(s))
      return s;
    if (alpha > 0) {
      const double chars =
          (double)(std::max)((size_t)1, zhuyin_preview::SplitChars(c.sentences[i]).size());
      s /= std::pow(chars, alpha);
    }
    if (personal_weight > 0 && i < c.personal.size())
      s += personal_weight * c.personal[i];
    return s;
  };
  size_t best = 0;
  for (size_t i = 1; i < n; ++i)
    if (std::isfinite(score(i)) && (!std::isfinite(score(best)) || score(i) > score(best)))
      best = i;
  if (best && std::isfinite(score(0)) && score(best) - score(0) > margin &&
      c.sentences[0] == c.draft)
    return c.sentences[best];
  return c.draft;
}

}  // namespace

size_t LoadLoggedTopK(const std::filesystem::path& personal_dir, std::vector<LoggedCase>* cases) {
  size_t skipped = 0;
  std::ifstream in(personal_dir / "choice_log.dat", std::ios::binary);
  if (!in)
    return skipped;
  const std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  size_t pos = 0;
  // method=reverted 的列：那筆送出是錯的，等下一筆同注音的送出當修正後的答案
  struct Pending {
    LoggedCase c;
    size_t row = 0;
  };
  std::vector<Pending> pending;
  size_t row_no = 0;
  while (pos + 4 <= data.size()) {
    uint32_t len = 0;
    std::memcpy(&len, data.data() + pos, 4);
    pos += 4;
    if (pos + len > data.size())
      break;
    std::string plain;
    const bool ok = personal_crypto::Unprotect(data.substr(pos, len), &plain);
    pos += len;
    ++row_no;
    // 等太久（50 列）還沒修正的：正確答案不知道，丟掉
    for (auto it = pending.begin(); it != pending.end();) {
      if (row_no - it->row > 50) {
        ++skipped;
        it = pending.erase(it);
      } else {
        ++it;
      }
    }
    if (!ok) {
      ++skipped;
      continue;
    }
    // 時間 \t 應用程式 \t 方式 \t 前文 \t 注音 \t 預設轉換 \t 送出 \t 整句 \t 分數
    // 新格式再補：\t 個人詞庫 \t 建議 \t 套用 \t 前文key
    const auto f = Split(utf8::ToWide(plain), L'\t');
    if (f.size() < 8) {
      ++skipped;
      continue;
    }
    const bool reverted = f[2] == L"reverted";
    if (!reverted && !f[4].empty() && !f[6].empty()) {
      // 這筆同注音的送出可能就是前面 reverted 的修正答案（先來先配）
      for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (it->c.zhuyin == f[4]) {
          it->c.truth = f[6];
          cases->push_back(std::move(it->c));
          pending.erase(it);
          break;
        }
      }
    }
    if (!reverted && (f[2] == L"mixed" || f[6].empty() || f[7].empty())) {
      ++skipped;
      continue;
    }
    LoggedCase c;
    c.time = std::wcstoll(f[0].c_str(), nullptr, 10);
    c.method = utf8::FromWide(f[2]);
    c.context = f[3];
    c.zhuyin = f[4];
    c.draft = f[5];
    c.truth = f[6];
    c.sentences = Split(f[7], L'\x1E');
    if (f.size() > 9)
      c.personal = Doubles(f[9]);
    if (f.size() > 10)
      c.rec = utf8::FromWide(f[10]);
    if (f.size() > 11)
      c.took = utf8::FromWide(f[11]);
    if (f.size() > 12)
      c.context_key = f[12];
    if (c.sentences.size() < 2) {
      ++skipped;
      continue;
    }
    if (c.sentences.size() > kMaxTop)
      c.sentences.resize(kMaxTop);
    if (reverted)
      pending.push_back({std::move(c), row_no});
    else
      cases->push_back(std::move(c));
  }
  skipped += pending.size();
  return skipped;
}

TuneReport Tune(LLMProvider* scorer,
                const std::vector<LoggedCase>& cases,
                size_t context_chars,
                double length_alpha,
                double personal_weight,
                const std::atomic<bool>& cancel,
                const std::function<void(size_t, size_t)>& progress) {
  TuneReport report;
  // 每筆一次 ScoreBatch；評分失敗的整筆當「不換」算（同輸入法 fail-open）
  struct Item {
    const LoggedCase* c;
    std::vector<double> lm;
  };
  std::vector<Item> items;
  items.reserve(cases.size());
  size_t done = 0;
  for (const auto& c : cases) {
    if (cancel) {
      report.cancelled = true;
      break;
    }
    Item it{&c, {}};
    scorer->ScoreBatch(Tail(c.context, context_chars), c.sentences, &it.lm);
    if (it.lm.size() != c.sentences.size())
      it.lm.assign(c.sentences.size(), std::numeric_limits<double>::quiet_NaN());
    items.push_back(std::move(it));
    if (progress)
      progress(++done, cases.size());
  }
  report.cases = items.size();
  for (const auto& it : items)
    report.scored += it.lm.size();
  if (cancel || items.empty())
    return report;

  // 「評幾句」的網格：資料裡最大的句數以內的預設值，再加上句數本身（例如只有 8 句時加 8）
  size_t max_n = 0;
  for (const auto& it : items)
    max_n = (std::max)(max_n, it.lm.size());
  std::set<size_t> tops(std::begin(kTops), std::end(kTops));
  if (max_n >= 2 && max_n < kMaxTop)
    tops.insert(max_n);
  // 評一種組合；pick 與 truth 比較算對錯（同 TuneChoice：wrong 只算初稿本來對的題目）
  const auto eval = [&](size_t top, double margin, TuneOutcome* out) {
    size_t right = 0, base_right = 0, wrong = 0;
    for (const auto& it : items) {
      const std::wstring& pick =
          Choose(*it.c, it.lm, top, margin, length_alpha, personal_weight);
      right += pick == it.c->truth;
      if (it.c->draft == it.c->truth) {
        ++base_right;
        wrong += pick != it.c->truth;
      }
    }
    out->top = top;
    out->margin = margin;
    out->accuracy = items.empty() ? 0 : (double)right / items.size();
    out->wrong_change = base_right ? (double)wrong / base_right : 0;
  };

  bool have_best = false;
  for (size_t top : tops) {
    if (top < 2 || top > max_n)
      continue;
    for (double margin : kMargins) {
      TuneOutcome out;
      eval(top, margin, &out);
      // 準確率最高；同分取門檻較大（保守）、再取評較少句（快）
      const double acc = out.accuracy;
      if (!have_best || acc > report.best.accuracy + 1e-9 ||
          (acc > report.best.accuracy - 1e-9 &&
           (margin > report.best.margin || (margin == report.best.margin && top < report.best.top))))
        report.best = out;
      have_best = true;
      report.grid.push_back(out);
    }
  }
  return report;
}

}  // namespace settings_rerank
