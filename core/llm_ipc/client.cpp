#include "client.h"

namespace llm_ipc {

namespace {

using Clock = std::chrono::steady_clock;

// 訊息開頭：u32 長度 | u8 種類 | u32 編號
constexpr size_t kIdOffset = 5;

void PatchId(std::string* bytes, uint32_t id) {
  std::memcpy(&(*bytes)[kIdOffset], &id, sizeof(id));
}

}  // namespace

Client::Client(Options options) : options_(std::move(options)) {}

Client::~Client() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    closing_ = true;
  }
  // 若另一個執行緒的請求正在等待，結束推理行程讓它立刻返回
  {
    std::lock_guard<std::mutex> lock(child_mutex_);
    if (child_)
      child_->Kill();
  }
  std::lock_guard<std::mutex> call(call_mutex_);
  ReapLocked(false);
}

bool Client::GaveUp() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return gave_up_;
}

void Client::Event(const std::string& text) const {
  if (options_.on_event)
    options_.on_event(text);
}

void Client::ReaderLoop(platform::ChildProcess* child) {
  std::string payload;
  while (ReadMessage(*child, &payload)) {
    Reader message(payload);
    if (!message.Ok())
      break;
    if (message.op() == Op::kLog) {
      const std::string text = message.Str();
      if (options_.on_log)
        options_.on_log(text);
    } else if (message.op() == Op::kReply) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (message.id() == pending_id_) {
        reply_ = std::move(payload);
        cv_.notify_all();
      }
    }
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  dead_ = true;
  cv_.notify_all();
}

void Client::ReapLocked(bool crashed) {
  if (!child_)
    return;
  child_->Shutdown();
  if (reader_.joinable())
    reader_.join();
  {
    std::lock_guard<std::mutex> lock(child_mutex_);
    child_.reset();
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  dead_ = false;
  if (!crashed || closing_)
    return;
  const auto now = Clock::now();
  crashes_.push_back(now);
  while (!crashes_.empty() && now - crashes_.front() > options_.crash_window)
    crashes_.pop_front();
  if ((int)crashes_.size() >= options_.max_crashes) {
    gave_up_ = true;
    Event("推理行程在短時間內結束了 " + std::to_string(crashes_.size()) +
          " 次，暫停使用到下次重新部署；輸入法的其他功能不受影響");
  } else {
    Event("推理行程意外結束，下次請求時重新啟動");
  }
}

bool Client::EnsureRunningLocked() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (gave_up_ || closing_)
      return false;
    if (child_ && !dead_ && child_->Alive())
      return true;
  }
  if (child_)
    ReapLocked(true);
  while (true) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (gave_up_ || closing_)
        return false;
      reply_.reset();
      pending_id_ = 0;
    }
    {
      auto child = platform::ChildProcess::Start(options_.exe, options_.args);
      std::lock_guard<std::mutex> lock(child_mutex_);
      child_ = std::move(child);
    }
    if (!child_) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      gave_up_ = true;
      Event("無法啟動推理行程：" + options_.exe.u8string());
      return false;
    }
    reader_ = std::thread(&Client::ReaderLoop, this, child_.get());
    // 重送設定；途中又當掉就重來（次數受 max_crashes 限制）
    bool ok = true;
    for (const std::string& setup : setups_) {
      std::string bytes = setup;
      const uint32_t id = ++next_id_;
      PatchId(&bytes, id);
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_id_ = id;
        reply_.reset();
      }
      if (!child_->Write(bytes.data(), bytes.size())) {
        ok = false;
        break;
      }
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (!cv_.wait_for(lock, options_.setup_timeout, [&] { return reply_ || dead_; })) {
        lock.unlock();
        child_->Kill();
        ok = false;
        break;
      }
      if (!reply_) {
        ok = false;
        break;
      }
    }
    if (ok)
      return true;
    ReapLocked(true);
  }
}

std::optional<Reader> Client::RoundTripLocked(Writer& request,
                                              const std::function<bool()>& cancelled,
                                              std::chrono::milliseconds timeout, bool* retry) {
  *retry = true;
  const uint32_t id = ++next_id_;
  request.SetId(id);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pending_id_ = id;
    reply_.reset();
  }
  if (!WriteMessage(*child_, request)) {
    ReapLocked(true);
    return std::nullopt;
  }
  const auto start = Clock::now();
  bool cancel_sent = false;
  Clock::time_point cancel_at;
  bool killed = false;
  std::unique_lock<std::mutex> lock(state_mutex_);
  while (!reply_ && !dead_) {
    cv_.wait_for(lock, std::chrono::milliseconds(20));
    if (reply_ || dead_)
      break;
    lock.unlock();
    const auto now = Clock::now();
    if (!cancel_sent && cancelled && cancelled()) {
      Writer cancel(Op::kCancel, id);
      WriteMessage(*child_, cancel);
      cancel_sent = true;
      cancel_at = now;
    }
    const bool hung = cancel_sent && now - cancel_at > options_.cancel_grace;
    const bool overtime = timeout.count() > 0 && now - start > timeout;
    if (!killed && (hung || overtime)) {
      Event(hung ? "推理行程取消後沒有回應，強制結束" : "推理行程逾時，強制結束");
      child_->Kill();
      killed = true;
    }
    lock.lock();
  }
  if (reply_) {
    Reader reply(std::move(*reply_));
    reply_.reset();
    return reply;
  }
  lock.unlock();
  *retry = !killed && !cancel_sent;
  ReapLocked(true);
  return std::nullopt;
}

std::optional<Reader> Client::SendLocked(Writer& request, const std::function<bool()>& cancelled,
                                         std::chrono::milliseconds timeout) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureRunningLocked())
      return std::nullopt;
    bool retry = false;
    auto reply = RoundTripLocked(request, cancelled, timeout, &retry);
    if (reply || !retry)
      return reply;
  }
  return std::nullopt;
}

std::optional<Reader> Client::Setup(Writer request, const std::function<bool()>& cancelled) {
  std::lock_guard<std::mutex> call(call_mutex_);
  std::string bytes = request.Finish();
  auto reply = SendLocked(request, cancelled, options_.setup_timeout);
  if (reply)
    setups_.push_back(std::move(bytes));
  return reply;
}

std::optional<Reader> Client::Call(Writer request, const std::function<bool()>& cancelled,
                                   std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> call(call_mutex_);
  return SendLocked(request, cancelled, timeout);
}

}  // namespace llm_ipc
