#pragma once

// 學習式整句排序器（選字紀錄訓出來的小模型）：
// Rime 給候選、LLM 給每句 log 機率，這個小模型用「每句的特徵」重打分數，
// 最高分比第一句好超過模型自己的門檻才推薦。特徵是稠密數值（LLM 分數、
// 名次、個人詞庫分數、現行公式分數……），模型是線性或 GBDT——表格特徵
// 在小資料量上比類神經網路穩，推理是查表/內積，微秒等級。
// 模型檔在 personal/ranker_model.json（設定程式 rank.train 寫出）；
// 載入或評分失敗一律 fail-open：照舊用公式結果。
#include <filesystem>
#include <string>
#include <vector>

#include "candidate_reranker.h"

namespace ime {

// 特徵順序固定（模型檔存特徵名，載入時比對避免新舊版錯位）
inline constexpr size_t kRankerFeatures = 11;
extern const char* const kRankerFeatureNames[kRankerFeatures];

// 一個候選一列（row-major，共 N×kRankerFeatures）。NaN 表示無效（lm 評分失敗）。
// texts/lm/personal/formula 要一樣長；formula 是現行公式分數（RerankScore）
void RankerFeatures(size_t context_chars,
                    size_t zhuyin_chars,
                    const std::vector<std::wstring>& texts,
                    const std::vector<double>& lm,
                    const std::vector<double>& personal,
                    const std::vector<double>& formula,
                    std::vector<double>* out);

class RankerModel {
 public:
  // 讀 JSON 模型；檔案不存在、格式不對、特徵名不符都回 false（IsLoaded=false）
  bool Load(const std::filesystem::path& path);
  bool IsLoaded() const { return loaded_; }
  const std::string& type() const { return type_; }
  double margin() const { return margin_; }
  // 訓練資訊（trained.*；顯示用）
  const std::string& info() const { return info_; }
  const std::filesystem::path& path() const { return path_; }

  // 一列特徵的分數；有 NaN 就回 NaN
  double Score(const double* f) const;
  // 整組候選各一個分數（失敗的候選是 NaN）
  void Scores(size_t context_chars,
              size_t zhuyin_chars,
              const std::vector<std::wstring>& texts,
              const std::vector<double>& lm,
              const std::vector<double>& personal,
              const std::vector<double>& formula,
              std::vector<double>* out) const;

 private:
  struct Node {
    int f = -1;      // 特徵編號；-1 = 葉子
    double t = 0;    // 分割門檻
    int l = -1, r = -1;
    double v = 0;    // 葉子的值
  };
  bool loaded_ = false;
  std::string type_;
  double margin_ = 0;
  std::string info_;
  std::filesystem::path path_;
  // linear：score = b + Σ w·(x−mean)/std
  std::vector<double> mean_, std_, w_;
  double b_ = 0;
  // gbdt：score = init + lr·Σ trees
  double init_ = 0, lr_ = 1;
  std::vector<std::vector<Node>> trees_;
};

// 模型在就用模型分數覆寫 result 的決策（text/best/index/gain/scores；
// lm_scores 保持原樣）。best 非第一句且分數差過模型門檻才推薦
void ApplyRanker(const RankerModel* model,
                 size_t context_chars,
                 const std::wstring& zhuyin,
                 const RerankOptions& options,
                 RerankResult* result);

}  // namespace ime
