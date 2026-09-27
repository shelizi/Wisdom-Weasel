#pragma once

// 時間：單調時鐘（量間隔用）與本地時間
#include <chrono>
#include <cstdint>
#include <ctime>

namespace base {

// 單調遞增的毫秒數（取代 GetTickCount64）
inline uint64_t MonotonicMs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline std::tm LocalTime(std::time_t t = std::time(nullptr)) {
  std::tm tm = {};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

}  // namespace base
