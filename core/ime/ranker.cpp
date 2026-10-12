#include "ranker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

#include "../base/devlog.h"
#include "../third_party/nlohmann/json.hpp"
#include "ZhuyinPreview.h"

namespace ime {

const char* const kRankerFeatureNames[kRankerFeatures] = {
    "lm",       // 模型整句 log 機率
    "lm_pc",    // 每字 log 機率（長度偏差）
    "lm_gap",   // 與第一句的模型分數差
    "pos",      // 在 Rime 整句裡的名次
    "chars",    // 字數
    "personal", // 個人詞庫分數
    "pers_gap", // 與第一句的個人詞庫分差
    "formula",  // 現行公式分數（RerankScore）
    "form_gap", // 與第一句的公式分差（目前的決策變數）
    "ctx",      // 前文長度（同組相同；跨組訊號）
    "zy",       // 注音長度（輸入越長越模糊）
};

void RankerFeatures(size_t context_chars,
                    size_t zhuyin_chars,
                    const std::vector<std::wstring>& texts,
                    const std::vector<double>& lm,
                    const std::vector<double>& personal,
                    const std::vector<double>& formula,
                    std::vector<double>* out) {
  const size_t n = texts.size();
  out->assign(n * kRankerFeatures, std::numeric_limits<double>::quiet_NaN());
  const double lm0 = lm.empty() ? 0 : lm[0];
  const double pers0 = personal.empty() ? 0 : personal[0];
  const double form0 = formula.empty() ? 0 : formula[0];
  for (size_t i = 0; i < n; ++i) {
    double* f = out->data() + i * kRankerFeatures;
    const double l = i < lm.size() ? lm[i] : std::numeric_limits<double>::quiet_NaN();
    const double chars =
        (double)(std::max)((size_t)1, zhuyin_preview::SplitChars(texts[i]).size());
    f[0] = l;
    f[1] = l / chars;
    f[2] = l - lm0;
    f[3] = (double)i;
    f[4] = chars;
    f[5] = i < personal.size() ? personal[i] : 0;
    f[6] = f[5] - pers0;
    f[7] = i < formula.size() ? formula[i] : std::numeric_limits<double>::quiet_NaN();
    f[8] = f[7] - form0;
    f[9] = (double)context_chars;
    f[10] = (double)zhuyin_chars;
  }
}

bool RankerModel::Load(const std::filesystem::path& path) {
  loaded_ = false;
  type_.clear();
  trees_.clear();
  w_.clear();
  mean_.clear();
  std_.clear();
  try {
    std::ifstream in(path);
    if (!in)
      return false;
    const auto j = nlohmann::json::parse(
        std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    if (j.value("version", 0) != 1)
      return false;
    // 特徵名要完全一致（順序錯位會靜默排錯）
    const auto names = j.value("features", std::vector<std::string>{});
    if (names.size() != kRankerFeatures)
      return false;
    for (size_t i = 0; i < kRankerFeatures; ++i)
      if (names[i] != kRankerFeatureNames[i])
        return false;
    type_ = j.value("type", "");
    margin_ = j.value("margin", 0.0);
    path_ = path;
    if (j.contains("trained"))
      info_ = j["trained"].dump();
    if (type_ == "linear") {
      const auto& m = j.at("linear");
      mean_ = m.value("mean", std::vector<double>{});
      std_ = m.value("std", std::vector<double>{});
      w_ = m.value("w", std::vector<double>{});
      b_ = m.value("b", 0.0);
      if (w_.size() != kRankerFeatures || mean_.size() != kRankerFeatures ||
          std_.size() != kRankerFeatures)
        return false;
      for (double& s : std_)
        if (!(s > 0))
          s = 1;
    } else if (type_ == "gbdt") {
      const auto& g = j.at("gbdt");
      init_ = g.value("init", 0.0);
      lr_ = g.value("lr", 1.0);
      for (const auto& t : g.value("trees", std::vector<nlohmann::json>{})) {
        std::vector<Node> nodes;
        for (const auto& n : t) {
          Node node;
          node.f = n.value("f", -1);
          node.t = n.value("t", 0.0);
          node.l = n.value("l", -1);
          node.r = n.value("r", -1);
          node.v = n.value("v", 0.0);
          nodes.push_back(node);
        }
        if (!nodes.empty())
          trees_.push_back(std::move(nodes));
      }
      if (trees_.empty() || !(lr_ > 0))
        return false;
    } else {
      return false;
    }
    loaded_ = true;
  } catch (...) {
    return false;
  }
  return loaded_;
}

double RankerModel::Score(const double* f) const {
  if (!loaded_)
    return std::numeric_limits<double>::quiet_NaN();
  for (size_t i = 0; i < kRankerFeatures; ++i)
    if (std::isnan(f[i]))
      return std::numeric_limits<double>::quiet_NaN();
  if (type_ == "linear") {
    double s = b_;
    for (size_t i = 0; i < kRankerFeatures; ++i)
      s += w_[i] * (f[i] - mean_[i]) / std_[i];
    return s;
  }
  double s = init_;
  for (const auto& tree : trees_) {
    size_t i = 0;
    while (tree[i].f >= 0)
      i = (size_t)(f[tree[i].f] <= tree[i].t ? tree[i].l : tree[i].r);
    s += lr_ * tree[i].v;
  }
  return s;
}

void RankerModel::Scores(size_t context_chars,
                         size_t zhuyin_chars,
                         const std::vector<std::wstring>& texts,
                         const std::vector<double>& lm,
                         const std::vector<double>& personal,
                         const std::vector<double>& formula,
                         std::vector<double>* out) const {
  std::vector<double> feats;
  RankerFeatures(context_chars, zhuyin_chars, texts, lm, personal, formula, &feats);
  out->resize(texts.size());
  for (size_t i = 0; i < texts.size(); ++i)
    (*out)[i] = Score(feats.data() + i * kRankerFeatures);
}

void ApplyRanker(const RankerModel* model,
                 size_t context_chars,
                 const std::wstring& zhuyin,
                 const RerankOptions& options,
                 RerankResult* result) {
  if (!model || !model->IsLoaded() || !result)
    return;
  RerankResult& r = *result;
  if (r.texts.size() < 2 || r.lm_scores.size() != r.texts.size() ||
      r.scores.size() != r.texts.size())
    return;
  std::vector<double> personal(r.texts.size(), 0);
  if (options.personal)
    for (size_t i = 0; i < r.texts.size(); ++i)
      personal[i] = options.personal(r.texts[i]);
  std::vector<double> rs;
  model->Scores(context_chars, zhuyin.size(), r.texts, r.lm_scores, personal, r.scores, &rs);
  size_t best = 0;
  for (size_t i = 1; i < rs.size(); ++i)
    if (!std::isnan(rs[i]) && (std::isnan(rs[best]) || rs[i] > rs[best]))
      best = i;
  if (std::isnan(rs[best]))
    return;  // 全部無效：保留公式結果
  // 模型分數取代公式分數（機率顯示與實際決策一致），重算推薦
  r.scores = rs;
  r.text.clear();
  r.best.clear();
  r.index = 0;
  r.gain = std::numeric_limits<double>::quiet_NaN();
  if (best > 0) {
    r.gain = rs[best] - rs[0];
    r.index = best;
    r.best = r.texts[best];
    if (r.gain > model->margin())
      r.text = r.texts[best];
  }
}

}  // namespace ime
