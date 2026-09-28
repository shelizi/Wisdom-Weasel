// 推薦／整句校正的離線評估與調參：在測試集上用真正的本機模型跑，找出最好的搜尋參數、
// 校準的先驗與顯示門檻。
//
// 測試集（可以同時給）：
//   --log <choice_log.dat>
//   使用者的選字紀錄（加密，只在本機解開；只印統計數字，不印打字內容）
//   --cases <檔案>          公開的題目：每行「前文|注音|初稿|正確的句子」，#
//   開頭是註解（同模型測試頁）
// 每一題：初稿是 Rime 的預設轉換，正確的是使用者送出的句子。推薦／校正「對」＝
// 結果和正確的句子相同。
//
// 主要指標是「第一候選整句正確率」：顯示推薦就照推薦、否則照初稿，和正確的句子相同的比例。
// 顯示與否由校準過的採用機率與門檻決定，校準器用 5-fold
// 交叉驗證（在其他四份上擬合）。
//
//   TuneChoice <model.gguf> <rime_shared_dir> <work_dir> [--log f] [--cases f]
//   [--correct]
//              [--limit N] [--gpu N] [--base] [--dump f] [--only m,p,a]
//   --correct  也評估整句校正（每題要生成一次，比較慢）
//   --base     模型是 base（不是 instruct）
//   --dump     把每題的 gain 與對錯（只有數字）寫到檔案
//   --only m,p,a
//   只評估這一組搜尋參數（margin、位置、同音字），例如拿選字紀錄調出來的參數驗證公開題目
#include <rime_api.h>

#include "../../core/base/utf8.h"
#include "../../core/ime/ZhuyinPreview.h"
#include "../../core/ime/calibration.h"
#include "../../core/ime/rescore.h"
#include "../../core/ime/rime_helpers.h"
#include "../../core/ime/text_rules.h"
#include "../../core/llm/RemoteLLMProvider.h"

#include <PersonalCrypto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Strings = std::vector<std::wstring>;

