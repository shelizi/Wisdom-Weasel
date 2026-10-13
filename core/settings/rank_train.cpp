#include "rank_train.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <set>

#include "../ime/ranker.h"
#include "../llm/LLMProvider.h"
#include "../third_party/nlohmann/json.hpp"
#include "../ime/ZhuyinPreview.h"
#include "rerank_tune.h"

namespace fs = std::filesystem;

namespace settings_rerank {

namespace {

using ime::kRankerFeatures;
using nlohmann::json;

constexpr size_t kMinCases = 50;      // 訓練至少要這麼多可用紀錄
constexpr double kValShare = 0.2;     // 時間切分：最後 20% 驗證
constexpr size_t kMinVal = 10;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 一個 case 的訓練資料：每句一列特徵 + 使用者選了第幾句（-1 = 不在候選裡）
struct CaseRows {
  const ScoredCase* sc = nullptr;
  int chosen = -1;
  std::vector<double> feats;  // n × kRankerFeatures
};

double Sigmoid(double x) {
  return x < -30 ? 0 : x > 30 ? 1 : 1.0 / (1.0 + std::exp(-x));
}

// 現行公式分數（RerankScore 的逐項版：要 index 對齊的 personal）
double FormulaScore(double lm, size_t chars, double personal, double alpha, double w) {
  if (std::isnan(lm))
    return lm;
  double s = lm;
  if (alpha > 0)
    s /= std::pow((double)(std::max)((size_t)1, chars), alpha);
  return s + w * personal;
}

// 建一個 case 的特徵列；回傳是否有無效（任一列有 NaN → 訓練不用這個 case）
bool BuildRows(const ScoredCase& sc, size_t context_chars, double alpha, double personal_w,
               CaseRows* out) {
  const LoggedCase& c = *sc.c;
  const size_t n = c.sentences.size();
  if (n < 2 || sc.lm.size() != n)
    return false;
  int chosen = -1;
  for (size_t i = 0; i < n; ++i)
    if (c.sentences[i] == c.truth) {
      chosen = (int)i;
      break;
    }
  if (chosen < 0)
    return false;
  std::vector<double> personal(n, 0);
  for (size_t i = 0; i < (std::min)(n, c.personal.size()); ++i)
    personal[i] = c.personal[i];
  std::vector<double> formula(n);
  for (size_t i = 0; i < n; ++i) {
    const size_t chars =
        (std::max)((size_t)1, zhuyin_preview::SplitChars(c.sentences[i]).size());
    formula[i] = FormulaScore(sc.lm[i], chars, personal[i], alpha, personal_w);
  }
  const size_t ctx = (std::min)(c.context.size(), context_chars);
  ime::RankerFeatures(ctx, c.zhuyin.size(), c.sentences, sc.lm, personal, formula,
                      &out->feats);
  for (double v : out->feats)
    if (std::isnan(v))
      return false;
  out->sc = &sc;
  out->chosen = chosen;
  return true;
}

// --- 線性：pairwise logistic（Bradley-Terry）；b 在分差裡消掉，固定 0 --------

struct LinearModel {
  std::vector<double> w = std::vector<double>(kRankerFeatures, 0);
  std::vector<double> mean = std::vector<double>(kRankerFeatures, 0);
  std::vector<double> stdev = std::vector<double>(kRankerFeatures, 1);
};

double LinearScore(const LinearModel& m, const double* f) {
  double s = 0;
  for (size_t i = 0; i < kRankerFeatures; ++i)
    s += m.w[i] * (f[i] - m.mean[i]) / m.stdev[i];
  return s;
}

void TrainLinear(std::vector<CaseRows>& train, LinearModel* out) {
  // 特徵標準化（全部候選列一起算）
  size_t rows = 0;
  for (const auto& c : train)
    rows += c.sc->c->sentences.size();
  for (const auto& c : train) {
    const size_t n = c.sc->c->sentences.size();
    for (size_t i = 0; i < n; ++i)
      for (size_t f = 0; f < kRankerFeatures; ++f)
        out->mean[f] += c.feats[i * kRankerFeatures + f];
  }
  for (double& v : out->mean)
    v /= (double)(std::max)((size_t)1, rows);
  for (const auto& c : train) {
    const size_t n = c.sc->c->sentences.size();
    for (size_t i = 0; i < n; ++i)
      for (size_t f = 0; f < kRankerFeatures; ++f) {
        const double d = c.feats[i * kRankerFeatures + f] - out->mean[f];
        out->stdev[f] += d * d;
      }
  }
  for (double& v : out->stdev)
    v = std::sqrt(v / (double)(std::max)((size_t)1, rows)) + 1e-9;

  // pairwise：選中的要比其他每一句高分
  std::vector<std::pair<const double*, const double*>> pairs;
  for (const auto& c : train) {
    const size_t n = c.sc->c->sentences.size();
    const double* pos = c.feats.data() + (size_t)c.chosen * kRankerFeatures;
    for (size_t i = 0; i < n; ++i)
      if ((int)i != c.chosen)
        pairs.emplace_back(pos, c.feats.data() + i * kRankerFeatures);
  }
  std::mt19937 rng(20261013);
  const double lambda = 1e-3;
  for (int epoch = 0; epoch < 40; ++epoch) {
    std::shuffle(pairs.begin(), pairs.end(), rng);
    const double lr = 0.05 / (1.0 + epoch * 0.15);
    for (const auto& p : pairs) {
      double diff[kRankerFeatures];
      double margin_s = 0;
      for (size_t f = 0; f < kRankerFeatures; ++f) {
        diff[f] = (p.first[f] - p.second[f]) / out->stdev[f];  // mean 消掉
        margin_s += out->w[f] * diff[f];
      }
      const double g = Sigmoid(-margin_s);  // d loss / d s_pos
      for (size_t f = 0; f < kRankerFeatures; ++f)
        out->w[f] += lr * (g * diff[f] - lambda * out->w[f]);
    }
  }
}

// --- GBDT：binary logistic，殘差上擬合小樹 -----------------------------------

struct TreeNode {
  int f = -1;
  double t = 0;
  int l = -1, r = -1;
  double v = 0;
};

struct Gbdt {
  double init = 0;
  double lr = 0.1;
  std::vector<std::vector<TreeNode>> trees;
};

// 一個節點的最佳分割：回傳 gain 與門檻；rows 是這個節點的列號
void BestSplit(const std::vector<const double*>& X,
               const std::vector<double>& r,
               const std::vector<double>& h,
               const std::vector<size_t>& rows,
               double lambda,
               int* feat,
               double* thr,
               double* gain) {
  *feat = -1;
  *gain = 0;
  double sum_r = 0, sum_h = 0;
  for (size_t i : rows) {
    sum_r += r[i];
    sum_h += h[i];
  }
  const double base_gain = sum_r * sum_r / (sum_h + lambda);
  for (size_t f = 0; f < kRankerFeatures; ++f) {
    std::vector<std::pair<double, size_t>> sorted;
    sorted.reserve(rows.size());
    for (size_t i : rows)
      sorted.emplace_back(X[i][f], i);
    std::sort(sorted.begin(), sorted.end());
    // 掃描分割點（相同值不切）
    double rl = 0, hl = 0;
    for (size_t k = 0; k + 1 < sorted.size(); ++k) {
      const size_t i = sorted[k].second;
      rl += r[i];
      hl += h[i];
      if (sorted[k].first == sorted[k + 1].first)
        continue;
      const size_t nl = k + 1, nr = sorted.size() - nl;
      if (nl < 5 || nr < 5)
        continue;
      const double rr = sum_r - rl, hr = sum_h - hl;
      const double g = rl * rl / (hl + lambda) + rr * rr / (hr + lambda) - base_gain;
      if (g > *gain) {
        *gain = g;
        *feat = (int)f;
        *thr = (sorted[k].first + sorted[k + 1].first) / 2;
      }
    }
  }
}

// 深度至多 depth 的回歸樹，擬合殘差 r（h 是 Hessian，葉子值 = Σr/(Σh+λ)）
void FitTree(const std::vector<const double*>& X,
             const std::vector<double>& r,
             const std::vector<double>& h,
             const std::vector<size_t>& rows,
             int depth,
             std::vector<TreeNode>* tree) {
  const double lambda = 1.0;
  struct Item {
    std::vector<size_t> rows;
    int depth;
    int idx;
  };
  std::vector<Item> stack{{rows, 0, 0}};
  tree->push_back(TreeNode{});
  while (!stack.empty()) {
    Item it = std::move(stack.back());
    stack.pop_back();
    TreeNode& node = (*tree)[it.idx];
    double sum_r = 0, sum_h = 0;
    for (size_t i : it.rows) {
      sum_r += r[i];
      sum_h += h[i];
    }
    if (it.depth >= depth || it.rows.size() < 10) {
      node.v = sum_r / (sum_h + lambda);
      continue;
    }
    int feat;
    double thr, gain;
    BestSplit(X, r, h, it.rows, lambda, &feat, &thr, &gain);
    if (feat < 0 || gain <= 1e-9) {
      node.v = sum_r / (sum_h + lambda);
      continue;
    }
    std::vector<size_t> left, right;
    for (size_t i : it.rows)
      (X[i][feat] <= thr ? left : right).push_back(i);
    if (left.empty() || right.empty()) {
      node.v = sum_r / (sum_h + lambda);
      continue;
    }
    node.f = feat;
    node.t = thr;
    node.l = (int)tree->size();
    node.r = (int)tree->size() + 1;
    tree->push_back(TreeNode{});
    tree->push_back(TreeNode{});
    stack.push_back({std::move(left), it.depth + 1, node.l});
    stack.push_back({std::move(right), it.depth + 1, node.r});
  }
}

double TreeScore(const std::vector<TreeNode>& tree, const double* f) {
  size_t i = 0;
  while (tree[i].f >= 0)
    i = (size_t)(f[tree[i].f] <= tree[i].t ? tree[i].l : tree[i].r);
  return tree[i].v;
}

void TrainGbdt(const std::vector<CaseRows>& train, const std::atomic<bool>& cancel,
               Gbdt* out) {
  // y=1 是使用者選的那句
  std::vector<const double*> X;
  std::vector<double> y;
  for (const auto& c : train) {
    const size_t n = c.sc->c->sentences.size();
    for (size_t i = 0; i < n; ++i) {
      X.push_back(c.feats.data() + i * kRankerFeatures);
      y.push_back((int)i == c.chosen ? 1 : 0);
    }
  }
  double pos = 0;
  for (double v : y)
    pos += v;
  out->init = std::log((pos + 0.5) / (y.size() - pos + 0.5));
  std::vector<double> F(X.size(), out->init), r(X.size()), h(X.size());
  std::vector<size_t> all(X.size());
  std::iota(all.begin(), all.end(), 0);
  std::mt19937 rng(20261013);
  for (int round = 0; round < 150 && !cancel; ++round) {
    for (size_t i = 0; i < X.size(); ++i) {
      const double p = Sigmoid(F[i]);
      r[i] = y[i] - p;
      h[i] = (std::max)(p * (1 - p), 1e-6);
    }
    std::vector<size_t> sub = all;
    std::shuffle(sub.begin(), sub.end(), rng);
    sub.resize((size_t)(sub.size() * 0.8));
    std::vector<TreeNode> tree;
    FitTree(X, r, h, sub, 3, &tree);
    if (tree.size() <= 1)
      break;
    for (size_t i = 0; i < X.size(); ++i)
      F[i] += out->lr * TreeScore(tree, X[i]);
    out->trees.push_back(std::move(tree));
  }
}

// --- 驗證：門檻掃描＋成績 ------------------------------------------------------

struct ModelScores {
  std::vector<double> s;  // 每個 val case 的「最好非第一句 − 第一句」與對錯
  std::vector<int> best;  // argmax_{i>0}；-1 = 沒有可選的
};

// 對每個 val case 算模型分數，記 argmax 與 gap
void ScoreVal(const std::vector<CaseRows>& val,
              const std::function<double(const double*)>& score,
              ModelScores* out) {
  out->s.resize(val.size());
  out->best.resize(val.size());
  for (size_t k = 0; k < val.size(); ++k) {
    const CaseRows& c = val[k];
    const size_t n = c.sc->c->sentences.size();
    const double s0 = score(c.feats.data());
    int best = -1;
    double bs = -std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < n; ++i) {
      const double s = score(c.feats.data() + i * kRankerFeatures);
      if (s > bs) {
        bs = s;
        best = (int)i;
      }
    }
    out->best[k] = best;
    out->s[k] = best >= 0 ? bs - s0 : -std::numeric_limits<double>::infinity();
  }
}

// gap > margin 才換：掃門檻，回傳最佳門檻下的 acc/wrong（限制錯改 ≤ cap）
struct ValOutcome {
  double margin = 0, acc = 0, wrong = 0;
};
ValOutcome PickMargin(const std::vector<CaseRows>& val,
                      const ModelScores& ms,
                      double wrong_cap) {
  ValOutcome best;
  std::set<double> grid{0.0};
  for (double g : ms.s)
    if (std::isfinite(g))
      grid.insert(g);
  // 取 gaps 的中點當門檻候選（模型分數尺度任意）
  std::vector<double> ths(grid.begin(), grid.end());
  std::vector<double> cands{0.0};
  for (size_t i = 0; i + 1 < ths.size(); ++i)
    cands.push_back((ths[i] + ths[i + 1]) / 2);
  cands.push_back(ths.back() + 1);
  size_t base_right = 0;
  for (const auto& c : val)
    base_right += c.sc->c->draft == c.sc->c->truth;
  for (double m : cands) {
    size_t right = 0, wrong = 0;
    for (size_t k = 0; k < val.size(); ++k) {
      const bool change = ms.best[k] > 0 && ms.s[k] > m;
      const auto& c = val[k];
      right += change ? ms.best[k] == c.chosen : c.chosen == 0;
      wrong += change && c.sc->c->draft == c.sc->c->truth && ms.best[k] != c.chosen;
    }
    const double acc = (double)right / val.size();
    const double w = base_right ? (double)wrong / base_right : 0;
    if (w > wrong_cap + 1e-9)
      continue;
    // 準確率同分取較大門檻（保守）
    if (acc > best.acc + 1e-9 || (acc > best.acc - 1e-9 && m > best.margin))
      best = {m, acc, w};
  }
  if (best.acc == 0) {  // 沒有門檻滿足錯改上限：回報不受限的最佳，讓比較階段決定
    for (double m : cands) {
      size_t right = 0, wrong = 0;
      for (size_t k = 0; k < val.size(); ++k) {
        const bool change = ms.best[k] > 0 && ms.s[k] > m;
        const auto& c = val[k];
        right += change ? ms.best[k] == c.chosen : c.chosen == 0;
        wrong += change && c.sc->c->draft == c.sc->c->truth && ms.best[k] != c.chosen;
      }
      const double acc = (double)right / val.size();
      const double w = base_right ? (double)wrong / base_right : 0;
      if (acc > best.acc + 1e-9 || (acc > best.acc - 1e-9 && m > best.margin))
        best = {m, acc, w};
    }
  }
  return best;
}

}  // namespace

