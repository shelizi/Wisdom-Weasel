// core/ime
// 的測試：文字規則、候選清洗、注音預覽的小工具、推薦、信心校準、選字統計與選字紀錄、學習過濾
#include "../../core/ime/ZhuyinPreview.h"
#include "../../core/ime/calibration.h"
#include "../../core/ime/choice_log.h"
#include "../../core/ime/choice_stats.h"
#include "../../core/ime/prediction_engine.h"
#include "../../core/ime/rescore.h"
#include "../../core/ime/text_rules.h"
#include "../../core/llm/LLMProvider.h"
#include "../../core/personal/LearnFilter.h"
#include "../../core/personal/PersonalLexicon.h"

#include <PersonalCrypto.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

using Strings = std::vector<std::wstring>;

// 假的評分模型：整句分數是各字分數的和，per_char 是各字分數
class FakeScorer : public LLMProvider {
 public:
  std::map<wchar_t, double> score;
  int calls = 0;
  // 預測與校正
  std::vector<std::wstring> predictions;
  std::wstring correction;
  std::atomic<int> predict_calls{0};
  std::atomic<int> sleep_ms{0};  // 模擬推理時間（期間會檢查取消）
  std::atomic<bool> cancelled_seen{false};
  std::wstring last_context;
  bool LoadConfig(const std::string&) override { return true; }
  std::vector<std::wstring> PredictCandidates(const std::wstring& context,
                                              const std::wstring&,
                                              size_t) override {
    ++predict_calls;
    last_context = context;
    for (int i = 0; i < sleep_ms / 10; ++i) {
      if (LLMCancelled()) {
        cancelled_seen = true;
        return {};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predictions;
  }
  std::wstring CorrectSentence(const std::wstring&,
                               const std::wstring&,
                               const std::wstring&,
                               const std::wstring&) override {
    return correction;
  }
  bool ScoreText(const std::wstring&,
                 const std::wstring& text,
                 double* total,
                 std::vector<double>* per_char) override {
    ++calls;
    *total = 0;
    if (per_char)
      per_char->clear();
    for (wchar_t c : text) {
      const double s = score.count(c) ? score[c] : -1.0;
      *total += s;
      if (per_char)
        per_char->push_back(s);
    }
    return true;
  }
  bool IsAvailable() const override { return true; }
  std::string GetProviderName() const override { return "fake"; }
};

static void TestTextRules() {
  CHECK(ime::IsSeparatorOrPunctuation(L'，'));
  CHECK(ime::IsSeparatorOrPunctuation(L' '));
  CHECK(!ime::IsSeparatorOrPunctuation(L'字'));
  CHECK(!ime::HasMeaningfulContent(L"，。！ "));
  CHECK(ime::HasMeaningfulContent(L"，好"));
  CHECK(!ime::HasMeaningfulContent(L""));

  // 只留第一行、去掉引號與標點、去重、丟掉空的，接上前綴
  const Strings cleaned = ime::CleanCandidates(
      {L"「天氣」\n第二行", L"天氣", L"  。", L"\"很好\"！",
       L"a\x01"
       L"b"},
      L"今天");
  CHECK(cleaned == (Strings{L"今天天氣", L"今天很好", L"今天ab"}));
  CHECK(ime::CleanCandidates({L"…"}, L"").empty());

  CHECK(ime::CleanCorrection(L"校正：今天天氣很好。", L"今天天汽很好") ==
        L"今天天氣很好");
  CHECK(ime::CleanCorrection(L"今天天汽很好", L"今天天汽很好")
            .empty());  // 和初稿相同
  CHECK(ime::CleanCorrection(L"今天天氣很好而且適合出門", L"今天天汽很好")
            .empty());  // 差太多
  CHECK(ime::CleanCorrection(L"「今天天氣好」", L"今天天汽很好") ==
        L"今天天氣好");

  CHECK(ime::MergeCandidates({L"a"}, {L"b", L"a", L"c", L"d", L"e", L"f"}) ==
        (Strings{L"a", L"b", L"c", L"d", L"e"}));
  CHECK(ime::MergeCandidates({}, {L"x", L"y"}, 1) == Strings{L"x"});
}

static void TestZhuyin() {
  using namespace zhuyin_preview;
  // 擴充字元（UTF-16 的代理對）算一個字
  const std::wstring ext = utf8::ToWide("\xF0\xA0\x80\x80");
  CHECK(SplitChars(L"注" + ext + L"音") == (Strings{L"注", ext, L"音"}));
  CHECK(Join({L"a", L"b", L"c"}, 1) == L"bc");
  // Backspace：游標前的音節已打聲調就整個刪掉，還在拼的一次一鍵
  CHECK(BackspaceKeys(L"ㄓㄨˋ") == 3);
  CHECK(BackspaceKeys(L"注ㄧㄣ") == 1);
  CHECK(BackspaceKeys(L"注音") == 1);
  CHECK(BackspaceKeys(L"ˋ") == 1);
  ZhuyinSpeller sp;
  CHECK(CountSyllables(sp, "5j4u.3") == 2);
  CHECK(SplitSyllables(sp, "5j4u.3") ==
        (std::vector<std::string>{"5j4", "u.3"}));
}

static void TestRescore() {
  FakeScorer scorer;
  // 「天汽」的「汽」分數很低，同音的「氣」較高 → 推薦「天氣」
  scorer.score = {
      {L'今', -1}, {L'天', -1}, {L'汽', -8}, {L'氣', -2}, {L'器', -6}};
  const Strings units = {L"今", L"天", L"汽", L"ㄏㄣ"};
  const std::vector<Strings> homophones = {
      {L"今"}, {L"天", L"添"}, {L"汽", L"器", L"氣"}, {}};
  double gain = 0;
  CHECK(ime::RescoreSentence(&scorer, L"", units, homophones, &gain) ==
        L"今天氣ㄏㄣ");
  CHECK(std::abs(gain - 6) < 1e-9);  // -8 → -2
  // 同音字都沒比較好：不推薦
  scorer.score[L'汽'] = -1;
  CHECK(ime::RescoreSentence(&scorer, L"", units, homophones).empty());
  // 不到兩個完整的字：不推薦，也不必評分
  scorer.calls = 0;
  CHECK(ime::RescoreSentence(&scorer, L"", {L"今", L"ㄊㄧㄢ"},
                             {{L"今", L"金"}, {}})
            .empty());
  CHECK(scorer.calls == 0);
}

static void TestChoiceStats(const fs::path& dir) {
  CHECK(ime::DateString(0).size() == 10);
  CHECK(ime::DateString(90) < ime::DateString(0));
  const std::string key = ime::ProfileKey("abc", "bopomofo｜推薦");
  CHECK(key.size() == 8 && key == ime::ProfileKey("abc", "bopomofo｜推薦"));
  CHECK(key != ime::ProfileKey("abc*", "bopomofo｜推薦"));

  fs::create_directories(dir);
  // 舊資料：沒有組合代碼的舊格式、太舊要丟掉的一天
  {
    std::ofstream out(dir / "weasel_stats.txt", std::ios::binary);
    out << ime::DateString(3) << "\t5\t10\t1\t0\t0\t0\t2\n";
    out << ime::DateString(200)
        << "\tdeadbeef\t1\t1\t0\t0\t0\t0\t0\t0\t0\t0\t0\n";
  }
  {
    ime::ChoiceStatsStore store(dir);
    store.AddProfile(key, "abc", "2026-01-01", "subject", "bopomofo｜推薦");
    store.AddProfile(key, "abc", "2026-01-01", "subject",
                     "bopomofo｜推薦");  // 第二次不重複記
    ime::ChoiceStats& s = store.Today(key);
    s.commits += 3;
    s.recommend_used = 1;
    store.Save();
  }
  const auto read = [](const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  };
  const std::string saved = read(dir / "weasel_stats.txt");
  CHECK(saved.find(ime::DateString(3) +
                   "\tlegacy\t5\t10\t1\t0\t0\t0\t2\t0\t0\t0\t0\n") !=
        std::string::npos);
  CHECK(saved.find(ime::DateString(0) + "\t" + key +
                   "\t3\t0\t0\t0\t0\t0\t0\t0\t0\t0\t1\n") != std::string::npos);
  CHECK(saved.find("deadbeef") == std::string::npos);  // 超過 90 天
  const std::string profiles = read(dir / "weasel_stats_profiles.txt");
  CHECK(profiles == key + "\tabc\t2026-01-01\tsubject\tbopomofo｜推薦\t" +
                        ime::DateString(0) + "\n");
  // 重新讀檔後累加
  {
    ime::ChoiceStatsStore store(dir);
    CHECK(store.Today(key).commits == 3);
    store.Reset();
  }
  CHECK(!fs::exists(dir / "weasel_stats.txt") &&
        !fs::exists(dir / "weasel_stats_profiles.txt"));
}

static void TestChoiceLog(const fs::path& dir) {
  CHECK(std::string(ime::ChoiceMethod(false, false, false, false, false)) ==
        "direct");
  CHECK(std::string(ime::ChoiceMethod(false, false, false, true, true)) ==
        "focus");
  CHECK(std::string(ime::ChoiceMethod(false, false, false, true, false)) ==
        "direct");
  CHECK(std::string(ime::ChoiceMethod(true, true, true, true, true)) ==
        "mixed");
  CHECK(std::string(ime::ChoiceMethod(false, false, true, false, true)) ==
        "llm");
  // 前文裡已經有這次送出的文字就去掉，最多 max 字
  CHECK(ime::ChoiceContext(L"前文今天", L"今天") == L"前文");
  CHECK(ime::ChoiceContext(L"abcdef", L"x", 3) == L"def");

  ime::ChoiceRecord r;
  r.time = 1700000000;
  r.app = "notepad.exe";
  r.method = "changed";
  r.context = L"前\t文";
  r.zhuyin = L"ㄐㄧㄣ ㄊㄧㄢ";
  r.default_text = L"金天";
  r.text = L"今天\n";
  const std::string plain = ime::FormatChoiceRecord(r);
  CHECK(plain ==
        "1700000000\tnotepad.exe\tchanged\t前 文\tㄐㄧㄣ ㄊㄧㄢ\t金天\t今天 ");

  // 加密附加兩筆，讀回來逐筆解開
  const fs::path personal = dir / "personal";
  CHECK(ime::AppendChoiceRecord(personal, r));
  CHECK(ime::AppendChoiceRecord(personal, r));
  std::ifstream in(personal / "choice_log.dat", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  size_t pos = 0, count = 0;
  while (pos + 4 <= data.size()) {
    uint32_t len = 0;
    std::memcpy(&len, data.data() + pos, 4);
    pos += 4;
    std::string decoded;
    CHECK(pos + len <= data.size());
    CHECK(personal_crypto::Unprotect(data.substr(pos, len), &decoded));
    CHECK(decoded == plain);
    CHECK(data.substr(pos, len).find("notepad") ==
          std::string::npos);  // 真的有加密
    pos += len;
    ++count;
  }
  CHECK(count == 2 && pos == data.size());
}

// 等到條件成立（最多 3 秒）
template <typename F>
static bool WaitFor(F cond) {
  for (int i = 0; i < 300; ++i) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

static void TestPredictionEngine() {
  FakeScorer model;
  model.predictions = {L"「天氣很好」", L"很好", L"天氣很好"};
  model.correction = L"今天天氣";
  model.score = {{L'今', -1}, {L'天', -1}, {L'汽', -8}, {L'氣', -2}};
  std::mutex updates_mutex;
  std::vector<std::pair<uint64_t, ime::PredictionSet>> updates;
  bool offer_rescore = true;
  ime::PredictionEngine::Hooks hooks;
  hooks.models = [&] {
    ime::PredictionModels m;
    m.predict = m.typo = m.rescore = &model;
    return m;
  };
  hooks.rescore_input =
      [&](uint64_t, uint64_t, std::vector<std::wstring>* units,
          std::vector<std::vector<std::wstring>>* homophones) {
        if (!offer_rescore)
          return false;
        *units = {L"今", L"天", L"汽"};
        *homophones = {{L"今"}, {L"天"}, {L"汽", L"氣"}};
        return true;
      };
  hooks.on_update = [&](uint64_t tag, uint64_t, const ime::PredictionSet& set) {
    std::lock_guard<std::mutex> lock(updates_mutex);
    updates.emplace_back(tag, set);
  };
  ime::PredictionEngine engine(hooks);
  const auto clear_updates = [&] {
    std::lock_guard<std::mutex> lock(updates_mutex);
    updates.clear();
  };
  const auto update_count = [&] {
    std::lock_guard<std::mutex> lock(updates_mutex);
    return updates.size();
  };

  // 送出後預測下一個詞：個人詞庫先出現，LLM 續寫接在後面
  ime::PredictionRequest next;
  next.tag = 7;
  next.history = L"前文";
  next.personal = {L"很好", L"不錯"};
  next.predict = true;
  engine.Request(next);
  CHECK(WaitFor([&] { return update_count() == 2; }));
  {
    std::lock_guard<std::mutex> lock(updates_mutex);
    CHECK(updates[0].first == 7);
    CHECK(updates[0].second.candidates == (Strings{L"很好", L"不錯"}));
    CHECK(updates[1].second.candidates ==
          (Strings{L"很好", L"不錯", L"天氣很好"}));
    CHECK(updates[1].second.Count(ime::CandidateKind::kRecommend) == 0 &&
          updates[1].second.Count(ime::CandidateKind::kCorrection) == 0);
  }
  CHECK(model.last_context == L"前文");

  // 打字中：推薦（天汽 → 天氣）、整句校正排最前，續寫接在校正結果後面
  clear_updates();
  ime::PredictionRequest typing;
  typing.history = L"前文";
  typing.prefix = L"今天汽";
  typing.personal = {L"今天汽車"};
  typing.predict = typing.correct = typing.rescore = true;
  typing.zhuyin = L"ㄐㄧㄣ ㄊㄧㄢ ㄑㄧˋ";
  engine.Request(typing);
  CHECK(WaitFor([&] { return update_count() == 4; }));
  const ime::PredictionSet final_set = engine.Snapshot();
  CHECK(final_set.candidates ==
        (Strings{L"今天氣", L"今天天氣", L"今天汽車", L"今天天氣天氣很好",
                 L"今天天氣很好"}));
  CHECK(final_set.Count(ime::CandidateKind::kRecommend) == 1 &&
        final_set.Count(ime::CandidateKind::kCorrection) == 1);
  CHECK(final_set.IsRecommend(0) && final_set.IsCorrection(1) &&
        !final_set.IsCorrection(2));
  // 沒有校準：註解只有種類，沒有機率
  CHECK(final_set.Comment(0) == L"推薦" && final_set.Comment(1) == L"校正" &&
        final_set.Comment(2).empty());
  CHECK(!ime::HasConfidence(final_set.Confidence(0)));
  CHECK(model.last_context == L"前文今天天氣");  // 續寫接在校正結果後面

  // 取出候選：知道是推薦還是校正，取出後候選清空
  std::wstring text;
  bool recommend = false, correction = false;
  CHECK(engine.Take(1, &text, &recommend, &correction));
  CHECK(text == L"今天天氣" && !recommend && correction);
  CHECK(!engine.HasCandidates());
  CHECK(!engine.Take(0, &text, &recommend, &correction));

  // 只校正、不續寫、不推薦
  clear_updates();
  offer_rescore = false;
  model.predict_calls = 0;
  ime::PredictionRequest correct_only = typing;
  correct_only.personal.clear();
  correct_only.predict = false;
  engine.Request(correct_only);
  CHECK(WaitFor([&] { return update_count() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(engine.Snapshot().candidates == Strings{L"今天天氣"});
  CHECK(model.predict_calls == 0);

  // 新請求取代舊的：舊的在防抖期間就放棄；推理中的舊請求被取消，結果不會出現
  clear_updates();
  model.predict_calls = 0;
  ime::PredictionRequest slow = next;
  slow.personal.clear();
  slow.delay_ms = 200;
  engine.Request(slow);
  ime::PredictionRequest newer = next;
  newer.personal = {L"新的"};
  engine.Request(newer);
  CHECK(WaitFor([&] { return update_count() == 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(update_count() == 2);
  CHECK(model.predict_calls == 1);  // 防抖中的舊請求沒有推理
  CHECK(engine.Snapshot().candidates ==
        (Strings{L"新的", L"天氣很好", L"很好"}));

  clear_updates();
  model.sleep_ms = 1000;
  model.cancelled_seen = false;
  ime::PredictionRequest long_one = next;
  long_one.personal.clear();
  engine.Request(long_one);
  CHECK(WaitFor([&] { return model.predict_calls >= 2; }));
  engine.Cancel();
  CHECK(WaitFor(
      [&] { return model.cancelled_seen.load(); }));  // 本機模型立即停止
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(update_count() == 0 && !engine.HasCandidates());
  model.sleep_ms = 0;

  // 推理鎖：持鎖時背景推理會等（例如重新部署換模型）
  clear_updates();
  {
    std::lock_guard<std::mutex> lock(engine.InferMutex());
    engine.Request(long_one);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(update_count() == 0);
  }
  CHECK(WaitFor([&] { return update_count() == 1; }));
}

// 可重現的亂數（0～1）
static double Uniform(uint64_t* state) {
  *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
  return (double)(*state >> 11) / (double)(1ULL << 53);
}

static void TestCalibration(const fs::path& dir) {
  // 樣本不夠或只有一種結果：不給機率
  ime::Calibrator few;
  for (int i = 0; i < 29; ++i)
    few.Add(i % 2 ? 5 : 0, i % 2 == 1);
  CHECK(!few.Ready() && !ime::HasConfidence(few.Probability(3)));
  ime::Calibrator same;
  for (int i = 0; i < 100; ++i)
    same.Add(i * 0.1, true);
  CHECK(!same.Ready());

  // 使用者真正的採用機率是 sigmoid(1.5·(gain −
  // 4))：擬合出來要接近，而且是校準的
  ime::Calibrator c;
  uint64_t rng = 42;
  for (int i = 0; i < 2000; ++i) {
    const double gain = (i % 100) / 10.0;
    const double truth = 1.0 / (1.0 + std::exp(-1.5 * (gain - 4)));
    c.Add(gain, Uniform(&rng) < truth, false);
  }
  c.Fit();
  CHECK(c.Ready());
  CHECK(std::abs(c.a() - 1.5) < 0.3);
  CHECK(std::abs(c.b() + 6) < 1.2);
  CHECK(c.Probability(8) > 0.95 && c.Probability(1) < 0.05);
  CHECK(c.Probability(2) < c.Probability(3));
  CHECK(c.Ece() < 0.05);
  CHECK(c.LogLoss() < std::log(2.0));  // 比亂猜好
  // 只留最近的樣本
  for (int i = 0; i < 100; ++i)
    c.Add(1, true);
  CHECK(c.Samples() == ime::Calibrator::kMaxSamples);

  // 存檔：另一個 store 讀回來的結果一樣
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    ime::CalibrationStore store(dir);
    CHECK(store.Summary(ime::SuggestionKind::kRecommend).empty());
    for (int i = 0; i < 40; ++i)
      store.Record(ime::SuggestionKind::kRecommend, i % 10, i % 10 >= 5);
    store.Record(ime::SuggestionKind::kCorrection, 2, true);
    CHECK(ime::HasConfidence(
        store.Probability(ime::SuggestionKind::kRecommend, 7)));
    CHECK(!ime::HasConfidence(
        store.Probability(ime::SuggestionKind::kCorrection, 7)));
  }
  ime::CalibrationStore reloaded(dir);
  const double p7 = reloaded.Probability(ime::SuggestionKind::kRecommend, 7);
  const double p2 = reloaded.Probability(ime::SuggestionKind::kRecommend, 2);
  CHECK(ime::HasConfidence(p7) && p7 > 0.8 && p2 < 0.2);
  CHECK(reloaded.Summary(ime::SuggestionKind::kRecommend).find("ECE") !=
        std::string::npos);
  CHECK(reloaded.Summary(ime::SuggestionKind::kCorrection).find("1 筆") !=
        std::string::npos);
}

static void TestConfidenceOrdering() {
  // 推薦「今天汽 → 今天氣」gain 6；校正「今天汽 → 今天天氣」gain 5（-10 → -5）
  FakeScorer model;
  model.predictions = {L"很好"};
  model.correction = L"今天天氣";
  model.score = {{L'今', -1}, {L'天', -1}, {L'汽', -8}, {L'氣', -2}};
  std::mutex mutex;
  double p_recommend = 0.3, p_correction = 0.9;
  std::vector<std::pair<ime::SuggestionKind, double>> asked;
  ime::PredictionEngine::Hooks hooks;
  hooks.models = [&] {
    ime::PredictionModels m;
    m.predict = m.typo = m.rescore = &model;
    return m;
  };
  hooks.rescore_input =
      [&](uint64_t, uint64_t, std::vector<std::wstring>* units,
          std::vector<std::vector<std::wstring>>* homophones) {
        *units = {L"今", L"天", L"汽"};
        *homophones = {{L"今"}, {L"天"}, {L"汽", L"氣"}};
        return true;
      };
  hooks.confidence = [&](ime::SuggestionKind kind, double gain) {
    std::lock_guard<std::mutex> lock(mutex);
    asked.emplace_back(kind, gain);
    return kind == ime::SuggestionKind::kRecommend ? p_recommend : p_correction;
  };
  std::atomic<int> updates{0};
  hooks.on_update = [&](uint64_t, uint64_t, const ime::PredictionSet&) {
    ++updates;
  };
  ime::PredictionEngine engine(hooks);

  ime::PredictionRequest typing;
  typing.history = L"前文";
  typing.prefix = L"今天汽";
  typing.predict = typing.correct = typing.rescore = true;
  typing.zhuyin = L"ㄐㄧㄣ ㄊㄧㄢ ㄑㄧˋ";
  const auto run = [&](const ime::PredictionRequest& req, int publishes) {
    updates = 0;
    engine.Request(req);
    CHECK(WaitFor([&] { return updates.load() == publishes; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return engine.Snapshot();
  };

  // 校正的採用機率比較高：排到推薦前面（Tab 選到的是校正）
  ime::PredictionSet set = run(typing, 3);
  CHECK(set.candidates.size() >= 2 && set.candidates[0] == L"今天天氣" &&
        set.candidates[1] == L"今天氣");
  CHECK(set.IsCorrection(0) && set.IsRecommend(1));
  CHECK(std::abs(set.Gain(0) - 5) < 1e-9 && std::abs(set.Gain(1) - 6) < 1e-9);
  CHECK(set.Confidence(0) == 0.9 && set.Comment(0) == L"校正 90%" &&
        set.Comment(1) == L"推薦 30%");
  CHECK(!ime::HasConfidence(set.Confidence(2)) && set.Comment(2).empty());
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(asked.size() == 2 &&
          asked[0].first == ime::SuggestionKind::kRecommend &&
          asked[1].first == ime::SuggestionKind::kCorrection);
  }

  // 推薦低於門檻：不顯示，續寫接在校正後面
  typing.min_confidence = 0.5;
  set = run(typing, 2);
  CHECK(set.Count(ime::CandidateKind::kRecommend) == 0 && set.IsCorrection(0));
  CHECK(model.last_context == L"前文今天天氣");

  // 都低於門檻：只剩續寫，接在原本的轉換結果後面
  p_correction = 0.2;
  set = run(typing, 1);
  CHECK(set.Count(ime::CandidateKind::kRecommend) == 0 &&
        set.Count(ime::CandidateKind::kCorrection) == 0);
  CHECK(model.last_context == L"前文今天汽");

  // 還在拼注音時不比較校正前後（避免沒拼完的注音把 gain 灌高）
  {
    std::lock_guard<std::mutex> lock(mutex);
    asked.clear();
  }
  p_correction = 0.9;
  typing.min_confidence = 0;
  typing.prefix = L"今天ㄑ";
  typing.rescore = false;
  model.correction = L"今天氣";
  set = run(typing, 2);
  CHECK(set.IsCorrection(0) && !ime::HasConfidence(set.Gain(0)));
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(asked.empty());
  }
}

static void TestLearnFilter(const fs::path& dir) {
  // 片段的平均分數：逗號切開，低於門檻的不學；分數與文字長度對不上時不擋
  const std::wstring text = L"今天很好，ㄅㄅㄅ";
  const std::vector<double> scores = {-2, -2, -2, -2, -1, -10, -10, -10};
  CHECK(LearnFilter::LowScoreUnits(text, scores, -7) == Strings{L"ㄅㄅㄅ"});
  CHECK(LearnFilter::LowScoreUnits(text, {-10}, -7).empty());

  PersonalLexicon lexicon(dir);
  // 假的評分：「亂」「碼」很低，其他 -2；前幾次回報忙碌
  std::atomic<int> busy{2};
  std::atomic<bool> available{true};
  auto scorer = [&](const std::wstring&, const std::wstring& t,
                    std::vector<double>* per_char) {
    if (busy > 0) {
      --busy;
      return LearnFilter::Score::kBusy;
    }
    if (!available)
      return LearnFilter::Score::kUnavailable;
    per_char->clear();
    for (wchar_t c : t)
      per_char->push_back(c == L'亂' || c == L'碼' ? -12.0 : -2.0);
    return LearnFilter::Score::kOk;
  };
  {
    LearnFilter filter(&lexicon, scorer);
    LearnFilter::Options options;
    options.delay_ms = 0;
    options.busy_retry_ms = 10;
    filter.SetOptions(options);
    lexicon.NoteCommit(L"w", L"今天很好，亂碼");
    filter.Submit(L"w", L"", L"今天很好，亂碼");
    CHECK(filter.WaitIdle(5000));
    CHECK(busy == 0);  // 忙碌時等一下再評
    CHECK(lexicon.WordScore(L"今天很好") > 0);
    CHECK(lexicon.WordScore(L"亂碼") == 0);
    // 同一個片段被擋第三次就放行
    filter.Submit(L"w", L"", L"亂碼");
    filter.Submit(L"w", L"", L"亂碼");
    CHECK(filter.WaitIdle(5000));
    CHECK(lexicon.WordScore(L"亂碼") > 0);
    // 沒有本機模型：照常學習
    available = false;
    filter.Submit(L"w", L"", L"碼亂");
    CHECK(filter.WaitIdle(5000));
    CHECK(lexicon.WordScore(L"碼亂") > 0);
  }
  // 原始紀錄標記了擋下的片段，重建時一樣不學
  auto records = PersonalLexicon::ReadRawLog(lexicon.ActiveLogPath());
  CHECK(records.size() == 4);
  if (records.size() == 4) {
    CHECK(records[0].text == L"今天很好，亂碼");
    CHECK(records[0].rejected == Strings{L"亂碼"});
    CHECK(records[3].rejected.empty());
  }
  records.resize(1);
  lexicon.Rebuild(records);
  CHECK(lexicon.WordScore(L"今天很好") > 0);
  CHECK(lexicon.WordScore(L"亂碼") == 0);

  // 送出當下就記下最後的詞，接續查得到（學習還沒做）
  lexicon.Record(L"a", L"早安，你好");
  lexicon.NoteCommit(L"b", L"早安");
  CHECK(lexicon.NextAfter(L"b", 3) == Strings{L"你好"});
  lexicon.NoteCommit(L"b", L"，");  // 只有標點：保留
  CHECK(lexicon.NextAfter(L"b", 3) == Strings{L"你好"});
  lexicon.NoteCommit(L"b", L"早安。");  // 句尾：重新開始
  CHECK(lexicon.NextAfter(L"b", 3).empty());
}

static void TestSplits(const fs::path& dir) {
  {
    PersonalLexicon lexicon(dir);
    lexicon.Record(L"w", L"好啊，我明天下午要去台北開會，再說");
    lexicon.Record(L"w", L"我明天下午要去台北開會");
    CHECK(lexicon.WordScore(L"我明天下午要去台北開會") > 1.5);
    // 接不回原片段（改了字）、只有一段：不收
    lexicon.ApplySplits({{L"好啊", {L"好", L"阿"}},
                         {L"再說", {L"再說"}},
                         {L"我明天下午要去台北開會",
                          {L"我", L"明天下午", L"要去", L"台北", L"開會"}}});
    CHECK(lexicon.Splits().size() == 1);
    // 分數與接續移到拆出來的部分
    CHECK(lexicon.WordScore(L"我明天下午要去台北開會") == 0);
    CHECK(lexicon.WordScore(L"台北") > 1.5);
    CHECK(lexicon.WordScore(L"好啊") > 0);
    lexicon.NoteCommit(L"x", L"台北");
    CHECK(lexicon.NextAfter(L"x", 3) == Strings{L"開會"});
    lexicon.NoteCommit(L"x", L"好啊");
    CHECK(lexicon.NextAfter(L"x", 3) == Strings{L"我"});
    lexicon.NoteCommit(L"x", L"開會");
    CHECK(lexicon.NextAfter(L"x", 3) == Strings{L"再說"});
    // 之後再打到：學拆出來的部分；最後的詞是最後一個部分
    lexicon.Record(L"y", L"我明天下午要去台北開會");
    CHECK(lexicon.WordScore(L"我明天下午要去台北開會") == 0);
    CHECK(lexicon.WordScore(L"要去") > 2.5);
    CHECK(lexicon.NextAfter(L"y", 3) == Strings{L"再說"});
    lexicon.Save();
  }
  {
    // 規則存在 refine.dat：重新載入後仍然生效；重建時也套用
    PersonalLexicon lexicon(dir);
    CHECK(lexicon.Load());
    CHECK(lexicon.Splits().size() == 1);
    auto records = PersonalLexicon::ReadRawLog(lexicon.ActiveLogPath());
    lexicon.Rebuild(records);
    CHECK(lexicon.WordScore(L"我明天下午要去台北開會") == 0);
    CHECK(lexicon.WordScore(L"明天下午") > 2.5);
    // 設定程式的詞庫管理：匯出看得到拆解規則，Z 取消拆解
    const fs::path exported = dir / "export.dat";
    CHECK(lexicon.ExportTo(exported, 100));
    std::string plain;
    CHECK(personal_crypto::ReadProtected(exported, &plain));
    CHECK(plain.find(utf8::FromWide(
              L"S\t我明天下午要去台北開會\t我 明天下午 要去 台北 開會\n")) !=
          std::string::npos);
    const fs::path edits = dir / "edit.dat";
    CHECK(personal_crypto::WriteProtected(
        edits, utf8::FromWide(L"WWPE1\nZ\t我明天下午要去台北開會\n")));
    CHECK(lexicon.ApplyEdits(edits) == 1);
    CHECK(lexicon.Splits().empty());
    lexicon.Record(L"w", L"我明天下午要去台北開會");
    CHECK(lexicon.WordScore(L"我明天下午要去台北開會") > 0);
    lexicon.ApplySplits({{L"我明天下午要去台北開會",
                          {L"我", L"明天下午", L"要去", L"台北", L"開會"}}});
    CHECK(lexicon.Splits().size() == 1);
    // 親自加入整個片段：取消拆解
    lexicon.AddWord(L"我明天下午要去台北開會");
    CHECK(lexicon.Splits().empty());
    lexicon.Record(L"w", L"我明天下午要去台北開會");
    CHECK(lexicon.WordScore(L"我明天下午要去台北開會") > 5);
  }
}

int main() {
  const fs::path dir = fs::temp_directory_path() / L"TestIme-注音";
  std::error_code ec;
  fs::remove_all(dir, ec);
  TestTextRules();
  TestZhuyin();
  TestRescore();
  TestPredictionEngine();
  TestConfidenceOrdering();
  TestCalibration(dir / "calibration");
  TestChoiceStats(dir / "stats");
  TestChoiceLog(dir / "log");
  TestLearnFilter(dir / "personal");
  TestSplits(dir / "splits");
  fs::remove_all(dir, ec);
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