namespace {

struct Case {
  std::wstring context, zhuyin, draft, truth;
  bool from_log = false;
  // 推薦用：每個音節的字與同音字
  Strings units;
  std::vector<Strings> homophones;
  bool usable = false;  // 音節數 = 初稿字數 = 正確字數
  bool fixable = false;  // 正確的句子每個字都在同音字裡（推薦有可能改對）
  std::string source =
      "public";  // log / public / 開放測試集的來源（wiki、cc100）
  std::string tags;  // 開放測試集的分類（A、D、F0/F10/F30）
  // Rime（沙盒，沒有選字記憶）打這串注音的結果
  std::wstring rime_draft;
  Strings sentences;  // 整句候選（字數和注音相同），依 Rime 的順序
  double rime_ms = 0;
};

// ---------------------------------------------------------------------------
// 讀題目

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

std::wstring Trim(std::wstring s) {
  while (!s.empty() &&
         (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\n'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && s[i] == L' ')
    ++i;
  return s.substr(i);
}

bool LoadLog(const fs::path& file, std::vector<Case>* cases, size_t* skipped) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    return false;
  const std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  size_t pos = 0;
  while (pos + 4 <= data.size()) {
    uint32_t len = 0;
    std::memcpy(&len, data.data() + pos, 4);
    pos += 4;
    if (pos + len > data.size())
      break;
    std::string plain;
    const bool ok = personal_crypto::Unprotect(data.substr(pos, len), &plain);
    pos += len;
    if (!ok) {
      ++*skipped;
      continue;
    }
    // 時間 \t 應用程式 \t 方式 \t 前文 \t 注音 \t 預設轉換 \t 送出的文字
    const auto f = Split(utf8::ToWide(plain), L'\t');
    if (f.size() < 7 || f[2] == L"mixed" || f[4].empty() || f[5].empty() ||
        f[6].empty()) {
      ++*skipped;
      continue;
    }
    Case c;
    c.context = f[3];
    c.zhuyin = Trim(f[4]);
    c.draft = f[5];
    c.truth = f[6];
    c.from_log = true;
    c.source = "log";
    cases->push_back(std::move(c));
  }
  return true;
}

bool LoadCases(const fs::path& file, std::vector<Case>* cases) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    return false;
  for (std::string line; std::getline(in, line);) {
    const std::wstring w = Trim(utf8::ToWide(line));
    if (w.empty() || w[0] == L'#')
      continue;
    const auto f = Split(w, L'|');
    if (f.size() < 4)
      continue;
    Case c;
    c.context = f[0];
    c.zhuyin = Trim(f[1]);
    c.draft = Trim(f[2]);
    c.truth = Trim(f[3]);
    if (f.size() > 4)
      c.tags = utf8::FromWide(Trim(f[4]));
    if (f.size() > 5)
      c.source = utf8::FromWide(Trim(f[5]));
    cases->push_back(std::move(c));
  }
  return true;
}

// 注音（輸入法記錄的格式：聲調之後、一聲用空白斷開）→
// 每個音節的大千按鍵（一聲補空白）
std::vector<std::string> SyllableKeys(const std::wstring& zhuyin) {
  static const wchar_t kSymbols[] =
      L"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤ"
      L"ㄥㄦ";
  static const char kKeys[] = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-";
  static const wchar_t kTones[] = L"ˊˇˋ˙";
  static const char kToneKeys[] = "6347";
  std::vector<std::string> out;
  std::string cur;
  for (wchar_t c : zhuyin) {
    if (const wchar_t* s = std::wcschr(kSymbols, c)) {
      cur += kKeys[s - kSymbols];
    } else if (const wchar_t* t = std::wcschr(kTones, c)) {
      if (!cur.empty())
        out.push_back(cur + kToneKeys[t - kTones]);
      cur.clear();
    } else if (c == L' ') {
      if (!cur.empty())
        out.push_back(cur + ' ');
      cur.clear();
    } else {
      return {};  // 其他字元（英文、標點）：這題不用
    }
  }
  if (!cur.empty())
    out.push_back(cur + ' ');
  return out;
}

// 在 Rime 打一整串按鍵：初稿（commit preview）與字數相同的整句候選（最多 20
// 個）
void Convert(RimeApi* api,
             RimeSessionId session,
             const std::string& input,
             size_t chars,
             Case* c) {
  const auto t0 = std::chrono::steady_clock::now();
  api->set_input(session, input.c_str());
  RIME_STRUCT(RimeContext, ctx);
  if (api->get_context(session, &ctx)) {
    if (ctx.commit_text_preview) {
      c->rime_draft = utf8::ToWide(ctx.commit_text_preview);
      while (!c->rime_draft.empty() && c->rime_draft.back() < 0x80)
        c->rime_draft.pop_back();
    }
    api->free_context(&ctx);
  }
  RimeCandidateListIterator iter = {0};
  if (api->candidate_list_begin(session, &iter)) {
    for (int i = 0;
         i < 100 && c->sentences.size() < 20 && api->candidate_list_next(&iter);
         ++i) {
      if (!iter.candidate.text)
        continue;
      const std::wstring text = utf8::ToWide(iter.candidate.text);
      if (zhuyin_preview::SplitChars(text).size() == chars &&
          std::find(c->sentences.begin(), c->sentences.end(), text) ==
              c->sentences.end())
        c->sentences.push_back(text);
    }
    api->candidate_list_end(&iter);
  }
  c->rime_ms = std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
  api->clear_composition(session);
}

double Percentile(std::vector<double> v, double q) {
  if (v.empty())
    return NAN;
  std::sort(v.begin(), v.end());
  return v[(size_t)std::min<double>(v.size() - 1, q * (v.size() - 1) + 0.5)];
}

// 分組的鍵：來源，以及開放測試集的每個標籤
std::vector<std::string> Groups(const Case& c) {
  std::vector<std::string> g = {"all", "src:" + c.source};
  std::stringstream ss(c.tags);
  for (std::string t; std::getline(ss, t, ',');)
    if (!t.empty())
      g.push_back("tag:" + t);
  return g;
}

// P0.5：只看 Rime（沙盒）。Top-1、oracle@K、MRR、延遲，依來源與標籤分開
void ReportRime(const std::vector<Case>& cases) {
  struct Sum {
    size_t n = 0, top1 = 0, at3 = 0, at5 = 0, at10 = 0, unreachable = 0;
    size_t user_draft = 0, user_n = 0;
    double mrr = 0;
    std::vector<double> ms;
  };
  std::map<std::string, Sum> sums;
  for (const auto& c : cases) {
    if (c.sentences.empty() && c.rime_draft.empty())
      continue;
    size_t rank = 0;
    for (size_t i = 0; i < c.sentences.size(); ++i)
      if (c.sentences[i] == c.truth) {
        rank = i + 1;
        break;
      }
    for (const auto& g : Groups(c)) {
      Sum& s = sums[g];
      ++s.n;
      s.top1 += c.rime_draft == c.truth ? 1 : 0;
      s.at3 += rank && rank <= 3 ? 1 : 0;
      s.at5 += rank && rank <= 5 ? 1 : 0;
      s.at10 += rank && rank <= 10 ? 1 : 0;
      s.mrr += rank ? 1.0 / rank : 0;
      s.unreachable +=
          c.usable && !c.fixable && c.rime_draft != c.truth ? 1 : 0;
      if (c.from_log) {
        ++s.user_n;
        s.user_draft += c.draft == c.truth ? 1 : 0;
      }
      s.ms.push_back(c.rime_ms);
    }
  }
  std::printf(
      "\nRime（沙盒，沒有選字記憶）：\n"
      "  %-12s %6s %7s %7s %7s %7s %6s %9s %8s %8s\n",
      "分組", "題數", "Top-1", "@3", "@5", "@10", "MRR", "讀音存疑", "P50 ms",
      "P95 ms");
  for (const auto& [g, s] : sums) {
    std::printf("  %-12s %6zu %7.4f %7.4f %7.4f %7.4f %6.3f %9zu %8.2f %8.2f",
                g.c_str(), s.n, (double)s.top1 / s.n, (double)s.at3 / s.n,
                (double)s.at5 / s.n, (double)s.at10 / s.n, s.mrr / s.n,
                s.unreachable, Percentile(s.ms, 0.5), Percentile(s.ms, 0.95));
    if (s.user_n)
      std::printf("（使用者自己的 Rime 初稿 %.4f）",
                  (double)s.user_draft / s.user_n);
    std::printf("\n");
  }
}

// ---------------------------------------------------------------------------
// 評分快取：網格搜尋的各組參數大多重複評同樣的句子

class CachedScorer : public LLMProvider {
 public:
  explicit CachedScorer(LLMProvider* inner) : inner_(inner) {}
  size_t calls = 0, hits = 0;

