#pragma once

// 輸入法端：啟動並管理一個 LLM 推理行程。
// - 推理行程當掉或沒有回應時，只有這次請求失敗；下次請求會重新啟動它並重送設定。
// - 短時間內連續當掉太多次就停用，直到重新建立（重新部署）為止，不會一直重啟。
// - 等待回覆時定期檢查取消條件，取消後推理行程若遲遲不回應就強制結束。
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"

namespace llm_ipc {

class Client {
 public:
  struct Options {
    std::filesystem::path exe;
    std::vector<std::string> args;
    // 推理行程送來的開發終端記錄（在讀取執行緒上呼叫）
    std::function<void(const std::string&)> on_log;
    // 推理行程啟動、當掉、停用等事件的說明
    std::function<void(const std::string&)> on_event;
    int max_crashes = 3;                                   // crash_window 內當掉幾次就停用
    std::chrono::seconds crash_window{600};
    std::chrono::milliseconds cancel_grace{5000};          // 取消後等多久還沒回應就強制結束
    std::chrono::milliseconds setup_timeout{10 * 60 * 1000};  // 設定（載入模型）的時間上限
  };

  explicit Client(Options options);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // 設定請求：成功後記下來，推理行程重新啟動時依序重送（重送時不檢查取消）
  std::optional<Reader> Setup(Writer request, const std::function<bool()>& cancelled = nullptr);
  // 一般請求。cancelled 在等待時定期呼叫（呼叫端的執行緒上）；timeout 為 0 表示不限時。
  // 推理行程不可用、當掉、逾時時回傳 nullopt
  std::optional<Reader> Call(Writer request, const std::function<bool()>& cancelled,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  // 連續當掉而停用了
  bool GaveUp() const;

 private:
  bool EnsureRunningLocked();
  void ReapLocked(bool crashed);
  // retry 表示可以重新啟動後重送：推理行程自行結束（包括在兩次請求之間結束、請求送出時
  // 還沒察覺）。取消或逾時而被強制結束的不重送。請求都沒有副作用，重送是安全的
  std::optional<Reader> RoundTripLocked(Writer& request, const std::function<bool()>& cancelled,
                                        std::chrono::milliseconds timeout, bool* retry);
  // 失敗且可以重送時，重新啟動推理行程再送一次
  std::optional<Reader> SendLocked(Writer& request, const std::function<bool()>& cancelled,
                                   std::chrono::milliseconds timeout);
  void ReaderLoop(platform::ChildProcess* child);
  void Event(const std::string& text) const;

  const Options options_;
  std::mutex call_mutex_;  // 一次只處理一個請求
  std::mutex child_mutex_;  // 更換 child_ 時握著，讓解構時可以安全地結束子行程
  std::unique_ptr<platform::ChildProcess> child_;
  std::thread reader_;
  std::vector<std::string> setups_;  // 已成功的設定請求（原始位元組）
  std::deque<std::chrono::steady_clock::time_point> crashes_;
  uint32_t next_id_ = 0;

  mutable std::mutex state_mutex_;  // 保護以下（讀取執行緒與呼叫端共用）
  std::condition_variable cv_;
  uint32_t pending_id_ = 0;
  std::optional<std::string> reply_;
  bool dead_ = false;
  bool gave_up_ = false;
  bool closing_ = false;
};

}  // namespace llm_ipc
