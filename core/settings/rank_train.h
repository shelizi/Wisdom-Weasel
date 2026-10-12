#pragma once

// 學習式整句排序器的訓練（設定程式「選字策略」頁＋日後排程）：
// 讀選字紀錄（Top-K 候選＋使用者送出的），用評分模型補齊每句的 LM 分數，
// 時間切分前 80% 訓練、後 20% 驗證，訓線性（pairwise logistic）與小 GBDT
// 兩個模型；驗證集上勝過現行公式（且錯改沒變多）才把較好的那個匯出成
// personal/ranker_model.json 給輸入法載入。失敗或沒贏就不匯出（fail-open）。
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class LLMProvider;

namespace settings_rerank {

struct RankTrainReport {
  bool exported = false;
  std::string type;               // 匯出的模型種類（linear/gbdt）
  size_t cases = 0;               // 有整句候選的紀錄
  size_t usable = 0;              // 送出的句子在候選裡、評分成功的
  size_t train_cases = 0, val_cases = 0;
  double base_acc = 0;            // 現行公式在驗證集的成績
  double base_wrong = 0;
  double lin_acc = 0;             // 線性模型驗證成績（各自最佳門檻下）
  double gbdt_acc = 0;
  double val_acc = 0;             // 匯出模型的驗證成績
  double val_wrong = 0;
  double margin = 0;              // 匯出模型的門檻
  bool cancelled = false;
  std::string message;            // 人看的結果
};

// 訓練＋驗證＋（贏才）匯出。scorer 由呼叫端載好；寫檔到 personal_dir。
// progress(done,total) 回報評分進度；cancel 設 true 盡快停
RankTrainReport TrainRanker(LLMProvider* scorer,
                            const std::filesystem::path& personal_dir,
                            size_t context_chars,
                            double alpha,
                            double personal_weight,
                            size_t cur_top,
                            double cur_margin,
                            const std::atomic<bool>& cancel,
                            const std::function<void(size_t, size_t)>& progress);

}  // namespace settings_rerank