  bool LoadConfig(const std::string&) override { return true; }
  std::vector<std::wstring> PredictCandidates(const std::wstring&,
                                              const std::wstring&,
                                              size_t) override {
    return {};
  }
  bool IsAvailable() const override { return true; }
  std::string GetProviderName() const override { return "cache"; }
  bool ScoreText(const std::wstring& context,
                 const std::wstring& text,
                 double* total,
                 std::vector<double>* per_char) override {
    const std::wstring key = context + L'\x1f' + text;
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      ++calls;
      Entry e;
      e.ok = inner_->ScoreText(context, text, &e.total, &e.per_char);
      it = cache_.emplace(key, std::move(e)).first;
    } else {
      ++hits;
    }
    if (!it->second.ok)
      return false;
    *total = it->second.total;
    if (per_char)
      *per_char = it->second.per_char;
    return true;
  }

 private:
  struct Entry {
    bool ok = false;
    double total = 0;
    std::vector<double> per_char;
  };
  LLMProvider* inner_;
  std::map<std::wstring, Entry> cache_;
};

std::wstring Tail(const std::wstring& s, size_t n) {
  return s.size() > n ? s.substr(s.size() - n) : s;
}

// ---------------------------------------------------------------------------
// 評估

// 一題的結果
struct Outcome {
  bool offered = false;  // 有推薦（或校正）
  double gain = 0;
  bool right = false;        // 推薦／校正和正確的句子相同
  bool draft_right = false;  // 初稿本來就對
  const Case* c = nullptr;
};

uint64_t Mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return x;
}

struct Score {
  double accuracy = 0;  // 第一候選整句正確率
  double log_loss =
      0;  // 有推薦的題目上，校準機率的平均 log loss（out-of-fold）
  double ece = 0;
  size_t shown = 0, shown_right = 0, hidden_right = 0;
  // wrong-change：初稿本來就對，卻顯示了（錯的）推薦
  size_t draft_right = 0, wrong_changes = 0;
};

// 5-fold：在其他四份上擬合校準器，對這一份決定顯示與否；p
// 還沒校準（NaN）時照舊顯示
Score CrossValidate(const std::vector<Outcome>& outcomes,
                    const ime::CalibrationPrior& prior,
                    double threshold,
                    int folds = 5) {
  Score s;
  const size_t n = outcomes.size();
  if (!n)
    return s;
  std::vector<int> fold(n);
  for (size_t i = 0; i < n; ++i)
    fold[i] = (int)(Mix(i * 2654435761ULL + 17) % folds);
  size_t right = 0, scored = 0;
  const int kBins = 10;
  double sum_p[kBins] = {0}, sum_y[kBins] = {0};
  for (int f = 0; f < folds; ++f) {
    ime::Calibrator cal(prior);
    for (size_t i = 0; i < n; ++i)
      if (fold[i] != f && outcomes[i].offered)
        cal.Add(outcomes[i].gain, outcomes[i].right, false);
    cal.Fit();
    for (size_t i = 0; i < n; ++i) {
      if (fold[i] != f)
        continue;
      const Outcome& o = outcomes[i];
      bool show = o.offered;
      if (o.offered) {
        const double p = cal.Probability(o.gain);
        if (ime::HasConfidence(p)) {
          show = p >= threshold;
          const double q = (std::min)(1 - 1e-6, (std::max)(1e-6, p));
          s.log_loss -= std::log(o.right ? q : 1 - q);
          const int bin = (std::min)(kBins - 1, (int)(p * kBins));
          sum_p[bin] += p;
          sum_y[bin] += o.right ? 1 : 0;
          ++scored;
        }
      }
      if (o.draft_right) {
        ++s.draft_right;
        s.wrong_changes += show && !o.right ? 1 : 0;
      }
      if (show) {
        ++s.shown;
        s.shown_right += o.right ? 1 : 0;
      } else if (o.offered && o.right) {
        ++s.hidden_right;
      }
      right += (show ? o.right : o.draft_right) ? 1 : 0;
    }
  }
  s.accuracy = (double)right / n;
  if (scored) {
    s.log_loss /= scored;
    for (int b = 0; b < kBins; ++b)
      s.ece += std::abs(sum_p[b] - sum_y[b]) / scored;
  } else {
    s.log_loss = s.ece = NAN;
  }
  return s;
}

struct Tuned {
  ime::CalibrationPrior prior;
  double threshold = 0;
  Score score;
};

// 先驗用 log loss 選（proper scoring rule），門檻用第一候選正確率選
Tuned TuneCalibration(const std::vector<Outcome>& outcomes) {
  Tuned best;
  double best_loss = INFINITY;
  for (double slope : {0.5, 1.0, 2.0})
    for (double sw : {0.1, 0.3, 1.0, 3.0})
      for (double bw : {0.001, 0.01, 0.1}) {
        ime::CalibrationPrior prior;
        prior.slope = slope;
        prior.slope_weight = sw;
        prior.bias_weight = bw;
        prior.min_samples = 1;  // 交叉驗證的訓練集夠大，這裡不擋
        const Score s = CrossValidate(outcomes, prior, 0);
        if (std::isfinite(s.log_loss) && s.log_loss < best_loss) {
          best_loss = s.log_loss;
          best.prior = prior;
        }
      }
  best.score = CrossValidate(outcomes, best.prior, 0);
  for (int t = 1; t <= 18; ++t) {
    const Score s = CrossValidate(outcomes, best.prior, t * 0.05);
    if (s.accuracy > best.score.accuracy + 1e-12) {
      best.threshold = t * 0.05;
      best.score = s;
    }
  }
  return best;
}

