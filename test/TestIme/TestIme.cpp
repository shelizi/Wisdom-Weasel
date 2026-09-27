// core/ime 的測試：文字規則、候選清洗、注音預覽的小工具、推薦、選字統計與選字紀錄
#include "../../core/ime/ZhuyinPreview.h"
#include "../../core/ime/choice_log.h"
#include "../../core/ime/choice_stats.h"
#include "../../core/ime/rescore.h"
#include "../../core/ime/text_rules.h"
#include "../../core/llm/LLMProvider.h"

#include <PersonalCrypto.h>

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
  bool LoadConfig(const std::string&) override { return true; }
  std::vector<std::wstring> PredictCandidates(const std::wstring&, const std::wstring&, size_t) override {
    return {};
  }
  bool ScoreText(const std::wstring&, const std::wstring& text, double* total,
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
  const Strings cleaned =
      ime::CleanCandidates({L"「天氣」\n第二行", L"天氣", L"  。", L"\"很好\"！", L"a\x01" L"b"}, L"今天");
  CHECK(cleaned == (Strings{L"今天天氣", L"今天很好", L"今天ab"}));
  CHECK(ime::CleanCandidates({L"…"}, L"").empty());

  CHECK(ime::CleanCorrection(L"校正：今天天氣很好。", L"今天天汽很好") == L"今天天氣很好");
  CHECK(ime::CleanCorrection(L"今天天汽很好", L"今天天汽很好").empty());   // 和初稿相同
  CHECK(ime::CleanCorrection(L"今天天氣很好而且適合出門", L"今天天汽很好").empty());  // 差太多
  CHECK(ime::CleanCorrection(L"「今天天氣好」", L"今天天汽很好") == L"今天天氣好");

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
  CHECK(SplitSyllables(sp, "5j4u.3") == (std::vector<std::string>{"5j4", "u.3"}));
}

static void TestRescore() {
  FakeScorer scorer;
  // 「天汽」的「汽」分數很低，同音的「氣」較高 → 推薦「天氣」
  scorer.score = {{L'今', -1}, {L'天', -1}, {L'汽', -8}, {L'氣', -2}, {L'器', -6}};
  const Strings units = {L"今", L"天", L"汽", L"ㄏㄣ"};
  const std::vector<Strings> homophones = {{L"今"}, {L"天", L"添"}, {L"汽", L"器", L"氣"}, {}};
  CHECK(ime::RescoreSentence(&scorer, L"", units, homophones) == L"今天氣ㄏㄣ");
  // 同音字都沒比較好：不推薦
  scorer.score[L'汽'] = -1;
  CHECK(ime::RescoreSentence(&scorer, L"", units, homophones).empty());
  // 不到兩個完整的字：不推薦，也不必評分
  scorer.calls = 0;
  CHECK(ime::RescoreSentence(&scorer, L"", {L"今", L"ㄊㄧㄢ"}, {{L"今", L"金"}, {}}).empty());
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
    out << ime::DateString(200) << "\tdeadbeef\t1\t1\t0\t0\t0\t0\t0\t0\t0\t0\t0\n";
  }
  {
    ime::ChoiceStatsStore store(dir);
    store.AddProfile(key, "abc", "2026-01-01", "subject", "bopomofo｜推薦");
    store.AddProfile(key, "abc", "2026-01-01", "subject", "bopomofo｜推薦");  // 第二次不重複記
    ime::ChoiceStats& s = store.Today(key);
    s.commits += 3;
    s.recommend_used = 1;
    store.Save();
  }
  const auto read = [](const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::string saved = read(dir / "weasel_stats.txt");
  CHECK(saved.find(ime::DateString(3) + "\tlegacy\t5\t10\t1\t0\t0\t0\t2\t0\t0\t0\t0\n") != std::string::npos);
  CHECK(saved.find(ime::DateString(0) + "\t" + key + "\t3\t0\t0\t0\t0\t0\t0\t0\t0\t0\t1\n") != std::string::npos);
  CHECK(saved.find("deadbeef") == std::string::npos);  // 超過 90 天
  const std::string profiles = read(dir / "weasel_stats_profiles.txt");
  CHECK(profiles == key + "\tabc\t2026-01-01\tsubject\tbopomofo｜推薦\t" + ime::DateString(0) + "\n");
  // 重新讀檔後累加
  {
    ime::ChoiceStatsStore store(dir);
    CHECK(store.Today(key).commits == 3);
    store.Reset();
  }
  CHECK(!fs::exists(dir / "weasel_stats.txt") && !fs::exists(dir / "weasel_stats_profiles.txt"));
}

static void TestChoiceLog(const fs::path& dir) {
  CHECK(std::string(ime::ChoiceMethod(false, false, false, false, false)) == "direct");
  CHECK(std::string(ime::ChoiceMethod(false, false, false, true, true)) == "focus");
  CHECK(std::string(ime::ChoiceMethod(false, false, false, true, false)) == "direct");
  CHECK(std::string(ime::ChoiceMethod(true, true, true, true, true)) == "mixed");
  CHECK(std::string(ime::ChoiceMethod(false, false, true, false, true)) == "llm");
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
  CHECK(plain == "1700000000\tnotepad.exe\tchanged\t前 文\tㄐㄧㄣ ㄊㄧㄢ\t金天\t今天 ");

  // 加密附加兩筆，讀回來逐筆解開
  const fs::path personal = dir / "personal";
  CHECK(ime::AppendChoiceRecord(personal, r));
  CHECK(ime::AppendChoiceRecord(personal, r));
  std::ifstream in(personal / "choice_log.dat", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t pos = 0, count = 0;
  while (pos + 4 <= data.size()) {
    uint32_t len = 0;
    std::memcpy(&len, data.data() + pos, 4);
    pos += 4;
    std::string decoded;
    CHECK(pos + len <= data.size());
    CHECK(personal_crypto::Unprotect(data.substr(pos, len), &decoded));
    CHECK(decoded == plain);
    CHECK(data.substr(pos, len).find("notepad") == std::string::npos);  // 真的有加密
    pos += len;
    ++count;
  }
  CHECK(count == 2 && pos == data.size());
}

int main() {
  const fs::path dir = fs::temp_directory_path() / L"TestIme-注音";
  std::error_code ec;
  fs::remove_all(dir, ec);
  TestTextRules();
  TestZhuyin();
  TestRescore();
  TestChoiceStats(dir / "stats");
  TestChoiceLog(dir / "log");
  fs::remove_all(dir, ec);
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
