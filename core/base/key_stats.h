#pragma once

// 按鍵路徑計時：環形緩衝記最近幾百次的各階段耗時，Report() 給開發終端看 p50/p95。
// Add 只存一個 uint32，開銷可忽略；不要在 Report 以外的路徑做排序或字串處理。
#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace base {

class KeyStats {
 public:
  enum Stage {
    kProcessKey = 0,  // librime 的 process_key（controller 裡實際送給 Rime 的時間）
    kController,      // Controller::ProcessKey 全程
    kRespond,         // _Respond + _UpdateUI（組回應與畫面）
    kTotal,           // ProcessKeyEvent 全程
    kStageCount
  };

  static KeyStats& Instance() {
    static KeyStats inst;
    return inst;
  }

  void Add(Stage stage, uint64_t us) {
    std::lock_guard<std::mutex> lock(mutex_);
    StageStat& s = stages_[stage];
    const uint32_t capped = us > 9999999 ? 9999999u : (uint32_t)us;
    if (s.ring.size() < kRing)
      s.ring.push_back(capped);
    else
      s.ring[s.pos++ % kRing] = capped;
    ++s.count;
    if (us >= kSlowUs)
      ++s.slow;
  }

  // 目前累計的按鍵數（以 kTotal 計）
  uint64_t Count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stages_[kTotal].count;
  }

  std::wstring Report() {
    static const wchar_t* kNames[kStageCount] = {L"rime", L"ctrl", L"respond", L"total"};
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstringstream out;
    out << L"[按鍵計時]";
    for (int i = 0; i < kStageCount; ++i) {
      const StageStat& s = stages_[i];
      if (s.ring.empty())
        continue;
      std::vector<uint32_t> v(s.ring.begin(), s.ring.end());
      std::sort(v.begin(), v.end());
      const double p50 = v[v.size() / 2] / 1000.0;
      const double p95 = v[v.size() * 95 / 100] / 1000.0;
      const double max = v.back() / 1000.0;
      out << L" " << kNames[i] << L" p50=" << std::fixed << std::setprecision(1) << p50
          << L" p95=" << p95 << L" max=" << max << L"ms >10ms:" << s.slow << L"(" << s.count
          << L")";
    }
    return out.str();
  }

 private:
  static constexpr size_t kRing = 512;
  static constexpr uint64_t kSlowUs = 10000;  // 超過 10ms 記一次慢

  struct StageStat {
    std::vector<uint32_t> ring;  // 微秒
    size_t pos = 0;
    uint64_t count = 0;
    uint64_t slow = 0;
  };

  std::mutex mutex_;
  StageStat stages_[kStageCount];
};

}  // namespace base