double Baseline(const std::vector<Outcome>& outcomes, bool show_all) {
  size_t right = 0;
  for (const auto& o : outcomes)
    right += ((show_all && o.offered) ? o.right : o.draft_right) ? 1 : 0;
  return outcomes.empty() ? 0 : (double)right / outcomes.size();
}

// 校準要多少樣本才開始用：隨機抽 n
// 筆擬合，在其餘的題目上比較「用門檻」和「全顯示」
void LearningCurve(const std::vector<Outcome>& outcomes, const Tuned& tuned) {
  std::vector<size_t> offered;
  for (size_t i = 0; i < outcomes.size(); ++i)
    if (outcomes[i].offered)
      offered.push_back(i);
  std::printf(
      "\n  校準樣本數 → 在其餘題目上的第一候選正確率（門檻 %.2f；全顯示 = "
      "不校準）\n",
      tuned.threshold);
  for (size_t n : {5, 10, 20, 30, 50, 100, 200, 400}) {
    if (n * 2 > offered.size())
      break;
    double gated = 0, all = 0;
    const int kDraws = 30;
    for (int d = 0; d < kDraws; ++d) {
      std::vector<size_t> pick = offered;
      for (size_t i = 0; i < pick.size(); ++i)
        std::swap(pick[i],
                  pick[i + Mix(d * 1000003ULL + i) % (pick.size() - i)]);
      pick.resize(n);
      std::vector<bool> train(outcomes.size(), false);
      ime::CalibrationPrior prior = tuned.prior;
      prior.min_samples = 1;
      ime::Calibrator cal(prior);
      for (size_t i : pick) {
        train[i] = true;
        cal.Add(outcomes[i].gain, outcomes[i].right, false);
      }
      cal.Fit();
      size_t right_g = 0, right_a = 0, total = 0;
      for (size_t i = 0; i < outcomes.size(); ++i) {
        if (train[i])
          continue;
        const Outcome& o = outcomes[i];
        const double p = o.offered ? cal.Probability(o.gain) : NAN;
        const bool show =
            o.offered && (!ime::HasConfidence(p) || p >= tuned.threshold);
        right_g += (show ? o.right : o.draft_right) ? 1 : 0;
        right_a += ((o.offered ? o.right : o.draft_right)) ? 1 : 0;
        ++total;
      }
      gated += (double)right_g / total;
      all += (double)right_a / total;
    }
    std::printf("    n = %3zu：用門檻 %.4f、全顯示 %.4f\n", n, gated / kDraws,
                all / kDraws);
  }
}

void PrintScore(const char* name, const Score& s, size_t n) {
  std::printf(
      "  %s：第一候選正確率 %.4f（%zu 題），顯示 %zu 次、其中對 "
      "%zu、藏掉的對的 %zu，wrong-change %.4f，"
      "log loss %.3f、ECE %.3f\n",
      name, s.accuracy, n, s.shown, s.shown_right, s.hidden_right,
      s.draft_right ? (double)s.wrong_changes / s.draft_right : 0.0, s.log_loss,
      s.ece);
}

