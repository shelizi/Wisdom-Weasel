// LLM 的 HTTP（core/net）測試：搭配 mock_server.py，以 run.bat 編譯執行
#include "../../core/llm/LLMProvider.h"
#include "../../core/net/http.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static std::string g_base;

static int ConnectionCount() {
  net::Request request;
  request.url = g_base + "/count";
  net::Response response;
  std::string error;
  if (!net::Fetch(request, &response, &error))
    std::printf("  GET /count failed: %s\n", error.c_str());
  const size_t p = response.body.find(':');
  return p == std::string::npos ? -1 : atoi(response.body.c_str() + p + 1);
}

int main(int argc, char** argv) {
  g_base = std::string("http://127.0.0.1:") + (argc > 1 ? argv[1] : "18765");
  using Clock = std::chrono::steady_clock;

  // 預測：OpenAI 相容 API，連續請求重用同一條連線
  {
    OpenAICompatibleProvider provider;
    provider.ConfigureDirect(g_base + "/v1/chat/completions", "k", "m", L"",
                             false, 0);
    CHECK(provider.IsAvailable());
    const int before = ConnectionCount();
    std::vector<std::wstring> first;
    for (int i = 0; i < 3; ++i)
      first = provider.PredictCandidates(L"前文", L"", 5);
    CHECK(first.size() == 4);
    if (first.size() == 4) {
      CHECK(first[0] == L"候選一");
      CHECK(first[3] == L"候選四");  // 「4. 」編號去掉
    }
    const int after = ConnectionCount();
    // 三次預測重用同一條連線，加上一次查詢，最多多兩條連線
    // （WinHTTP 的連線池整個行程共用，通常一條都不多；libcurl 每個 handle
    // 各自一份）
    CHECK(before > 0 && after - before <= 2);
    std::printf("  new connections for 3 predictions: %d\n", after - before);
  }

  // 單次 POST
  {
    std::string response;
    unsigned long status = 0;
    bool timed_out = true;
    CHECK(LLMHttpPostJson(g_base + "/v1/chat/completions", "k",
                          "{\"model\":\"x\"}", &response, 5000, &status,
                          &timed_out));
    CHECK(status == 200 && !timed_out);
    bool found = false;
    CHECK(LLMExtractChatContent(response, &found).find(L"候選二") !=
              std::wstring::npos &&
          found);
  }

  // HTTP 錯誤：回傳 false、狀態碼與錯誤內容
  {
    std::string response;
    unsigned long status = 0;
    CHECK(!LLMHttpPostJson(g_base + "/error", "secret", "{}", &response, 5000,
                           &status, nullptr));
    CHECK(status == 401);
    CHECK(response.find("bad key: Bearer secret") != std::string::npos);
  }

  // 總時間上限：伺服器一直送空白保持連線
  {
    std::string response;
    bool timed_out = false;
    const auto t0 = Clock::now();
    CHECK(!LLMHttpPostJson(g_base + "/slow", "", "{}", &response, 800, nullptr,
                           &timed_out));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0)
            .count();
    CHECK(timed_out);
    CHECK(ms < 3000);
    std::printf("  total timeout after %lld ms\n", (long long)ms);
  }

  // SSE 串流：保持連線的註解不算事件，[DONE] 結束
  {
    std::vector<std::string> events;
    unsigned long status = 0;
    bool timed_out = true;
    std::string error_body;
    CHECK(LLMHttpPostStream(
        g_base + "/v1/chat/completions", "k", "{\"stream\":true}",
        [&](const std::string& data) {
          events.push_back(data);
          return true;
        },
        5000, &status, &timed_out, &error_body));
    CHECK(status == 200 && !timed_out);
    CHECK(events.size() == 3);
    if (events.size() == 3)
      CHECK(events[2].find("丙") != std::string::npos);
  }

  // 串流：呼叫端中止
  {
    int seen = 0;
    CHECK(!LLMHttpPostStream(
        g_base + "/v1/chat/completions", "", "{\"stream\":true}",
        [&](const std::string&) { return ++seen < 1; }, 5000, nullptr, nullptr,
        nullptr));
    CHECK(seen == 1);
  }

  // 串流：取消（LLMCancelScope）
  {
    int seen = 0;
    LLMCancelScope cancel([&] { return seen >= 1; });
    CHECK(!LLMHttpPostStream(
        g_base + "/v1/chat/completions", "", "{\"stream\":true}",
        [&](const std::string&) {
          ++seen;
          return true;
        },
        5000, nullptr, nullptr, nullptr));
    CHECK(seen == 1);
  }

  // 串流：送了一個事件後就不再送東西 → 閒置逾時
  {
    bool timed_out = false;
    const auto t0 = Clock::now();
    CHECK(!LLMHttpPostStream(
        g_base + "/stall", "", "{}", [](const std::string&) { return true; },
        700, nullptr, &timed_out, nullptr));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0)
            .count();
    CHECK(timed_out);
    CHECK(ms < 5000);
    std::printf("  stalled stream gave up after %lld ms\n", (long long)ms);
  }

  // 串流：HTTP 錯誤時讀出錯誤內容
  {
    unsigned long status = 0;
    std::string error_body;
    CHECK(!LLMHttpPostStream(
        g_base + "/error", "k", "{}", [](const std::string&) { return true; },
        5000, &status, nullptr, &error_body));
    CHECK(status == 401);
    CHECK(error_body.find("bad key") != std::string::npos);
  }

  // 連不上：回傳 false，不會卡住
  {
    std::string response;
    const auto t0 = Clock::now();
    CHECK(!LLMHttpPostJson("http://127.0.0.1:1/x", "", "{}", &response, 2000,
                           nullptr, nullptr));
    CHECK(Clock::now() - t0 < std::chrono::seconds(5));
  }

  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
