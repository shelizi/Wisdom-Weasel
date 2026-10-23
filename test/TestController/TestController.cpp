// ime::Controller 的測試：用真的
// librime（注音方案）與假的平台、假的模型，模擬打字
//   TestController.exe <Rime 共用資料夾> <測試用的使用者資料夾>
#include "../../core/ime/controller.h"
#include "../../core/ime/keys.h"
#include "../../core/ime/prediction_engine.h"
#include "../../core/ime/script_convert.h"
#include "../../core/base/utf8.h"
#include "../../core/llm/ContextHistory.h"
#include "../../core/llm/LLMProvider.h"
#include "../../core/personal/PersonalRefiner.h"
#include "../../core/settings/bench.h"

#include <PersonalCrypto.h>

#include <rime_api.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// 簡體在地化的假模型：回傳固定的句子，可以慢一點（測送出時的等待）
class FakeLocalizer : public LLMProvider {
 public:
  std::wstring output;
  int delay_ms = 0;
  std::atomic<int> calls{0};
  bool LoadConfig(const std::string&) override { return true; }
  std::vector<std::wstring> PredictCandidates(const std::wstring&,
                                              const std::wstring&,
                                              size_t) override {
    return {};
  }
  std::wstring LocalizeText(const std::wstring&, const std::wstring&) override {
    ++calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    return output;
  }
  bool IsAvailable() const override { return true; }
  std::string GetProviderName() const override { return "fake-localize"; }
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
    out << "patch:\n  schema_list:\n    - schema: bopomofo\n    - schema: "
           "bopomofo_express\n";
  }
  // 標點測試用的鍵：各版注音方案的標點設定不同（3.1 版起 Shift+數字是選字鍵），
  // 固定讓 ~ 直接送出「～」
  {
    std::ofstream out(fs::path(user) / "bopomofo.custom.yaml",
                      std::ios::binary);
    out << "patch:\n"
           "  \"punctuator/full_shape/~\": { commit: \"\xEF\xBD\x9E\" }\n"
           "  \"punctuator/half_shape/~\": { commit: \"\xEF\xBD\x9E\" }\n"
           // 與設定的「長句的整句候選」相同：候選窗前面列出多個整句
           "  translator/max_sentences: 5\n"
           "  translator/sentence_cutoff_threshold: 100\n";
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

  // 簡體輸出：Rime 共用資料的 OpenCC 字典（tw2s 只換字，tw2sp 連用詞）
  {
    ScriptConverter chars, phrases;
    CHECK(chars.Load(fs::path(shared) / "opencc" / "tw2s.json"));
    CHECK(phrases.Load(fs::path(shared) / "opencc" / "tw2sp.json"));
    const std::wstring text = L"裡面的軟體和滑鼠都很好用";
    const std::wstring a = chars.Convert(text), b = phrases.Convert(text);
    std::printf("  tw2s  -> %s\n  tw2sp -> %s\n", U8(a).c_str(), U8(b).c_str());
    CHECK(a == L"里面的软体和滑鼠都很好用");
    CHECK(b == L"里面的软件和鼠标都很好用");
    ScriptConverter missing;
    CHECK(!missing.Load(fs::path(shared) / "opencc" / "no-such.json"));
    CHECK(missing.Convert(text) == text);
  }

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

  // 省略聲調的前字已成字：刪光後字的未完成注音，不得退回注音。
  for (bool soft_cursor : {false, true}) {
    api->set_option(t.Rime(), "soft_cursor", soft_cursor);
    t.Type("sucl");  // ㄋㄧ ㄏㄠ：前字未打聲調，但已因下一音節成字。
    const std::wstring first = t.preedit.text.substr(0, 1);
    CHECK(first.size() == 1 && IsCJK(first.front()));
    CHECK(t.preedit.text == first + L"ㄏㄠ");
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "suc" && t.preedit.text == first + L"ㄏ");
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "su" && t.preedit.text.size() == 1 &&
          IsCJK(t.preedit.text.front()));
    CHECK(t.preedit.cursor == 1);
    t.Respond();  // 重畫組字區也必須保留成字狀態。
    CHECK(t.preedit.text.size() == 1 && IsCJK(t.preedit.text.front()));
    t.Type("cl3");
    CHECK(t.preedit.text.size() == 2 && IsCJK(t.preedit.text.back()));
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "su" && t.preedit.text.size() == 1 &&
          IsCJK(t.preedit.text.front()));
    t.Key(key::kBackSpace);
    CHECK(t.Input().empty() && !t.Composing());
    t.Type("su");  // 全新輸入仍是尚未完成的注音。
    CHECK(t.preedit.text == L"ㄋㄧ");
    t.Key(key::kEscape);
    CHECK(!t.Composing());
    t.Type("sucl");
    t.Key(key::kBackSpace);
    t.Key(key::kBackSpace);
    const std::wstring shown = t.preedit.text;
    t.Key(key::kReturn);
    CHECK(!t.Composing() && t.commits.size() == 1 && t.commits.back() == shown);
    t.commits.clear();
  }
  api->set_option(t.Rime(), "soft_cursor", false);

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
    CHECK(t.preedit.cursor == 1 && t.preedit.sel_start == 1 &&
          t.preedit.sel_end == 2);
    t.Key(key::kLeft);
    CHECK(ss.focus == 0);
    CHECK(t.preedit.cursor == 0 && t.preedit.sel_start == 0 &&
          t.preedit.sel_end == 1);
    t.Key(key::kEnd);
    CHECK(ss.focus == -1);
    t.Key(key::kEscape);
    CHECK(!t.Composing());
  }

  // 依游標刪整個音節：Backspace 刪前字，Delete 刪後字，繼續輸入沒有殘音。
  for (bool soft_cursor : {false, true}) {
    api->set_option(t.Rime(), "soft_cursor", soft_cursor);
    for (int erase_key : {key::kBackSpace, key::kDelete}) {
      for (int index = 0; index < 3; ++index) {
        t.Type("5j4up 2j/4");  // ㄓㄨˋ ㄧㄣˉ ㄉㄨㄥˋ
        const std::wstring shown = t.preedit.text;
        CHECK(shown.size() == 3);
        t.Key(key::kHome);
        const int cursor = index + (erase_key == key::kBackSpace ? 1 : 0);
        for (int i = 0; i < cursor; ++i)
          t.Key(key::kRight);
        CHECK(ss.focus == (cursor == 3 ? -1 : cursor));
        CHECK(t.preedit.cursor == cursor);
        if (cursor < 3)
          CHECK(t.preedit.sel_start == cursor &&
                t.preedit.sel_end == cursor + 1);
        const std::string expected[] = {"up 2j/4", "5j42j/4", "5j4up "};
        t.Key(erase_key);
        CHECK(t.Input() == expected[index]);
        CHECK(t.commits.empty());
        CHECK(t.preedit.text ==
              shown.substr(0, index) + shown.substr(index + 1));
        CHECK(t.preedit.cursor == index);
        if (index < 2) {
          CHECK(ss.focus == index);
          t.Key(key::kEnd);
        } else {
          CHECK(ss.focus == -1);
        }
        const std::string remaining = t.Input();
        t.Type("su3");  // 再輸入 ㄋㄧˇ
        CHECK(t.Input() == remaining + "su3");
        CHECK(t.preedit.text.size() == 3);
        for (int i = 0; i < 5 && t.Composing(); ++i)
          t.Key(key::kEscape);
        CHECK(!t.Composing());
      }
    }
    // 句首 Backspace 不刪字；句尾 Delete 不刪字；Backspace 連刪到空。
    t.Type("5j4up 2j/4");
    t.Key(key::kHome);
    const std::wstring before = t.preedit.text;
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "5j4up 2j/4" && t.preedit.text == before);
    CHECK(t.preedit.cursor == 0 && ss.focus == 0);
    t.Key(key::kEnd);
    t.Key(key::kDelete);
    CHECK(t.Input() == "5j4up 2j/4" && t.preedit.text == before);
    t.Key(key::kBackSpace);
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "5j4");
    t.Key(key::kBackSpace);
    CHECK(t.Input().empty() && !t.Composing());
    t.Type("5j");
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "5");
    t.Key(key::kEscape);
    // 省略聲調的前字也依 Rime 分詞刪完整音節。
    t.Type("rupwu0wu0fu4cp3cl3");
    CHECK(ss.preview_lens.size() == 6);
    t.Key(key::kHome);
    t.Key(key::kRight);
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "wu0wu0fu4cp3cl3");
    CHECK(t.preedit.cursor == 0 && ss.focus == 0);
    for (int i = 0; i < 5 && t.Composing(); ++i)
      t.Key(key::kEscape);
    CHECK(!t.Composing());
  }
  api->set_option(t.Rime(), "soft_cursor", false);

  for (bool soft_cursor : {false, true}) {
    api->set_option(t.Rime(), "soft_cursor", soft_cursor);
    // 在未完成注音前的顯示游標按 Delete，必須刪右邊的符號，不能刪句尾。
    t.Type("5j4up");
    t.Key(key::kHome);
    t.Key(key::kRight);
    CHECK(t.preedit.cursor == 1);
    t.Key(key::kDelete);
    CHECK(t.Input() == "5j4p");
    CHECK(t.preedit.text.substr(1) == L"ㄣ" && t.preedit.cursor == 1);
    t.Key(key::kDelete);
    CHECK(t.Input() == "5j4" && t.preedit.cursor == 1);
    t.Key(key::kDelete);
    CHECK(t.Input() == "5j4");
    t.Type("up ");
    CHECK(t.Input() == "5j4up " && t.preedit.text.size() == 2);
    CHECK(t.commits.empty());
    t.Key(key::kEscape);
    t.Key(key::kEscape);
    // 未完成注音左方的字刪除後，不能把正在拼的音節變成中文字。
    t.Type("5j4up");
    t.Key(key::kHome);
    t.Key(key::kRight);
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "up");
    CHECK(t.preedit.text == L"ㄧㄣ");
    CHECK(t.preedit.cursor == 0);
    t.Key(key::kBackSpace);
    CHECK(t.Input() == "up" && t.preedit.cursor == 0);
    t.Key(key::kDelete);
    CHECK(t.Input() == "p" && t.preedit.text == L"ㄣ" && t.preedit.cursor == 0);
    t.Key(key::kDelete);
    CHECK(!t.Composing() && t.Input().empty());
    t.Key(key::kEscape);
    t.Key(key::kEscape);
  }
  api->set_option(t.Rime(), "soft_cursor", false);

  // 方向鍵移到顯示游標後開始打字：應在游標處插入，未完成注音與後綴都保持可見。
  for (const char* schema : {"bopomofo", "bopomofo_express"}) {
    CHECK(api->select_schema(t.Rime(), schema));
    for (bool soft_cursor : {false, true}) {
      api->set_option(t.Rime(), "soft_cursor", soft_cursor);
      t.Type("5j4up 2j/4");
      const std::wstring shown = t.preedit.text;
      t.Key(key::kLeft);
      t.Key(key::kLeft);
      CHECK(t.preedit.cursor == 1 && ss.focus == 1);
      t.Type("s");
      CHECK(t.Input() == "5j4sup 2j/4");
      CHECK(IsCJK(t.preedit.text.front()) &&
            t.preedit.text.substr(1) == L"ㄋ" + shown.substr(1));
      CHECK(t.preedit.cursor == 2);
      t.Type("u");
      CHECK(t.Input() == "5j4suup 2j/4");
      CHECK(IsCJK(t.preedit.text.front()) &&
            t.preedit.text.substr(1) == L"ㄋㄧ" + shown.substr(1));
      CHECK(t.preedit.cursor == 3);
      t.Type("3");
      CHECK(t.Input() == "5j4su3up 2j/4");
      CHECK(t.preedit.text.size() == 4 && t.preedit.cursor == 2);
      CHECK(ss.preview_input == t.Input() && ss.preview_lens.size() == 4);
      CHECK(t.commits.empty());
      t.Key(key::kLeft);
      CHECK(ss.focus == 1 && t.preedit.cursor == 1);
      t.Key(key::kRight);
      CHECK(ss.focus == 2 && t.preedit.cursor == 2);
      // 再插一次、刪光這次未完成的注音，後面的字仍保持完整。
      const std::wstring before_pending = t.preedit.text;
      t.Type("su");
      CHECK(t.Input() == "5j4su3suup 2j/4");
      CHECK(t.preedit.text.substr(2) == L"ㄋㄧ" + before_pending.substr(2));
      t.Key(key::kBackSpace);
      t.Key(key::kBackSpace);
      CHECK(t.Input() == "5j4su3up 2j/4");
      CHECK(t.preedit.text.size() == 4 && t.preedit.cursor == 2);
      t.Key(key::kRight);
      CHECK(ss.focus == 3 && t.preedit.cursor == 3);
      t.Key(key::kEnd);
      const std::wstring completed = t.preedit.text;
      t.Key(key::kReturn);
      CHECK(t.commits.size() == 1 && t.commits.back() == completed);
      t.commits.clear();
      api->clear_composition(t.Rime());
      t.Respond();
      for (int cursor : {0, 2}) {
        t.Type("5j4up 2j/4");
        const std::string original = t.Input();
        const std::wstring original_text = t.preedit.text;
        for (int i = cursor; i < 3; ++i)
          t.Key(key::kLeft);
        CHECK(t.preedit.cursor == cursor);
        const size_t start = cursor == 0 ? 0 : 6;
        const std::string inserted = cursor == 0 ? "1j4" : "su3";
        const std::wstring pending = cursor == 0 ? L"ㄅ" : L"ㄋ";
        t.Type(inserted.substr(0, 1));
        CHECK(t.Input() == original.substr(0, start) + inserted.substr(0, 1) +
                               original.substr(start));
        CHECK(t.preedit.text.substr(cursor) ==
              pending + original_text.substr(cursor));
        t.Type(inserted.substr(1));
        CHECK(t.Input() ==
              original.substr(0, start) + inserted + original.substr(start));
        CHECK(t.preedit.text.size() == 4 && t.preedit.cursor == cursor + 1);
        t.Key(key::kLeft);
        CHECK(t.preedit.cursor == cursor && ss.focus == cursor);
        t.Key(key::kDelete);
        CHECK(t.Input() == original && t.preedit.cursor == cursor);
        t.Key(key::kEnd);
        for (int i = 0; i < 5 && t.Composing(); ++i)
          t.Key(key::kEscape);
        CHECK(!t.Composing() && t.commits.empty());
      }
      // 省略聲調的前字：句中插入再刪光注音，也不能把前字退回注音。
      t.Type("sucl3");
      t.Key(key::kLeft);
      t.Type("s");
      CHECK(t.Input() == "suscl3" && t.preedit.text.substr(1, 1) == L"ㄋ");
      t.Key(key::kBackSpace);
      CHECK(t.Input() == "sucl3" && t.preedit.text.size() == 2 &&
            IsCJK(t.preedit.text.front()));
      CHECK(t.preedit.cursor == 1);
      t.Key(key::kEnd);
      for (int i = 0; i < 5 && t.Composing(); ++i)
        t.Key(key::kEscape);
      CHECK(!t.Composing());
    }
  }
  CHECK(api->select_schema(t.Rime(), "bopomofo"));
  api->set_option(t.Rime(), "soft_cursor", false);

  // 逐字選字後沒換字就回到句尾：恢復原本的整句與候選清單（框選時選好的前段要放開）
  {
    // Rime 目前這段的前 10 個候選（不管選單有沒有展開）
    auto candidates = [&] {
      std::vector<std::string> list;
      RimeCandidateListIterator it = {0};
      if (api->candidate_list_begin(t.Rime(), &it)) {
        while (list.size() < 10 && api->candidate_list_next(&it))
          list.push_back(it.candidate.text ? it.candidate.text : "");
        api->candidate_list_end(&it);
      }
      return list;
    };
    t.Type("rup wu0 wu0 fu4cp3cl3");  // 今天天氣很好（不按空白：選單照常顯示）
    const std::wstring before = t.preedit.text;
    const auto list = candidates();
    t.Key(key::kLeft);
    t.Key(key::kLeft);
    t.Key(key::kLeft);
    CHECK(ss.focus == 3);
    t.Key(key::kEnd);
    CHECK(ss.focus == -1);
    CHECK(t.preedit.text == before);
    CHECK(candidates() == list);
    // → 一路移到句尾也一樣
    t.Key(key::kLeft);
    t.Key(key::kLeft);
    t.Key(key::kRight);
    t.Key(key::kRight);
    CHECK(ss.focus == -1);
    CHECK(candidates() == list);
    // 框選時換過字：回到句尾後保留換好的字
    t.Key(key::kLeft);
    t.Key(key::kLeft);  // 框住「很」
    CHECK(ss.focus == 4);
    const std::wstring framed = t.preedit.text;
    int other = -1;
    {
      RIME_STRUCT(RimeContext, ctx);
      if (api->get_context(t.Rime(), &ctx)) {
        for (int i = 0; i < ctx.menu.num_candidates && other < 0; ++i)
          if (std::string(ctx.menu.candidates[i].text) != "很")
            other = i;
        api->free_context(&ctx);
      }
    }
    CHECK(other >= 0);
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      controller.SelectCandidate(1, (size_t)other);
    }
    t.Respond();
    const std::wstring changed = t.preedit.text;
    CHECK(changed != framed && changed.size() == framed.size());
    CHECK(changed.substr(4, 1) !=
          framed.substr(4, 1));  // 未手動確認的前後字可以重新造句。
    t.Key(key::kLeft);
    t.Key(key::kEnd);
    CHECK(ss.focus == -1);
    CHECK(t.preedit.text == changed);
    // 換過的「很」留在快取：刪它後面的末字仍保留選好的字。
    t.Key(key::kBackSpace);
    CHECK(t.preedit.text == changed.substr(0, changed.size() - 1));
    CHECK(t.Input() == "rup wu0 wu0 fu4cp3");
    for (int i = 0; i < 5 && t.Composing(); ++i)
      t.Key(key::kEscape);
    CHECK(!t.Composing());
  }

  // 手動選字逐字鎖定：先改後字再改前字、繼續打字、刪除與送出都保留選好的字。
  {
    auto choose_other = [&](int index) {
      t.Key(key::kHome);
      for (int i = 0; i < index; ++i)
        t.Key(key::kRight);
      CHECK(ss.focus == index);
      int pick = -1;
      std::wstring chosen;
      RIME_STRUCT(RimeContext, ctx);
      if (api->get_context(t.Rime(), &ctx)) {
        for (int i = 0; i < ctx.menu.num_candidates; ++i) {
          const std::wstring word = utf8::ToWide(ctx.menu.candidates[i].text);
          if (word.size() == 1 && word != ss.preview_units[index]) {
            pick = i;
            chosen = word;
            break;
          }
        }
        api->free_context(&ctx);
      }
      CHECK(pick >= 0);
      if (pick >= 0) {
        std::lock_guard<std::mutex> lock(frontend.mutex);
        controller.SelectCandidate(1, (size_t)pick);
      }
      t.Respond();
      CHECK(!chosen.empty() && t.preedit.text.substr(index, 1) == chosen);
      CHECK(t.commits.empty());
      return chosen;
    };
    for (const char* schema : {"bopomofo", "bopomofo_express"}) {
      CHECK(api->select_schema(t.Rime(), schema));
      for (bool soft_cursor : {false, true}) {
        api->set_option(t.Rime(), "soft_cursor", soft_cursor);
        t.Type("rup wu0 wu0 fu4cp3cl3");
        CHECK(ss.manual_choices.empty());  // 自動選字不鎖定。
        const std::wstring later = choose_other(4);
        CHECK(ss.manual_choices.size() == 1);
        const std::wstring earlier = choose_other(0);
        CHECK(ss.manual_choices.size() == 2);
        CHECK(t.preedit.text.substr(0, 1) == earlier);
        CHECK(t.preedit.text.substr(4, 1) == later);
        // 再改同一個前字：替換它的鎖定，不增加鎖數，也不影響後字。
        const std::wstring replacement = choose_other(0);
        CHECK(ss.manual_choices.size() == 2);
        CHECK(t.preedit.text.substr(0, 1) == replacement &&
              t.preedit.text.substr(4, 1) == later);
        t.Key(key::kHome);
        t.Key(key::kEnd);
        CHECK(t.preedit.text.substr(0, 1) == replacement &&
              t.preedit.text.substr(4, 1) == later);
        t.Type("su3");
        CHECK(ss.manual_choices.size() == 2);
        CHECK(t.preedit.text.substr(0, 1) == replacement &&
              t.preedit.text.substr(4, 1) == later);
        // 刪掉中間一個未鎖字，後方鎖定跟著位移。
        t.Key(key::kHome);
        t.Key(key::kRight);
        t.Key(key::kDelete);
        CHECK(ss.manual_choices.size() == 2);
        CHECK(t.preedit.text.substr(0, 1) == replacement &&
              t.preedit.text.substr(3, 1) == later);
        // 在鎖定字前插入音節，鎖定回到第四字的位置。
        size_t insert_caret = 0;
        for (int i = 0; i < ss.focus; ++i)
          insert_caret += ss.preview_lens[i];
        const std::string before_insert = t.Input();
        t.Type("w");
        CHECK(t.Input() == before_insert.substr(0, insert_caret) + "w" +
                               before_insert.substr(insert_caret));
        CHECK(t.preedit.text.substr(1, 1) == L"ㄊ");
        CHECK(t.preedit.text.substr(0, 1) == replacement &&
              t.preedit.text.substr(4, 1) == later);
        t.Type("u");
        CHECK(t.preedit.text.substr(1, 2) == L"ㄊㄧ");
        CHECK(t.preedit.text.substr(0, 1) == replacement &&
              t.preedit.text.substr(5, 1) == later);
        t.Type("0 ");
        CHECK(t.Input() == before_insert.substr(0, insert_caret) + "wu0 " +
                               before_insert.substr(insert_caret));
        CHECK(ss.manual_choices.size() == 2);
        CHECK(t.preedit.text.substr(0, 1) == replacement &&
              t.preedit.text.substr(4, 1) == later);
        t.Key(key::kHome);
        t.Key(key::kRight);
        t.Key(key::kBackSpace);  // 刪掉第一個鎖定字，只解除那個鎖。
        CHECK(ss.manual_choices.size() == 1);
        CHECK(t.preedit.text.substr(3, 1) == later);
        t.Key(key::kEnd);
        const std::wstring shown = t.preedit.text;
        t.Key(key::kReturn);
        CHECK(t.commits.size() == 1 && t.commits.back() == shown);
        CHECK(ss.manual_choices.empty());
        t.commits.clear();
        t.Type("rup wu0 ");
        CHECK(ss.manual_choices.empty());
        choose_other(0);
        t.Key(key::kEscape);
        if (t.Composing())
          t.Key(key::kEscape);
        CHECK(!t.Composing() && ss.manual_choices.empty());
        // 鍵盤選字也建立鎖；Shift 中英混打送出前恢復游標後的鎖定字。
        t.Type("rup wu0 wu0 fu4cp3cl3");
        const std::wstring keyboard_later = choose_other(4);
        t.Key(key::kHome);
        for (int i = 0; i < 3; ++i)
          t.Key(key::kRight);
        t.Key(key::kDown);
        std::wstring keyboard_word;
        RIME_STRUCT(RimeContext, keyboard_ctx);
        if (api->get_context(t.Rime(), &keyboard_ctx)) {
          const int hl = keyboard_ctx.menu.highlighted_candidate_index;
          if (hl >= 0 && hl < keyboard_ctx.menu.num_candidates)
            keyboard_word = utf8::ToWide(keyboard_ctx.menu.candidates[hl].text);
          api->free_context(&keyboard_ctx);
        }
        CHECK(keyboard_word.size() == 1);
        t.Key(key::kReturn);
        CHECK(t.commits.empty() && ss.manual_choices.size() == 2);
        CHECK(t.preedit.text.substr(3, 1) == keyboard_word &&
              t.preedit.text.substr(4, 1) == keyboard_later);
        t.Key(key::kEnd);
        const std::wstring mixed_shown = t.preedit.text;
        t.Key(key::kHome);
        t.ShiftTap();
        CHECK(ss.mixed_text == mixed_shown);
        t.Type("abc");
        t.Key(key::kReturn);
        CHECK(t.commits.size() == 1 &&
              t.commits.back() == mixed_shown + L"abc");
        CHECK(ss.manual_choices.empty());
        t.commits.clear();
        // 一般整句候選的滑鼠選字，逐字記住使用者確認的字詞。
        t.Type("rup wu0 wu0 fu4cp3cl3");
        const std::wstring sentence = t.preedit.text;
        {
          std::lock_guard<std::mutex> lock(frontend.mutex);
          controller.SelectCandidate(1, 0);
        }
        t.Respond();
        CHECK(ss.manual_choices.size() == 6 && t.preedit.text == sentence);
        const std::wstring new_first = choose_other(0);
        CHECK(ss.manual_choices.size() == 6);
        CHECK(t.preedit.text == new_first + sentence.substr(1));
        {
          std::lock_guard<std::mutex> lock(frontend.mutex);
          controller.CommitComposition(1);
        }
        t.Respond();
        CHECK(t.commits.size() == 1 &&
              t.commits.back() == new_first + sentence.substr(1));
        CHECK(ss.manual_choices.empty());
        t.commits.clear();
        // 在相同按鍵的鎖定音節前插入相同音節，不能誤認成修改鎖定音節。
        t.Type("rup wu0 wu0 fu4cp3cl3");
        const std::wstring same_keys = choose_other(0);
        api->set_caret_pos(t.Rime(), 0);
        t.Respond();
        t.Type("rup ");
        CHECK(ss.manual_choices.size() == 1 && ss.manual_choices[0].start == 4);
        CHECK(t.preedit.text.substr(1, 1) == same_keys);
        for (int i = 0; i < 10 && t.Composing(); ++i)
          t.Key(key::kEscape);
        CHECK(!t.Composing() && ss.manual_choices.empty());
      }
    }
    CHECK(api->select_schema(t.Rime(), "bopomofo"));
    api->set_option(t.Rime(), "soft_cursor", false);
  }

  // 簡體輸出：模型的輸出檢查
  CHECK(
      LLMCheckLocalized(L"我的滑鼠壞了", L"我的鼠标坏了\n（把滑鼠改成鼠标）") ==
      L"我的鼠标坏了");
  CHECK(LLMCheckLocalized(L"我的滑鼠壞了", L"「我的鼠标坏了」") ==
        L"我的鼠标坏了");
  CHECK(LLMCheckLocalized(L"今天天氣很好",
                          L"今天天气很好，我们一起去爬山吧，好不好呢") == L"");
  CHECK(LLMCheckLocalized(L"今天天氣很好", L"  ") == L"");

  // 簡體輸出：送出時用在地化模型的結果（停頓時先改好）；模型太慢就用 OpenCC
  {
    auto fast_owned = std::make_unique<FakeLocalizer>();
    FakeLocalizer* fast = fast_owned.get();
    fast->output = L"今天天气真好";
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      CHECK(controller.SetSimplifiedConversion(
          fs::path(shared) / "opencc" / "tw2sp.json", std::move(fast_owned),
          1500));
      CHECK(controller.SetSimplifiedOutput(true));
    }
    t.commits.clear();
    t.Type("rup wu0 wu0 fu4cp3cl3");                  // 今天天氣很好
    CHECK(WaitFor([&] { return fast->calls > 0; }));  // 停頓後在背景改寫
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      controller.CommitComposition(1);
    }
    t.Respond();
    CHECK(t.commits.size() == 1 && t.commits.back() == L"今天天气真好");
    CHECK(fast->calls == 1);

    auto slow_owned = std::make_unique<FakeLocalizer>();
    FakeLocalizer* slow = slow_owned.get();
    slow->output = L"今天天气真好";
    slow->delay_ms = 1500;
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      CHECK(controller.SetSimplifiedConversion(
          fs::path(shared) / "opencc" / "tw2sp.json", std::move(slow_owned),
          200));
    }
    t.commits.clear();
    t.Type("rup wu0 wu0 fu4cp3cl3");
    // 前面的手動選字測試與重跑會改變 Rime 詞頻；驗證的是超時後轉換當前初稿，
    // 不要求已學習的詞庫仍把「氣」排在「器」前面。
    ScriptConverter fallback;
    CHECK(fallback.Load(fs::path(shared) / "opencc" / "tw2sp.json"));
    const std::wstring expected_fallback = fallback.Convert(t.preedit.text);
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      controller.CommitComposition(1);
    }
    t.Respond();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    std::printf("  simplified fallback after %lld ms -> %s\n",
                (long long)waited,
                U8(t.commits.empty() ? L"" : t.commits.back()).c_str());
    CHECK(t.commits.size() == 1 && t.commits.back() == expected_fallback);
    CHECK(waited < 1200);
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      controller.SetSimplifiedOutput(false);
    }
    CHECK(WaitFor([&] { return slow->calls > 0; }));
    std::this_thread::sleep_for(
        std::chrono::milliseconds(1600));  // 讓慢模型跑完，不影響後面的測試
    t.commits.clear();
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

  // 逐字選字時收起整句的 LLM 候選（否則單字候選後面接著長句，候選窗被撐寬，Tab
  // 也會套用整句）
  {
    t.Type("rup wu0 ");  // ㄐㄧㄣ ㄊㄧㄢ
    CHECK(WaitFor([&] {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      return controller.ShowingPredictions();
    }));
    t.Key(key::kLeft);
    CHECK(ss.focus == 1);
    t.Key(key::kHome);
    t.Key(key::kDelete);
    CHECK(ss.focus == 0 && t.Input() == "wu0 ");
    std::this_thread::sleep_for(
        std::chrono::milliseconds(400));  // 排隊中的補全也不會再出現
    {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      CHECK(!controller.ShowingPredictions() && !controller.PredictionMode());
    }
    t.Key(key::kEscape);
    if (t.Composing())
      t.Key(key::kEscape);
    CHECK(!t.Composing());
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
        L"拆解\t一石二鳥\t一石二鳥\n",                         // 沒拆
        {L"我明天下午要去台北開會", L"謝謝大家的幫忙", L"今天天氣很好",
         L"應該是設定檔的路徑", L"一石二鳥"});
    CHECK(splits.size() == 3);
    if (splits.size() == 3) {
      CHECK(splits[0].first == L"我明天下午要去台北開會");
      CHECK(splits[0].second ==
            (std::vector<std::wstring>{L"我", L"明天下午", L"要去", L"台北",
                                       L"開會"}));
      CHECK(splits[1].second ==
            (std::vector<std::wstring>{L"謝謝", L"大家的", L"幫忙"}));
      CHECK(splits[2].second ==
            (std::vector<std::wstring>{L"應該是", L"設定檔的", L"路徑"}));
    }
  }

  // 模型測試的題目與判定
  {
    using namespace settings_bench;
    std::vector<std::wstring> errors;
    const auto p = ParsePredictCases(
        L"# 註解\n謝謝你的|幫忙\n\n路上小｜心\n少一欄\n", &errors);
    CHECK(p.size() == 2 && p[1].context == L"路上小" && p[1].expected == L"心");
    CHECK(errors.size() == 1);
    errors.clear();
    const auto c = ParseCorrectCases(
        L"|ㄐㄧㄣ ㄊㄧㄢ|今天|今天\n前文，|ㄗㄞˋ ㄐㄧㄢˋ|在見|再見\n", &errors);
    CHECK(errors.empty() && c.size() == 2 && c[0].context.empty() &&
          c[1].context == L"前文，");
    // 候選多接了字也算；只是開頭時要兩個字以上；超出 top 的不算
    CHECK(PredictionHit({L"幫忙了"}, L"幫忙", 1));
    CHECK(PredictionHit({L"快樂"}, L"快樂喔", 1));
    CHECK(!PredictionHit({L"快"}, L"快樂", 5));
    CHECK(PredictionHit({L"心"}, L"心", 1));
    CHECK(!PredictionHit({L"a", L"幫忙"}, L"幫忙", 1) &&
          PredictionHit({L"a", L"幫忙"}, L"幫忙", 5));
    // 校正：不改（空字串）只有初稿本來就對時才算對
    CHECK(CorrectionOk(L"", c[0]) && CorrectionOk(L"今天", c[0]) &&
          !CorrectionOk(L"金天", c[0]));
    CHECK(CorrectionOk(L"再見", c[1]) && !CorrectionOk(L"", c[1]));
  }

  // 精煉借用輸入法的模型：忙碌時等一下再試；審查與拆解共用，不另外載入（模型檔不存在，
  // 真的自己載入就會失敗）
  {
    const fs::path dir = fs::path(user) / "refine_shared";
    fs::remove_all(dir);
    PersonalLexicon lexicon(dir);
    lexicon.Record(L"w", L"亂碼亂碼，我明天下午要去台北開會");
    PersonalRefiner refiner(&lexicon);
    std::vector<std::string> systems;
    int busy = 1;
    refiner.SetSharedChat([&](const LLMLocalModelSpec& spec,
                              const std::string& system, const std::string&,
                              int, std::string* output, std::wstring*) {
      CHECK(spec.model_path == "shared.gguf");
      if (busy-- > 0)
        return PersonalRefiner::SharedChatResult::kBusy;
      systems.push_back(system);
      *output = systems.size() == 1 ? U8(L"刪除\t亂碼亂碼\n")
                                    : U8(L"拆解\t我明天下午要去台北開會\t我 "
                                         L"明天下午 要去 台北 開會\n");
      return PersonalRefiner::SharedChatResult::kOk;
    });
    PersonalRefiner::Config config;
    config.type = "llamacpp";
    config.model_path = "shared.gguf";
    config.interval_days = 0;
    config.clean_rime_memory = false;
    config.shared_busy_wait_ms = 10;
    refiner.Configure(config);
    CHECK(refiner.RunAsync(false));
    CHECK(WaitFor([&] { return !refiner.Running(); }, 10000));
    CHECK(systems.size() == 2);  // 審查、拆解
    CHECK(lexicon.WordScore(L"亂碼亂碼") == 0);
    CHECK(lexicon.WordScore(L"台北") > 0);
    refiner.Stop();
  }

  // 選字紀錄：LLM、評分、校正都沒開時也要查到整句候選（訓練排序器要用），且不進預測模式
  {
    RimeConfig config = {0};
    CHECK(api->config_load_string(&config,
                                  "llm:\n"
                                  "  enabled: false\n"
                                  "  personal: {enabled: false}\n"
                                  "  choice: {log: true}\n"));
    controller.LoadConfig(&config);
    api->config_close(&config);
    t.Type("rupwu0wu0fu4cp3cl3");  // ㄐㄧㄣ ㄊㄧㄢ ㄊㄧㄢ ㄑㄧˋ ㄏㄣˇ ㄏㄠˇ
    CHECK(WaitFor([&] {
      std::lock_guard<std::mutex> lock(frontend.mutex);
      return ss.shown_sentences.size() >= 2;
    }));
    CHECK(!controller.ShowingPredictions());
    if (!ss.shown_sentences.empty())
      std::printf("  choice log top-%zu: %s\n", ss.shown_sentences.size(),
                  U8(ss.shown_sentences[0]).c_str());
    t.Key(key::kEscape);
    CHECK(!t.Composing());

    // 打完馬上送出（背景還沒查）：送出前當場查，紀錄的最後一筆有整句候選
    const fs::path log = fs::path(user) / "personal" / "choice_log.dat";
    fs::remove(log);
    t.commits.clear();
    t.Type("rupwu0wu0fu4cp3cl3");
    t.Key(key::kReturn);
    CHECK(t.commits.size() == 1 && t.commits.back() == L"今天天氣很好");
    std::ifstream in(log, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::string last;
    for (size_t pos = 0; pos + 4 <= data.size();) {
      uint32_t len = 0;
      std::memcpy(&len, data.data() + pos, 4);
      pos += 4;
      if (pos + len > data.size() ||
          !personal_crypto::Unprotect(data.substr(pos, len), &last))
        last.clear();
      pos += len;
    }
    // 時間 \t 應用程式 \t 方式 \t 前文 \t 注音 \t 預設轉換 \t 送出 \t
    // 整句（\x1e 分隔）…
    std::vector<std::string> fields(1);
    for (char ch : last) {
      if (ch == '\t')
        fields.emplace_back();
      else
        fields.back() += ch;
    }
    CHECK(fields.size() > 7 && fields[7].find('\x1e') != std::string::npos);
    if (fields.size() > 7)
      std::printf("  choice log on Enter: %s\n",
                  fields[7].substr(0, fields[7].find('\x1e')).c_str());
  }

  {
    std::lock_guard<std::mutex> lock(frontend.mutex);
    api->destroy_session(ss.session_id);
  }
  api->finalize();
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