// P3 評估：LM 重排 Rime 的 Top-K 整句。比 Rime 第一句通順超過 margin 才換；
// margin 用 2-fold 選（一半選、另一半報），避免在同一份資料上選又報
void ReportTopK(const std::vector<Case>& cases, LLMProvider* scorer) {
  struct Item {
    const Case* c;
    std::vector<double> lm;
  };
  std::vector<Item> items;
  std::vector<double> ms;
  size_t scored = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& c : cases) {
    if (c.sentences.empty() || c.rime_draft.empty())
      continue;
    Item it{&c, {}};
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < c.sentences.size() && i < 10; ++i) {
      double total = NAN;
      if (!scorer->ScoreText(Tail(c.context, 30), c.sentences[i], &total,
                             nullptr))
        total = NAN;
      it.lm.push_back(total);
      ++scored;
    }
    ms.push_back(std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0)
                     .count());
    items.push_back(std::move(it));
    if (items.size() % 500 == 0)
      std::printf("  重排 %zu 題…\n", items.size());
  }
  std::printf("重排評分 %zu 次，%lld 秒\n", scored,
              (long long)std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
  const auto choose = [](const Item& it, double margin) -> const std::wstring& {
    size_t best = 0;
    for (size_t i = 1; i < it.lm.size(); ++i)
      if (std::isfinite(it.lm[i]) &&
          (!std::isfinite(it.lm[best]) || it.lm[i] > it.lm[best]))
        best = i;
    if (best && std::isfinite(it.lm[0]) && it.lm[best] - it.lm[0] > margin &&
        it.c->sentences[0] == it.c->rime_draft)
      return it.c->sentences[best];
    return it.c->rime_draft;
  };
  struct Eval {
    size_t n = 0, right = 0, base_right = 0, wrong = 0;
    double Acc() const { return n ? (double)right / n : 0; }
    double Wrong() const { return base_right ? (double)wrong / base_right : 0; }
  };
  const auto eval = [&](double margin, int fold, const std::string& group) {
    Eval e;
    for (size_t i = 0; i < items.size(); ++i) {
      if (fold >= 0 && (int)(Mix(i * 7919 + 3) % 2) != fold)
        continue;
      const Item& it = items[i];
      const auto g = Groups(*it.c);
      if (std::find(g.begin(), g.end(), group) == g.end())
        continue;
      const std::wstring& pick = choose(it, margin);
      ++e.n;
      e.right += pick == it.c->truth ? 1 : 0;
      if (it.c->rime_draft == it.c->truth) {
        ++e.base_right;
        e.wrong += pick != it.c->truth ? 1 : 0;
      }
    }
    return e;
  };
  const double kMargins[] = {0, 0.5, 1, 2, 3, 4, 6, 8, 12, 1e9};
  std::printf("\n  margin（全部）：");
  for (double m : kMargins) {
    const Eval e = eval(m, -1, "all");
    std::printf(" %s→%.4f/%.4f",
                m >= 1e9 ? "不換" : std::to_string(m).substr(0, 4).c_str(),
                e.Acc(), e.Wrong());
  }
  std::printf("（Top-1 / wrong-change）\n");
  // 2-fold：各自在另一半選 margin
  double picked[2] = {0, 0};
  for (int f = 0; f < 2; ++f) {
    double best = -1;
    for (double m : kMargins) {
      const double acc = eval(m, 1 - f, "all").Acc();
      if (acc > best) {
        best = acc;
        picked[f] = m;
      }
    }
  }
  std::printf("  2-fold 選出的 margin：%.1f、%.1f\n", picked[0], picked[1]);
  std::set<std::string> groups;
  for (const auto& it : items)
    for (const auto& g : Groups(*it.c))
      groups.insert(g);
  std::printf("  %-12s %6s %8s %8s %9s %10s %13s\n", "分組", "題數", "Rime",
              "@10", "LM 直接", "LM+margin", "wrong-change");
  for (const auto& g : groups) {
    Eval base = eval(1e9, -1, g), raw = eval(0, -1, g);
    Eval cv;
    for (int f = 0; f < 2; ++f) {
      const Eval e = eval(picked[f], f, g);
      cv.n += e.n;
      cv.right += e.right;
      cv.base_right += e.base_right;
      cv.wrong += e.wrong;
    }
    size_t at10 = 0;
    for (const auto& it : items) {
      const auto gg = Groups(*it.c);
      if (std::find(gg.begin(), gg.end(), g) == gg.end())
        continue;
      at10 += std::find(it.c->sentences.begin(), it.c->sentences.end(),
                        it.c->truth) != it.c->sentences.end()
                  ? 1
                  : 0;
    }
    std::printf("  %-12s %6zu %8.4f %8.4f %9.4f %10.4f %13.4f\n", g.c_str(),
                base.n, base.Acc(), base.n ? (double)at10 / base.n : 0,
                raw.Acc(), cv.Acc(), cv.Wrong());
  }
  std::printf(
      "  延遲（每題逐句評分，未批次）：P50 %.0f ms、P95 %.0f ms、P99 %.0f ms\n",
      Percentile(ms, 0.5), Percentile(ms, 0.95), Percentile(ms, 0.99));
}

