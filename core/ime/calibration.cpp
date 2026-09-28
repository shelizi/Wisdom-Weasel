#include "calibration.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ime {

namespace {

double Sigmoid(double z) {
  return z >= 0 ? 1.0 / (1.0 + std::exp(-z)) : std::exp(z) / (1.0 + std::exp(z));
}

// 機率不要剛好是 0 或 1（log loss 會無限大）
double Clamp(double p) {
  return (std::min)(1.0 - 1e-6, (std::max)(1e-6, p));
}

const char* const kFile = "weasel_calibration.txt";

}  // namespace

const char* SuggestionKindName(SuggestionKind kind) {
  return kind == SuggestionKind::kRecommend ? "recommend" : "correction";
}

void Calibrator::Add(double gain, bool accepted, bool refit) {
  if (!std::isfinite(gain))
    return;
  samples_.push_back({gain, accepted});
  while (samples_.size() > kMaxSamples)
    samples_.pop_front();
  if (refit)
    Fit();
}

size_t Calibrator::Accepted() const {
  size_t n = 0;
  for (const auto& s : samples_)
    n += s.accepted ? 1 : 0;
  return n;
}

// 帶先驗的 logistic regression（牛頓法）：斜率的先驗是 a = 1（直接把 log 機率差當 logit），
// 樣本少或完全分得開時不會衝到無限大；截距幾乎不設限，由資料決定
void Calibrator::Fit() {
  const size_t accepted = Accepted();
  ready_ = samples_.size() >= kMinSamples && accepted > 0 && accepted < samples_.size();
  const double kPriorA = 1.0, kPriorB = 0.01;
  // 目標：log loss 總和 + 先驗
  const auto objective = [&](double a, double b) {
    double loss = 0.5 * (kPriorA * (a - 1.0) * (a - 1.0) + kPriorB * b * b);
    for (const auto& s : samples_) {
      const double p = Clamp(Sigmoid(a * s.gain + b));
      loss -= std::log(s.accepted ? p : 1.0 - p);
    }
    return loss;
  };
  double a = 1.0, b = 0.0;
  double current = objective(a, b);
  for (int iter = 0; iter < 50; ++iter) {
    double ga = kPriorA * (a - 1.0), gb = kPriorB * b;
    double haa = kPriorA, hab = 0, hbb = kPriorB;
    for (const auto& s : samples_) {
      const double p = Sigmoid(a * s.gain + b);
      const double r = p - (s.accepted ? 1.0 : 0.0);
      const double w = p * (1.0 - p);
      ga += r * s.gain;
      gb += r;
      haa += w * s.gain * s.gain;
      hab += w * s.gain;
      hbb += w;
    }
    const double det = haa * hbb - hab * hab;
    if (!(det > 1e-12))
      break;
    double da = (hbb * ga - hab * gb) / det;
    double db = (haa * gb - hab * ga) / det;
    // 離最佳解很遠時牛頓步會衝過頭：步長減半直到目標變小
    double next = objective(a - da, b - db);
    for (int half = 0; half < 30 && !(next <= current); ++half) {
      da *= 0.5;
      db *= 0.5;
      next = objective(a - da, b - db);
    }
    if (!(next <= current))
      break;
    a -= da;
    b -= db;
    const bool converged = current - next < 1e-10;
    current = next;
    if (converged)
      break;
  }
  if (std::isfinite(a) && std::isfinite(b)) {
    a_ = a;
    b_ = b;
  }
}

double Calibrator::Probability(double gain) const {
  if (!ready_ || !std::isfinite(gain))
    return NoConfidence();
  return Sigmoid(a_ * gain + b_);
}