RankTrainReport TrainRanker(LLMProvider* scorer,
                            const std::filesystem::path& personal_dir,
                            size_t context_chars,
                            double alpha,
                            double personal_w,
                            size_t cur_top,
                            double cur_margin,
                            const std::atomic<bool>& cancel,
                            const std::function<void(size_t, size_t)>& progress) {
  RankTrainReport rep;
  std::vector<LoggedCase> cases;
  LoadLoggedTopK(personal_dir, &cases);
  rep.cases = cases.size();
  if (cases.size() < kMinCases) {
    rep.message = "有整句候選的紀錄 " + std::to_string(cases.size()) +
                  " 筆（訓練至少 " + std::to_string(kMinCases) + " 筆）";
    return rep;
  }
  const auto items = ScoreCases(scorer, cases, context_chars, cancel, progress);
  if (cancel) {
    rep.cancelled = true;
    rep.message = "已取消";
    return rep;
  }

  // 每個 case 建特徵列；選中的不在候選裡或評分失敗的不用
  std::vector<CaseRows> all;
  for (const auto& it : items) {
    CaseRows rows;
    if (BuildRows(it, context_chars, alpha, personal_w, &rows))
      all.push_back(std::move(rows));
  }
  rep.usable = all.size();
  if (all.size() < kMinCases) {
    rep.message = "可用紀錄 " + std::to_string(all.size()) + " 筆（送出的句子要在候選裡且評分成功）";
    return rep;
  }

  // 時間切分：照紀錄順序，最後 20% 驗證
  const size_t val_n = (std::max)(kMinVal, (size_t)(all.size() * kValShare));
  const size_t train_n = all.size() - val_n;
  if (train_n < 40) {
    rep.message = "訓練資料不足（" + std::to_string(train_n) + " 筆）";
    return rep;
  }
  std::vector<CaseRows> train(all.begin(), all.begin() + train_n);
  std::vector<CaseRows> val(all.begin() + train_n, all.end());
  rep.train_cases = train.size();
  rep.val_cases = val.size();

  // 基線：現行公式在驗證集（top 以資料句數為限）
  std::vector<ScoredCase> val_scored;
  for (const auto& c : val)
    val_scored.push_back(*c.sc);
  const TuneOutcome base = EvalParams(val_scored, cur_top, cur_margin, alpha, personal_w);
  rep.base_acc = base.accuracy;
  rep.base_wrong = base.wrong_change;
  // 錯改上限：不準比現在更常把對的改掉（留緩衝）
  const double wrong_cap = base.wrong_change + (std::max)(0.02, base.wrong_change * 0.25);

  // 訓練兩個模型
  LinearModel lin;
  TrainLinear(train, &lin);
  ModelScores ms;
  ScoreVal(val, [&lin](const double* f) { return LinearScore(lin, f); }, &ms);
  const ValOutcome lin_v = PickMargin(val, ms, wrong_cap);
  rep.lin_acc = lin_v.acc;

  Gbdt gbdt;
  TrainGbdt(train, cancel, &gbdt);
  if (cancel) {
    rep.cancelled = true;
    rep.message = "已取消";
    return rep;
  }
  ScoreVal(val, [&gbdt](const double* f) {
    double s = gbdt.init;
    for (const auto& t : gbdt.trees)
      s += gbdt.lr * TreeScore(t, f);
    return s;
  }, &ms);
  const ValOutcome gbdt_v = PickMargin(val, ms, wrong_cap);
  rep.gbdt_acc = gbdt_v.acc;

  // 贏才匯出：較好的模型要真的比公式準（至少多對一筆），錯改也要在預算內
  const bool use_gbdt = gbdt_v.acc > lin_v.acc + 1e-9;
  const ValOutcome v = use_gbdt ? gbdt_v : lin_v;
  rep.type = use_gbdt ? "gbdt" : "linear";
  rep.val_acc = v.acc;
  rep.val_wrong = v.wrong;
  rep.margin = v.margin;
  const double min_gain = 1.0 / val.size() + 1e-9;
  const bool win = v.acc >= base.accuracy + min_gain && v.wrong <= wrong_cap + 1e-9;
  const auto write_history = [&](bool exported) {
    // 訓練紀錄（rank_train.log；不含使用者文字）：每次都記，方便看為什麼沒啟用
    std::ofstream hist(personal_dir / "rank_train.log", std::ios::app);
    hist << json{{"ts", NowMs()},
                 {"type", rep.type},
                 {"cases", all.size()},
                 {"val", val.size()},
                 {"val_acc", v.acc},
                 {"val_wrong", v.wrong},
                 {"margin", v.margin},
                 {"base_acc", base.accuracy},
                 {"exported", exported}}
                .dump()
         << "\n";
  };
  if (!win) {
    write_history(false);
    const auto pct = [](double x) { return std::to_string((int)(x * 1000) / 10.0) + "%"; };
    std::string why;
    if (!(v.acc >= base.accuracy + min_gain))
      why = "準確率沒贏";
    if (!(v.wrong <= wrong_cap + 1e-9))
      why += (why.empty() ? "" : "，") + std::string("錯改率 ") + pct(v.wrong) +
             " 超過上限 " + pct(wrong_cap);
    rep.message = "驗證沒有贏過目前設定（公式 " + pct(base.accuracy) + " vs " +
                  rep.type + " " + pct(v.acc) + (why.empty() ? "" : "，" + why) + "），不匯出";
    return rep;
  }

  // 匯出 JSON（ranker.cpp 的 Load 讀）
  json model{{"version", 1},
             {"type", rep.type},
             {"margin", v.margin},
             {"features", std::vector<std::string>(
                              ime::kRankerFeatureNames,
                              ime::kRankerFeatureNames + kRankerFeatures)},
             {"trained",
              {{"ts", NowMs()},
               {"cases", all.size()},
               {"train", train.size()},
               {"val", val.size()},
               {"val_acc", v.acc},
               {"val_wrong", v.wrong},
               {"base_acc", base.accuracy},
               {"lin_acc", lin_v.acc},
               {"gbdt_acc", gbdt_v.acc},
               {"formula", {{"top", cur_top}, {"margin", cur_margin}}}}}};
  if (use_gbdt) {
    json trees = json::array();
    for (const auto& t : gbdt.trees) {
      json nodes = json::array();
      for (const auto& n : t)
        nodes.push_back({{"f", n.f}, {"t", n.t}, {"l", n.l}, {"r", n.r}, {"v", n.v}});
      trees.push_back(std::move(nodes));
    }
    model["gbdt"] = {{"init", gbdt.init}, {"lr", gbdt.lr}, {"trees", std::move(trees)}};
  } else {
    model["linear"] = {{"mean", lin.mean}, {"std", lin.stdev}, {"w", lin.w}, {"b", 0.0}};
  }
  const fs::path out_path = personal_dir / "ranker_model.json";
  {
    std::ofstream out(out_path, std::ios::trunc);
    if (!out) {
      rep.message = "寫入模型檔失敗";
      return rep;
    }
    out << model.dump(2);
  }
  rep.exported = true;
  write_history(true);
  rep.message = "已匯出 " + rep.type + " 模型：驗證準確率 " +
                std::to_string((int)(v.acc * 1000) / 10.0) + "%（公式 " +
                std::to_string((int)(base.accuracy * 1000) / 10.0) +
                "%），下次重排生效";
  return rep;
}

}  // namespace settings_rerank
