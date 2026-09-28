// ime::Controller 的測試：用真的
// librime（注音方案）與假的平台、假的模型，模擬打字
//   TestController.exe <Rime 共用資料夾> <測試用的使用者資料夾>
#include "../../core/ime/controller.h"
#include "../../core/ime/keys.h"
#include "../../core/ime/prediction_engine.h"
#include "../../core/base/utf8.h"
#include "../../core/llm/ContextHistory.h"
#include "../../core/llm/LLMProvider.h"
#include "../../core/personal/PersonalRefiner.h"

#include <rime_api.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace ime;

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static std::string U8(const std::wstring& s) {
  return utf8::FromWide(s);
}

static bool IsCJK(wchar_t c) {
  return c >= 0x3400 && c <= 0x9FFF;
}

class TestFrontend : public Frontend {
 public:
  std::map<uint64_t, SessionState> sessions;
  std::mutex mutex;
  std::atomic<int> refreshes{0};
  std::atomic<int> hides{0};
  SessionState* Session(uint64_t id) override {
    auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : &it->second;
  }
  void Refresh(uint64_t) override { ++refreshes; }
  void HideCandidates() override { ++hides; }
  std::mutex& ApiMutex() override { return mutex; }
  std::wstring ContextKey(uint64_t) override { return L"test.exe|1|視窗"; }
  void Redeploy() override {}
  bool ReleaseSessions() override { return true; }
};

class FakeModel : public LLMProvider {
 public:
  std::vector<std::wstring> predictions = {L"很好"};
  std::atomic<int> calls{0};
  std::mutex mutex;
  std::wstring last_context;
  bool LoadConfig(const std::string&) override { return true; }
  std::vector<std::wstring> PredictCandidates(const std::wstring& context,
                                              const std::wstring&,
                                              size_t) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_context = context;
    }
    ++calls;
    return predictions;
  }
  bool IsAvailable() const override { return true; }
  std::string GetProviderName() const override { return "fake"; }
};

// 模擬 Weasel 的一次按鍵與回應
class Typist {
 public:
  Typist(RimeApi* api,
         Controller* controller,
         TestFrontend* frontend,
         uint64_t id)
      : api_(api), c_(controller), f_(frontend), id_(id) {}

  SessionState& State() { return f_->sessions[id_]; }
  RimeSessionId Rime() { return State().session_id; }

  KeyResult Key(int keycode, int mask = 0) {
    std::lock_guard<std::mutex> lock(f_->mutex);
    KeyResult r = c_->ProcessKey(id_, keycode, mask);
    if (r.respond)
      RespondLocked();
    // 放開
    KeyResult up = c_->ProcessKey(id_, keycode, mask | mod::kRelease);
    if (up.respond)
      RespondLocked();
    return r;
  }
  void Type(const std::string& keys) {
    for (char k : keys)
      Key((unsigned char)k);
  }
  void ShiftTap() {
    std::lock_guard<std::mutex> lock(f_->mutex);
    c_->ProcessKey(id_, key::kShiftL, 0);
    if (c_->ProcessKey(id_, key::kShiftL, mod::kShift | mod::kRelease).respond)
      RespondLocked();
  }
  void Respond() {
    std::lock_guard<std::mutex> lock(f_->mutex);
    RespondLocked();
  }
  bool Composing() {
    RIME_STRUCT(RimeStatus, status);
    bool composing = false;
    if (api_->get_status(Rime(), &status)) {
      composing = !!status.is_composing;
      api_->free_status(&status);
    }
    return composing;
  }
  std::string Input() {
    const char* raw = api_->get_input(Rime());
    return raw ? raw : "";
  }
  // 送出過的文字（依序）
  std::vector<std::wstring> commits;
  Preedit preedit;  // 最後一次的組字區（注音預覽，混打時接上混打內容）