double Calibrator::Ece() const {
  if (samples_.empty())
    return NoConfidence();
  const int kBins = 10;
  double sum_p[kBins] = {0}, sum_y[kBins] = {0};
  int count[kBins] = {0};
  for (const auto& s : samples_) {
    const double p = Sigmoid(a_ * s.gain + b_);
    const int bin = (std::min)(kBins - 1, (int)(p * kBins));
    sum_p[bin] += p;
    sum_y[bin] += s.accepted ? 1.0 : 0.0;
    ++count[bin];
  }
  double ece = 0;
  for (int i = 0; i < kBins; ++i)
    ece += std::abs(sum_p[i] - sum_y[i]) / samples_.size();
  return ece;
}

double Calibrator::LogLoss() const {
  if (samples_.empty())
    return NoConfidence();
  double loss = 0;
  for (const auto& s : samples_) {
    const double p = Clamp(Sigmoid(a_ * s.gain + b_));
    loss -= std::log(s.accepted ? p : 1.0 - p);
  }
  return loss / samples_.size();
}

void CalibrationStore::Load() {
  if (loaded_)
    return;
  loaded_ = true;
  size_t lines = 0;
  {
    std::ifstream in(dir_ / kFile, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      ++lines;
      std::istringstream f(line);
      std::string kind;
      double gain = 0;
      int accepted = 0;
      if (!(f >> kind >> gain >> accepted))
        continue;
      for (int k = 0; k < kSuggestionKinds; ++k) {
        if (kind == SuggestionKindName((SuggestionKind)k))
          calibrators_[k].Add(gain, accepted != 0, false);
      }
    }
  }
  for (auto& c : calibrators_)
    c.Fit();
  // 檔案只會附加：太長時改寫成只剩保留的樣本
  if (lines <= 2 * kSuggestionKinds * Calibrator::kMaxSamples)
    return;
  // 依種類各留最後 kMaxSamples 行
  std::deque<std::string> kept[kSuggestionKinds];
  {
    std::ifstream in(dir_ / kFile, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      for (int k = 0; k < kSuggestionKinds; ++k) {
        const std::string name = SuggestionKindName((SuggestionKind)k);
        if (line.compare(0, name.size() + 1, name + "\t") == 0) {
          kept[k].push_back(line);
          if (kept[k].size() > Calibrator::kMaxSamples)
            kept[k].pop_front();
        }
      }
    }
  }
  std::ofstream out(dir_ / kFile, std::ios::binary | std::ios::trunc);
  for (const auto& lines_of_kind : kept) {
    for (const auto& line : lines_of_kind)
      out << line << "\n";
  }
}

void CalibrationStore::Record(SuggestionKind kind, double gain, bool accepted) {
  if (!std::isfinite(gain))
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  Load();
  calibrators_[(int)kind].Add(gain, accepted);
  std::ofstream out(dir_ / kFile, std::ios::binary | std::ios::app);
  char line[64];
  std::snprintf(line, sizeof(line), "%s\t%.4f\t%d\n", SuggestionKindName(kind), gain, accepted ? 1 : 0);
  out << line;
}

double CalibrationStore::Probability(SuggestionKind kind, double gain) {
  std::lock_guard<std::mutex> lock(mutex_);
  Load();
  return calibrators_[(int)kind].Probability(gain);
}

std::string CalibrationStore::Summary(SuggestionKind kind) {
  std::lock_guard<std::mutex> lock(mutex_);
  Load();
  const Calibrator& c = calibrators_[(int)kind];
  if (c.Samples() == 0)
    return "";
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s：%zu 筆（採用 %.0f%%），", kind == SuggestionKind::kRecommend ? "推薦" : "校正",
                c.Samples(), 100.0 * c.Accepted() / c.Samples());
  std::string text = buf;
  if (!c.Ready()) {
    std::snprintf(buf, sizeof(buf), "未滿 %zu 筆或只有一種結果，還沒用來排序", Calibrator::kMinSamples);
    return text + buf;
  }
  std::snprintf(buf, sizeof(buf), "p = sigmoid(%.2f·gain %+.2f)，ECE %.3f、log loss %.3f", c.a(), c.b(), c.Ece(),
                c.LogLoss());
  return text + buf;
}

}  // namespace ime
