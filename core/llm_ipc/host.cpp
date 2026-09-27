#include "host.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace llm_ipc {

namespace {

std::mutex g_write_mutex;
platform::Pipe* g_pipe = nullptr;  // g_write_mutex 保護

bool Send(Writer& message) {
  std::lock_guard<std::mutex> lock(g_write_mutex);
  return g_pipe && WriteMessage(*g_pipe, message);
}

}  // namespace

void Log(const std::string& text) {
  Writer message(Op::kLog, 0);
  message.Str(text);
  Send(message);
}

void Serve(platform::Pipe& pipe, const Handler& handler) {
  {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    g_pipe = &pipe;
  }
  // 讀取執行緒可能比這個函式晚結束，共用的狀態放在 shared_ptr 裡
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool closed = false;
    std::atomic<uint32_t> cancel_id{0};
  };
  auto state = std::make_shared<State>();

  // 讀取執行緒：取消訊息立刻生效，其他請求排隊交給主執行緒
  std::thread reader([state, &pipe] {
    std::string payload;
    while (ReadMessage(pipe, &payload)) {
      Reader peek(payload);
      if (!peek.Ok())
        break;
      if (peek.op() == Op::kCancel) {
        state->cancel_id = peek.id();
        continue;
      }
      std::lock_guard<std::mutex> lock(state->mutex);
      state->queue.push_back(std::move(payload));
      state->cv.notify_one();
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    state->cv.notify_one();
  });

  while (true) {
    std::string payload;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait(lock, [&] { return state->closed || !state->queue.empty(); });
      if (state->queue.empty())
        break;
      payload = std::move(state->queue.front());
      state->queue.pop_front();
    }
    Reader request(std::move(payload));
    const uint32_t id = request.id();
    Writer reply(Op::kReply, id);
    handler(request, reply, [state, id] { return state->cancel_id.load() == id; });
    if (!Send(reply))
      break;
  }

  {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    g_pipe = nullptr;
  }
  // 讀取執行緒只用到 state 與 pipe；pipe 由呼叫端在行程結束前保留
  reader.detach();
}

}  // namespace llm_ipc