 private:
  void RespondLocked() {
    for (auto& text : c_->TakeCommits(id_))
      commits.push_back(text);
    const bool composing = Composing();
    RIME_STRUCT(RimeContext, ctx);
    const bool has_ctx = !!api_->get_context(Rime(), &ctx);
    c_->UpdateComposition(id_, composing, has_ctx ? &ctx : nullptr);
    bool has = false;
    preedit = Preedit();
    if (has_ctx) {
      if (composing)
        has = c_->PreviewPreedit(id_, ctx, false, &preedit);
      api_->free_context(&ctx);
    }
    if (State().mixed_active())
      preedit = Controller::WithMixedText(State(), has ? &preedit : nullptr);
  }

  RimeApi* api_;
  Controller* c_;
  TestFrontend* f_;
  uint64_t id_;
};

template <typename F>
static bool WaitFor(F cond, int ms = 3000) {
  for (int i = 0; i < ms / 10; ++i) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: TestController <shared_data_dir> <user_data_dir>\n");
    return 2;
  }
  const std::string shared = argv[1], user = argv[2];
  fs::create_directories(user);
  // 只部署注音方案，第一次跑比較快（之後沿用 build 資料夾）
  {
    std::ofstream out(fs::path(user) / "default.custom.yaml", std::ios::binary);
    out << "patch:\n  schema_list:\n    - schema: bopomofo\n";
  }
  // 標點測試用的鍵：各版注音方案的標點設定不同（3.1 版起 Shift+數字是選字鍵），
  // 固定讓 ~ 直接送出「～」
  {
    std::ofstream out(fs::path(user) / "bopomofo.custom.yaml",
                      std::ios::binary);
    out << "patch:\n"
           "  \"punctuator/full_shape/~\": { commit: \"\xEF\xBD\x9E\" }\n"
           "  \"punctuator/half_shape/~\": { commit: \"\xEF\xBD\x9E\" }\n";
  }
  RimeApi* api = rime_get_api();
  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = shared.c_str();
  traits.user_data_dir = user.c_str();
  traits.prebuilt_data_dir = shared.c_str();
  traits.app_name = "rime.test_controller";
  traits.distribution_name = "TestController";
  traits.distribution_code_name = "test";
  traits.distribution_version = "0";
  api->setup(&traits);
  api->initialize(nullptr);
  const auto t0 = std::chrono::steady_clock::now();
  if (api->start_maintenance(False))
    api->join_maintenance_thread();
  std::printf("  deploy: %lld ms\n",
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count());

  TestFrontend frontend;
  Controller::Options options;
  options.user_dir = user;
  options.shared_dir = shared;
  options.version = "test";
  ContextHistory history;
  Controller controller(api, &frontend, options);
  controller.SetContextHistory(&history);

  const uint64_t id = 1;
  SessionState& ss = frontend.sessions[id];
  ss.session_id = api->create_session();
  CHECK(ss.session_id != 0);
  CHECK(api->select_schema(ss.session_id, "bopomofo"));
  Typist t(api, &controller, &frontend, id);

  // 注音：打一個字，預覽顯示轉好的字；應用程式要求送出時送出並記進前文
  {
    t.Type("5j4");
    CHECK(t.Composing());
    CHECK(t.preedit.text.size() == 1 && IsCJK(t.preedit.text[0]));
    std::printf("  5j4 -> %s\n", U8(t.preedit.text).c_str());
    const std::wstring shown = t.preedit.text;
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      controller.CommitComposition(id);
    }
    t.Respond();
    CHECK(t.commits.size() == 1 && t.commits.back() == shown);
    CHECK(history.GetRecentContext(10) == shown);
    CHECK(!t.Composing());
    t.commits.clear();
  }

  // Backspace：已打聲調的音節整個刪掉，還在拼的一次刪一鍵
  {
    t.Type("5j4");
    t.Key(key::kBackSpace);
    CHECK(!t.Composing());
    t.Type("5j");
    t.Key(key::kBackSpace);
    CHECK(t.Composing() && t.Input() == "5");
    t.Key(key::kEscape);
    CHECK(!t.Composing());
  }

  // 中英混打：組字中按 Shift 切英文，打的英文接在轉好的中文後面，再按 Shift
  // 回中文，Enter 一起送出
  {
    t.Type("5j4");
    const std::wstring first = t.preedit.text;
    t.ShiftTap();
    CHECK(ss.mixed_english && ss.mixed_text == first);
    t.Type("ok");
    CHECK(ss.mixed_text == first + L"ok");
    CHECK(t.preedit.text == first + L"ok");
    t.ShiftTap();
    CHECK(!ss.mixed_english);
    t.Type("5j4");
    CHECK(t.Composing());
    CHECK(t.preedit.text.size() == first.size() + 3);  // 混打內容 + 組字
    CHECK(t.commits.empty());                          // 還沒送出
    t.Key(key::kReturn);
    CHECK(t.commits.size() == 1);
    if (!t.commits.empty()) {
      const std::wstring& c = t.commits.back();
      std::printf("  mixed -> %s\n", U8(c).c_str());
      CHECK(c.size() == 4 && c.compare(0, 3, first + L"ok") == 0 &&
            IsCJK(c[3]));
    }
    CHECK(!ss.mixed_active());
    t.commits.clear();
  }

  // 組字中打標點：留在組字區，不直接送出
  {
    t.Type("5j4");
    t.Key('~', mod::kShift);
    CHECK(t.commits.empty());
    CHECK(ss.mixed_text.size() == 2 && IsCJK(ss.mixed_text[0]) &&
          !IsCJK(ss.mixed_text[1]));
    std::printf("  punct -> %s\n", U8(ss.mixed_text).c_str());
    t.Key(key::kReturn);
    CHECK(t.commits.size() == 1 && t.commits.back().size() == 2);
    t.commits.clear();
  }

  // 組字中 Shift+字母：打大寫英文接在轉好的中文後面（不是
  // alternative_select_keys 的選字），仍留在中文；放開 Shift 不切換中英
  {
    t.Type("5j4");
    const std::wstring first = t.preedit.text;
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      controller.ProcessKey(id, key::kShiftL, 0);
    }
    t.Key('A', mod::kShift);
    t.Key('B', mod::kShift);
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      controller.ProcessKey(id, key::kShiftL, mod::kShift | mod::kRelease);
    }
    t.Respond();
    CHECK(ss.mixed_text == first + L"AB");
    CHECK(!ss.mixed_english);
    CHECK(t.preedit.text == first + L"AB");
    CHECK(t.commits.empty());
    t.Type("5j4");
    CHECK(t.Composing());
    t.Key(key::kReturn);
    CHECK(t.commits.size() == 1);
    if (!t.commits.empty()) {
      const std::wstring& c = t.commits.back();
      std::printf("  shift+letter -> %s\n", U8(c).c_str());
      CHECK(c.size() == 4 && c.compare(0, 3, first + L"AB") == 0 &&
            IsCJK(c[3]));
    }
    CHECK(!ss.mixed_active());
    t.commits.clear();
  }

  // 逐字選字：←/→ 框住音節
  {
    t.Type("rup wu0 ");  // ㄐㄧㄣ ㄊㄧㄢ
    CHECK(t.preedit.text.size() == 2);
    std::printf("  rup wu0 -> %s\n", U8(t.preedit.text).c_str());
    t.Key(key::kLeft);
    CHECK(ss.focus == 1);
    t.Key(key::kLeft);
    CHECK(ss.focus == 0);
    t.Key(key::kEnd);
    CHECK(ss.focus == -1);
    t.Key(key::kEscape);
    CHECK(!t.Composing());
  }

  // LLM：打字停頓後出現補全候選，Tab 送出，接著預測下一個詞；Esc 收起
  auto model_owned = std::make_unique<FakeModel>();
  FakeModel* model = model_owned.get();
  controller.SetPredictionModel(std::move(model_owned));
  {
    t.Type("5j4");
    const std::wstring first = t.preedit.text;
    CHECK(WaitFor([&] {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      return controller.ShowingPredictions();
    }));
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      const PredictionSet set = controller.Predictions();
      CHECK(set.candidates.size() == 1 && set.candidates[0] == first + L"很好");
    }
    // 引擎先寫入候選、再拿鎖通知候選窗：等通知送到
    CHECK(WaitFor([&] { return frontend.refreshes > 0; }));
    const int calls = model->calls;
    t.Key(key::kTab);
    CHECK(t.commits.size() == 1 && t.commits.back() == first + L"很好");
    CHECK(!t.Composing());
    CHECK(history.GetRecentContext(100).size() >= 3);
    // 送出後預測下一個詞
    CHECK(WaitFor([&] { return model->calls > calls; }));
    CHECK(WaitFor([&] {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      return controller.ShowingPredictions();
    }));
    {
      std::lock_guard<std::mutex> lock(model->mutex);
      CHECK(model->last_context.size() >= 3 &&
            model->last_context.compare(model->last_context.size() - 3, 3,
                                        first + L"很好") == 0);
    }
    const int hides = frontend.hides;
    const KeyResult esc = t.Key(key::kEscape);
    CHECK(esc.handled && !esc.respond);
    CHECK(frontend.hides > hides);
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      CHECK(!controller.ShowingPredictions() && !controller.PredictionMode());
    }
    t.commits.clear();
  }

  // ` 鍵：組字中立即預測（不等停頓）；雙擊清空前文
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    t.Type("5j");
    const KeyResult grave = t.Key(key::kGrave);
    CHECK(grave.handled && !grave.respond);
    CHECK(WaitFor(
        [&] {
          std::lock_guard<std::mutex> lock(frontend.mutex);
          return controller.ShowingPredictions();
        },
        1000));
    CHECK(history.GetSize() > 0);
    t.Key(key::kGrave);  // 500ms 內第二次
    CHECK(history.GetSize() == 0);
    t.Key(key::kEscape);
    t.Key(key::kEscape);
    CHECK(!t.Composing());
  }

  // 選字統計寫在使用者資料夾
  CHECK(fs::exists(fs::path(user) / "weasel_stats.txt"));

  // 精煉的拆解結果：只收請它看的片段，部分接起來要等於原片段（改了字的不收）
  {
    const auto splits = PersonalRefiner::ParseSplits(
        L"好的，結果如下：\n"
        L"拆解\t我明天下午要去台北開會\t我 明天下午 要去 台北 開會\n"
        L"- 拆解\t謝謝大家的幫忙\t謝謝／大家的／幫忙\n"
        L"拆解\t今天天氣很好\t今天 天汽 很好\n"
        L"拆解\t沒請它看的片段\t沒請 它看的 片段\n"
        L"拆解\t我明天下午要去台北開會\t我明天 下午\n"
        L"拆解\t應該是設定檔的路徑\t應該 是 設定檔 的 路徑\n"  // 單獨的字接到前一個部分
        L"拆解\t一石二鳥\t一石二鳥\n",                          // 沒拆
        {L"我明天下午要去台北開會", L"謝謝大家的幫忙", L"今天天氣很好", L"應該是設定檔的路徑",
         L"一石二鳥"});
    CHECK(splits.size() == 3);
    if (splits.size() == 3) {
      CHECK(splits[0].first == L"我明天下午要去台北開會");
      CHECK(splits[0].second ==
            (std::vector<std::wstring>{L"我", L"明天下午", L"要去", L"台北", L"開會"}));
      CHECK(splits[1].second == (std::vector<std::wstring>{L"謝謝", L"大家的", L"幫忙"}));
      CHECK(splits[2].second == (std::vector<std::wstring>{L"應該是", L"設定檔的", L"路徑"}));
    }
  }

  {
    std::lock_guard<std::mutex> lock(frontend.mutex);
    api->destroy_session(ss.session_id);
  }
  api->finalize();
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
