// 輸入法端 llm_ipc::Client 的測試：推理行程正常回覆、取消、當掉後重新啟動、
// 連續當掉後停用、取消後不回應時強制結束、逾時、請求進行中解構。Windows 以 run.bat
// 編譯執行，macOS 由 mac/CMakeLists.txt 建置。
#include "../../core/llm_ipc/client.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

using namespace std::chrono;
using llm_ipc::Client;
using llm_ipc::Op;
using llm_ipc::Writer;

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static std::filesystem::path g_host;

static Client::Options MakeOptions(std::vector<std::string>* events = nullptr,
                                   std::vector<std::string>* logs = nullptr) {
  Client::Options options;
  options.exe = g_host;
  options.cancel_grace = milliseconds(500);
  options.on_event = [events](const std::string& e) {
    if (events)
      events->push_back(e);
  };
  options.on_log = [logs](const std::string& l) {
    if (logs)
      logs->push_back(l);
  };
  return options;
}

static Writer Predict(const std::string& context, const std::string& mode) {
  Writer w(Op::kPredict, 0);
  w.Str(context);
  w.Str(mode);
  w.U32(5);
  return w;
}

// 回覆的第一個候選；失敗時回傳 "<none>"
static std::string First(Client& client,
                         const std::string& context,
                         const std::string& mode,
                         const std::function<bool()>& cancelled = nullptr,
                         milliseconds timeout = milliseconds(0)) {
  auto reply = client.Call(Predict(context, mode), cancelled, timeout);
  if (!reply)
    return "<none>";
  auto list = reply->StrList();
  return list.empty() || !reply->Ok() ? "<empty>" : list[0];
}

int main(int argc, char** argv) {
#ifdef _WIN32
  g_host = std::filesystem::path(argv[0]).parent_path() / "FakeHost.exe";
#else
  g_host = std::filesystem::path(argv[0]).parent_path() / "FakeHost";
#endif

  // 正常回覆，含 UTF-8 與較大的內容
  {
    Client client(MakeOptions());
    CHECK(client.Setup([] {
      Writer w(Op::kCreate, 0);
      w.Str("test");
      return w;
    }()));
    CHECK(First(client, "你好", "echo") == "你好");
    const std::string big(3 << 20, 'x');
    CHECK(First(client, big, "echo") == big);
  }

  // 推理行程的記錄會轉給輸入法
  {
    std::vector<std::string> logs;
    Client client(MakeOptions(nullptr, &logs));
    CHECK(First(client, "", "log") == "logged");
    CHECK(logs.size() == 1 && logs[0] == "hello from host");
  }

  // 取消：推理行程收到取消後回覆
  {
    Client client(MakeOptions());
    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
      std::this_thread::sleep_for(milliseconds(100));
      cancel = true;
    });
    const auto start = steady_clock::now();
    CHECK(First(client, "", "wait", [&] { return cancel.load(); }) ==
          "cancelled");
    CHECK(steady_clock::now() - start < seconds(2));
    canceller.join();
    CHECK(First(client, "still", "echo") == "still");  // 同一個推理行程繼續用
  }

  // 當掉：這次失敗，下次自動重新啟動並重送設定
  {
    std::vector<std::string> events;
    Client client(MakeOptions(&events));
    client.Setup([] {
      Writer w(Op::kCreate, 0);
      w.Str("test");
      return w;
    }());
    client.Setup([] {
      Writer w(Op::kLoadConfig, 0);
      w.Str("weasel");
      return w;
    }());
    CHECK(First(client, "", "setups") == "2");
    CHECK(First(client, "", "crash") == "<none>");
    CHECK(!client.GaveUp());
    CHECK(First(client, "", "setups") ==
          "2");  // 新的推理行程收到了重送的兩個設定
    CHECK(First(client, "back", "echo") == "back");
    CHECK(!events.empty());
  }

  // 兩次請求之間當掉：下一次請求不會失敗，自動重新啟動
  {
    Client client(MakeOptions());
    client.Setup([] {
      Writer w(Op::kCreate, 0);
      w.Str("test");
      return w;
    }());
    CHECK(First(client, "", "exit") == "bye");
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(First(client, "next", "echo") == "next");
    CHECK(First(client, "", "setups") == "1");
    // 還沒察覺結束就送出（讀取執行緒尚未收到結尾）也一樣
    CHECK(First(client, "", "exit") == "bye");
    std::this_thread::sleep_for(milliseconds(60));
    CHECK(First(client, "again", "echo") == "again");
  }

  // 連續當掉：停用，不再重新啟動
  {
    std::vector<std::string> events;
    Client client(MakeOptions(&events));
    for (int i = 0; i < 3; ++i)
      CHECK(First(client, "", "crash") == "<none>");
    CHECK(client.GaveUp());
    const auto start = steady_clock::now();
    CHECK(First(client, "x", "echo") == "<none>");
    CHECK(steady_clock::now() - start < milliseconds(100));  // 停用後立即回傳
    CHECK(!events.empty() &&
          events.back().find("暫停使用") != std::string::npos);
  }

  // 取消後不回應：寬限時間後強制結束
  {
    Client client(MakeOptions());
    const auto start = steady_clock::now();
    CHECK(First(client, "", "hang", [] { return true; }) == "<none>");
    const auto elapsed = steady_clock::now() - start;
    CHECK(elapsed >= milliseconds(400) && elapsed < seconds(3));
    CHECK(First(client, "alive", "echo") == "alive");  // 重新啟動
  }

  // 逾時
  {
    Client client(MakeOptions());
    const auto start = steady_clock::now();
    CHECK(First(client, "", "hang", nullptr, milliseconds(300)) == "<none>");
    CHECK(steady_clock::now() - start < seconds(3));
  }

  // 請求進行中解構：等待中的請求立刻返回
  {
    auto client = std::make_unique<Client>(MakeOptions());
    CHECK(First(*client, "warm", "echo") == "warm");
    std::string result;
    std::thread caller([&] { result = First(*client, "", "hang"); });
    std::this_thread::sleep_for(milliseconds(200));
    const auto start = steady_clock::now();
    client.reset();
    caller.join();
    CHECK(result == "<none>");
    CHECK(steady_clock::now() - start < seconds(3));
  }

  // 找不到推理程式：停用，不會卡住
  {
    Client::Options options = MakeOptions();
    options.exe = g_host.parent_path() / "missing.exe";
    Client client(options);
    CHECK(First(client, "x", "echo") == "<none>");
    CHECK(client.GaveUp());
  }

  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
