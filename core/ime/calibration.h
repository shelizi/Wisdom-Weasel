#pragma once

// 信心校準：把「推薦／整句校正比原句好多少」（本機模型的 log 機率差）換成「使用者會採用的機率」。
//   p = sigmoid(a·gain + b)，a、b 用使用者實際的選擇擬合（採用 = 1、出現了但沒用 = 0），
//   最小化 log loss（proper scoring rule：只有說實話的機率才會最好），所以 p 是校準過的。
// 樣本只有數字、不含打字內容，寫在使用者資料夾的 weasel_calibration.txt：
//   每行：種類（recommend / correction） \t gain \t 採用（0/1）
#include <cmath>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>

namespace ime {

enum class SuggestionKind { kRecommend = 0, kCorrection = 1 };
constexpr int kSuggestionKinds = 2;

const char* SuggestionKindName(SuggestionKind kind);

inline double NoConfidence() {
  return std::numeric_limits<double>::quiet_NaN();
}
inline bool HasConfidence(double p) {
  return !std::isnan(p);
}

// 一種建議的樣本與擬合結果（不加鎖，由 CalibrationStore 保護）
class Calibrator {
 public:
  static constexpr size_t kMaxSamples = 2000;  // 只留最近的樣本
  static constexpr size_t kMinSamples = 30;    // 少於這麼多（或全是同一種結果）不給機率

  // refit = false：之後自己呼叫 Fit（例如讀檔時一次擬合）
  void Add(double gain, bool accepted, bool refit = true);
  void Fit();
  // 還不夠樣本時回傳 NaN
  double Probability(double gain) const;
  bool Ready() const { return ready_; }
  size_t Samples() const { return samples_.size(); }
  size_t Accepted() const;
  double a() const { return a_; }
  double b() const { return b_; }
  // 擬合結果在目前樣本上的評估：expected calibration error（10 格）與平均 log loss
  double Ece() const;
  double LogLoss() const;

 private:
  struct Sample {
    double gain;
    bool accepted;
  };
  std::deque<Sample> samples_;
  double a_ = 1.0, b_ = 0.0;
  bool ready_ = false;
};

// 各種建議的校準器；輸入法執行緒寫、預測背景執行緒讀，自己加鎖
class CalibrationStore {
 public:
  explicit CalibrationStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

  // 記一筆並附加到檔案
  void Record(SuggestionKind kind, double gain, bool accepted);
  // 校準過的採用機率；樣本不夠時是 NaN
  double Probability(SuggestionKind kind, double gain);
  // 設定程式顯示用：「推薦：N 筆、採用 x%、ECE y」；沒有樣本回傳空字串
  std::string Summary(SuggestionKind kind);

 private:
  void Load();

  std::filesystem::path dir_;
  std::mutex mutex_;
  bool loaded_ = false;
  Calibrator calibrators_[kSuggestionKinds];
};

}  // namespace ime