// 全顯示時的分組結果：只用初稿、全顯示、wrong-change
void ReportGroups(const std::vector<Outcome>& outcomes) {
  struct Sum {
    size_t n = 0, draft = 0, shown_all = 0, draft_right = 0, wrong = 0;
  };
  std::map<std::string, Sum> sums;
  for (const auto& o : outcomes)
    for (const auto& g : Groups(*o.c)) {
      Sum& s = sums[g];
      ++s.n;
      s.draft += o.draft_right ? 1 : 0;
      s.shown_all += (o.offered ? o.right : o.draft_right) ? 1 : 0;
      if (o.draft_right) {
        ++s.draft_right;
        s.wrong += o.offered && !o.right ? 1 : 0;
      }
    }
  std::printf("  %-12s %6s %8s %8s %13s\n", "分組", "題數", "只用初稿",
              "全顯示", "wrong-change");
  for (const auto& [g, s] : sums)
    std::printf("  %-12s %6zu %8.4f %8.4f %13.4f\n", g.c_str(), s.n,
                (double)s.draft / s.n, (double)s.shown_all / s.n,
                s.draft_right ? (double)s.wrong / s.draft_right : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // 導到檔案時也馬上看得到進度
  if (argc < 4) {
    std::printf(
        "usage: TuneChoice <model.gguf> <rime_shared_dir> <work_dir> [--log f] "
        "[--cases f] [--correct]\n"
        "                  [--limit N] [--gpu N] [--base] [--dump f] [--only "
        "m,p,a]\n");
    return 2;
  }
  const std::string model = argv[1], shared = argv[2];
  const fs::path work = argv[3];
  std::string log_file, cases_file, dump_file;
  bool correct = false, base = false;
  size_t limit = 0;
  int gpu = -1;
  bool only = false;
  ime::RescoreOptions only_options;
  std::string grammar;  // octagram 語言模型檔（.gram），空白 = 不用
  int max_sentences = 0;   // librime 1.17 translator/max_sentences，0 = 預設
  bool rime_only = false;  // 只評估 Rime（P0.5），不載入模型
  size_t latency = 0;      // 量推薦延遲的題數（不用快取）
  bool topk_rerank = false;  // P3 評估：LM 重排 Rime 的 Top-K 整句
  std::string dump_topk;  // 開放測試集的 Rime Top-K 寫到檔案（給 teacher
                          // 試跑；不含選字紀錄）
  for (int i = 4; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&] {
      return i + 1 < argc ? std::string(argv[++i]) : std::string();
    };
    if (a == "--log")
      log_file = next();
    else if (a == "--cases")
      cases_file = next();
    else if (a == "--dump")
      dump_file = next();
    else if (a == "--correct")
      correct = true;
    else if (a == "--base")
      base = true;
    else if (a == "--limit")
      limit = (size_t)std::atoll(next().c_str());
    else if (a == "--gpu")
      gpu = std::atoi(next().c_str());
    else if (a == "--grammar")
      grammar = next();
    else if (a == "--max-sentences")
      max_sentences = std::atoi(next().c_str());
    else if (a == "--rime-only")
      rime_only = true;
    else if (a == "--topk-rerank")
      topk_rerank = true;
    else if (a == "--dump-topk")
      dump_topk = next();
    else if (a == "--latency")
      latency = (size_t)std::atoll(next().c_str());
    else if (a == "--only") {
      only =
          std::sscanf(next().c_str(), "%lf,%zu,%zu", &only_options.margin,
                      &only_options.positions, &only_options.alternatives) == 3;
    }
  }

  std::vector<Case> cases;
  size_t skipped = 0;
  if (!log_file.empty() && !LoadLog(log_file, &cases, &skipped)) {
    std::printf("FAIL cannot read %s\n", log_file.c_str());
    return 1;
  }
  const size_t log_count = cases.size();
  if (!cases_file.empty() && !LoadCases(cases_file, &cases)) {
    std::printf("FAIL cannot read %s\n", cases_file.c_str());
    return 1;
  }
  if (limit && cases.size() > limit)
    cases.resize(limit);
  std::printf(
      "題目：選字紀錄 %zu 題（略過 %zu 筆：混打、沒有注音或解不開），公開題目 "
      "%zu 題\n",
      log_count, skipped, cases.size() - log_count);

  // Rime：只部署注音方案，查同音字
  std::string variant =
      grammar.empty() ? "plain" : fs::path(grammar).stem().string();
  if (max_sentences > 0)
    variant += "-k" + std::to_string(max_sentences);
  const fs::path user = work / ("user-" + variant);
  fs::create_directories(user);
  {
    std::ofstream out(user / "default.custom.yaml", std::ios::binary);
    out << "patch:\n  schema_list:\n    - schema: bopomofo\n";
  }
  {
    // 和設定程式加 octagram 的方式相同（core/settings/ops.cpp
    // PatchSchemaGrammar）
    std::ofstream out(user / "bopomofo.custom.yaml", std::ios::binary);
    out << "patch:\n";
    if (!grammar.empty()) {
      std::error_code ec;
      fs::copy_file(grammar, user / fs::path(grammar).filename(),
                    fs::copy_options::overwrite_existing, ec);
      out << "  grammar:\n    language: " << fs::path(grammar).stem().string()
          << "\n  translator/contextual_suggestions: true\n"
             "  translator/max_homophones: 7\n  translator/max_homographs: 7\n";
    }
    if (max_sentences > 0)
      out << "  translator/max_sentences: " << max_sentences << "\n";
  }
  RimeApi* api = rime_get_api();
  const std::string user_dir = user.string();
  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = shared.c_str();
  traits.user_data_dir = user_dir.c_str();
  traits.prebuilt_data_dir = shared.c_str();
  traits.app_name = "rime.tune_choice";
  traits.distribution_name = "TuneChoice";
  traits.distribution_code_name = "tune";
  traits.distribution_version = "0";
  api->setup(&traits);
  api->initialize(nullptr);
  if (api->start_maintenance(False))
    api->join_maintenance_thread();
  ime::HomophoneFinder finder(api);
  std::printf("librime %s，設定：%s\n",
              api->get_version ? api->get_version() : "?", variant.c_str());
  // 每一題打進 Rime：初稿空白的題目（開放測試集）用 Rime 的初稿
  {
    const RimeSessionId session = api->create_session();
    api->select_schema(session, "bopomofo");
    for (auto& c : cases) {
      const auto keys = SyllableKeys(c.zhuyin);
      if (keys.empty())
        continue;
      std::string input;
      for (const auto& k : keys)
        input += k;
      Convert(api, session, input, keys.size(), &c);
      if (c.draft.empty())
        c.draft = c.rime_draft;
    }
    api->destroy_session(session);
  }

  size_t usable = 0, fixable = 0, draft_right = 0;
  size_t public_bad = 0;
  for (auto& c : cases) {
    const Strings draft = zhuyin_preview::SplitChars(c.draft);
    const Strings truth = zhuyin_preview::SplitChars(c.truth);
    const auto keys = SyllableKeys(c.zhuyin);
    if (keys.empty() || keys.size() != draft.size() ||
        draft.size() != truth.size() || draft.size() < 2) {
      if (!c.from_log)
        ++public_bad;
      continue;
    }
    c.units = draft;
    c.fixable = true;
    for (size_t i = 0; i < keys.size(); ++i) {
      c.homophones.push_back(finder.Find("bopomofo", keys[i]));
      const Strings& h = c.homophones.back();
      if (truth[i] != draft[i] &&
          std::find(h.begin(), h.end(), truth[i]) == h.end())
        c.fixable = false;
    }
    // 公開題目的正確答案應該查得到：查不到多半是注音寫錯
    if (!c.from_log && !c.fixable) {
      ++public_bad;
      std::printf("  公開題目的注音或同音字對不上：%s\n",
                  utf8::FromWide(c.truth).c_str());
    }
    c.usable = true;
    ++usable;
    fixable += c.fixable && c.draft != c.truth ? 1 : 0;
    draft_right += c.draft == c.truth ? 1 : 0;
  }
  std::printf(
      "可用於推薦：%zu 題（初稿本來就對 %zu、有錯而且同音字改得對 "
      "%zu），公開題目有問題 %zu 題\n",
      usable, draft_right, fixable, public_bad);

  if (!dump_topk.empty()) {
    // 每行：來源 \t 前文 \t 注音 \t 正確 \t Rime 初稿 \t 整句候選（以 |
    // 分隔）。選字紀錄的題目不寫
    std::ofstream out(dump_topk, std::ios::binary);
    size_t n = 0;
    for (const auto& c : cases) {
      if (c.from_log || c.sentences.empty())
        continue;
      std::wstring joined;
      for (const auto& t : c.sentences)
        joined += (joined.empty() ? L"" : L"|") + t;
      out << c.source << '\t' << utf8::FromWide(c.context) << '\t'
          << utf8::FromWide(c.zhuyin) << '\t' << utf8::FromWide(c.truth) << '\t'
          << utf8::FromWide(c.rime_draft) << '\t' << utf8::FromWide(joined)
          << '\n';
      ++n;
    }
    std::printf("寫出 %zu 題的 Top-K → %s\n", n, dump_topk.c_str());
  }
  if (rime_only) {
    ReportRime(cases);
    api->finalize();
    return 0;
  }

  // 本機模型
  LLMLocalModelSpec spec;
  spec.model_path = model;
  spec.n_ctx = 2048;
  spec.n_gpu_layers = gpu;
  spec.instruct = !base;
  spec.disable_thinking = true;
  RemoteLLMProvider provider("llamacpp");
  auto start = std::chrono::steady_clock::now();
  if (!provider.LoadModelDirect(spec, 0.0) || !provider.IsAvailable()) {
    std::printf("FAIL model not loaded: %s\n", model.c_str());
    return 1;
  }
  CachedScorer scorer(&provider);
  if (topk_rerank) {
    ReportRime(cases);
    ReportTopK(cases, &scorer);
    api->finalize();
    return 0;
  }

  // 推薦：網格搜尋（先跑範圍最大的一組，之後大多是快取）
  struct Config {
    ime::RescoreOptions options;
    std::vector<Outcome> outcomes;
    Tuned tuned;
  };
  std::vector<Config> configs;
  if (only) {
    Config c;
    c.options = only_options;
    configs.push_back(c);
  }
  for (size_t positions : {8, 6, 4, 3, 2})
    for (size_t alternatives : {6, 5, 3})
      for (double margin : {0.0, 0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0}) {
        if (only)
          break;
        Config c;
        c.options.positions = positions;
        c.options.alternatives = alternatives;
        c.options.margin = margin;
        configs.push_back(c);
      }
  size_t done = 0;
  for (auto& config : configs) {
    for (const auto& c : cases) {
      if (!c.usable)
        continue;
      Outcome o;
      double gain = NAN;
      const std::wstring rec =
          ime::RescoreSentence(&scorer, Tail(c.context, 30), c.units,
                               c.homophones, &gain, config.options);
      o.offered = !rec.empty() && rec != c.draft;
      o.gain = gain;
      o.right = o.offered && rec == c.truth;
      o.draft_right = c.draft == c.truth;
      o.c = &c;
      config.outcomes.push_back(o);
    }
    if (++done == 1)
      std::printf("第一組（最大範圍）評分 %zu 次，%lld 秒\n", scorer.calls,
                  (long long)std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - start)
                      .count());
    config.tuned = TuneCalibration(config.outcomes);
  }
  std::printf("評分共 %zu 次（快取命中 %zu 次）\n", scorer.calls, scorer.hits);

  const std::vector<Outcome>& any = configs.front().outcomes;
  std::printf("\n推薦（%zu 題）：只用初稿 %.4f、改得對的上限 %.4f\n",
              any.size(), Baseline(any, false), [&] {
                size_t r = 0;
                for (const auto& c : cases)
                  r += c.usable && (c.draft == c.truth || c.fixable) ? 1 : 0;
                return any.empty() ? 0.0 : (double)r / any.size();
              }());
  // 目前的預設（RescoreOptions()）
  auto find = [&](double margin, size_t positions,
                  size_t alternatives) -> Config* {
    for (auto& c : configs)
      if (c.options.margin == margin && c.options.positions == positions &&
          c.options.alternatives == alternatives)
        return &c;
    return nullptr;
  };
  const ime::RescoreOptions defaults;
  if (Config* current =
          find(defaults.margin, defaults.positions, defaults.alternatives)) {
    std::printf(
        "  目前的預設（margin %.1f、位置 %zu、同音字 %zu）全顯示：%.4f\n",
        defaults.margin, defaults.positions, defaults.alternatives,
        Baseline(current->outcomes, true));
    ime::CalibrationPrior prior;
    prior.min_samples = 1;
    PrintScore("目前的預設 + 目前的校準（門檻 0.5）",
               CrossValidate(current->outcomes, prior, 0.5), any.size());
    ReportGroups(current->outcomes);
  }
  // 還沒校準時（樣本不夠）是全顯示：這時最好的參數
  std::sort(configs.begin(), configs.end(),
            [](const Config& a, const Config& b) {
              return Baseline(a.outcomes, true) > Baseline(b.outcomes, true);
            });
  std::printf("\n  還沒校準（全顯示）最好的 10 組：\n");
  for (size_t i = 0; i < configs.size() && i < 10; ++i) {
    const Config& c = configs[i];
    size_t shown = 0, right = 0;
    for (const auto& o : c.outcomes) {
      shown += o.offered ? 1 : 0;
      right += o.offered && o.right ? 1 : 0;
    }
    std::printf(
        "    margin %.2f、位置 %zu、同音字 %zu：%.4f（推薦 %zu 次、對 %zu）\n",
        c.options.margin, c.options.positions, c.options.alternatives,
        Baseline(c.outcomes, true), shown, right);
  }
  std::sort(configs.begin(), configs.end(),
            [](const Config& a, const Config& b) {
              return a.tuned.score.accuracy > b.tuned.score.accuracy;
            });
  std::printf("\n  最好的 10 組：\n");
  for (size_t i = 0; i < configs.size() && i < 10; ++i) {
    const Config& c = configs[i];
    std::printf(
        "    margin %.2f、位置 %zu、同音字 %zu、門檻 %.2f、先驗 a=%.1f(w %.1f) "
        "b(w %.3f)：%.4f"
        "（全顯示 %.4f，顯示 %zu、對 %zu）\n",
        c.options.margin, c.options.positions, c.options.alternatives,
        c.tuned.threshold, c.tuned.prior.slope, c.tuned.prior.slope_weight,
        c.tuned.prior.bias_weight, c.tuned.score.accuracy,
        Baseline(c.outcomes, true), c.tuned.score.shown,
        c.tuned.score.shown_right);
  }
  if (latency) {
    std::vector<double> ms;
    size_t calls = 0;
    for (const auto& c : cases) {
      if (!c.usable || ms.size() >= latency)
        continue;
      CachedScorer fresh(
          &provider);  // 每題重新開始：同一題內的重複評分仍然省掉
      const auto t0 = std::chrono::steady_clock::now();
      double gain = NAN;
      ime::RescoreSentence(&fresh, Tail(c.context, 30), c.units, c.homophones,
                           &gain, defaults);
      ms.push_back(std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count());
      calls += fresh.calls;
    }
    std::printf(
        "\n推薦延遲（預設參數，%zu 題，平均評分 %.1f 次）：P50 %.0f ms、P95 "
        "%.0f "
        "ms、P99 %.0f ms\n",
        ms.size(), ms.empty() ? 0.0 : (double)calls / ms.size(),
        Percentile(ms, 0.5), Percentile(ms, 0.95), Percentile(ms, 0.99));
  }
  const Config& best = configs.front();
  PrintScore("最好的一組", best.tuned.score, best.outcomes.size());
  LearningCurve(best.outcomes, best.tuned);

  if (!dump_file.empty()) {
    std::ofstream out(dump_file, std::ios::binary);
    out << "kind\tgain\tright\tdraft_right\n";
    for (const auto& o : best.outcomes)
      if (o.offered)
        out << "recommend\t" << o.gain << '\t' << o.right << '\t'
            << o.draft_right << '\n';
  }

  // 整句校正：生成一次，gain 是校正前後的通順度差
  if (correct) {
    std::vector<Outcome> outcomes;
    size_t n = 0;
    start = std::chrono::steady_clock::now();
    for (const auto& c : cases) {
      if (c.zhuyin.empty() || c.draft.empty())
        continue;
      Outcome o;
      o.draft_right = c.draft == c.truth;
      const std::wstring corrected = ime::CleanCorrection(
          provider.CorrectSentence(L"", c.zhuyin, c.draft, L""), c.draft);
      o.offered = !corrected.empty();
      o.right = o.offered && corrected == c.truth;
      double before = 0, after = 0;
      o.gain = o.offered &&
                       scorer.ScoreText(Tail(c.context, 30), c.draft, &before,
                                        nullptr) &&
                       scorer.ScoreText(Tail(c.context, 30), corrected, &after,
                                        nullptr)
                   ? after - before
                   : NAN;
      if (o.offered && !std::isfinite(o.gain))
        o.offered = false;
      outcomes.push_back(o);
      if (++n % 50 == 0)
        std::printf("  校正 %zu 題…\n", n);
    }
    std::printf("\n整句校正（%zu 題，%lld 秒）：只用初稿 %.4f、全顯示 %.4f\n",
                outcomes.size(),
                (long long)std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                Baseline(outcomes, false), Baseline(outcomes, true));
    ime::CalibrationPrior prior;
    prior.min_samples = 1;
    PrintScore("目前的校準（門檻 0.1）", CrossValidate(outcomes, prior, 0.1),
               outcomes.size());
    const Tuned tuned = TuneCalibration(outcomes);
    std::printf("  調過：門檻 %.2f、先驗 a=%.1f(w %.1f) b(w %.3f)\n",
                tuned.threshold, tuned.prior.slope, tuned.prior.slope_weight,
                tuned.prior.bias_weight);
    PrintScore("調過", tuned.score, outcomes.size());
    if (!dump_file.empty()) {
      std::ofstream out(dump_file, std::ios::binary | std::ios::app);
      for (const auto& o : outcomes)
        if (o.offered)
          out << "correction\t" << o.gain << '\t' << o.right << '\t'
              << o.draft_right << '\n';
    }
  }
  api->finalize();
  return 0;
}
