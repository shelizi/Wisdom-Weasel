// 注音打錯一鍵的對照：同一批公開題目，Rime 容錯關 vs 開。
// 不讀選字紀錄、不改使用者的方案。沙盒沒有選字記憶。
//
// librime 1.17.0 的 translator/enable_correction 只用 NearSearchCorrector：
// 沿著剩餘的輸入，把每個鍵換成 keyboard_map 裡的鄰鍵（門檻 5）。
// 編輯距離（多打、少打）的 EditDistanceCorrector 在 corrector.cc 裡被 #if 0
// 拿掉。 keyboard_map
// 對字母列只有左右鄰鍵，而且不對稱；直向的隔壁鍵大多不在表裡。 糾錯邊的可信度是
// log(0.01)。候選清單最多再留 4 個糾錯候選。
//
// 用法：typo_probe <rime_shared_dir> <work_dir> <cases> <grammar.gram>
// [題數上限]
#include <rime_api.h>

#include "../../core/base/utf8.h"

#include <PersonalCrypto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 與 librime 1.17.0 src/rime/dict/corrector.cc 的 keyboard_map 相同。
const std::map<char, std::string> kRimeNeighbors = {
    {'1', "2qw"},   {'2', "13qwe"}, {'3', "24wer"}, {'4', "35ert"},
    {'5', "46rty"}, {'6', "57tyu"}, {'7', "68yui"}, {'8', "79uio"},
    {'9', "80iop"}, {'0', "9-op["}, {'-', "0=p[]"}, {'=', "-[]\\"},
    {'q', "w"},     {'w', "qe"},    {'e', "wr"},    {'r', "et"},
    {'t', "ry"},    {'y', "tu"},    {'u', "yi"},    {'i', "uo"},
    {'o', "ip"},    {'p', "o["},    {'[', "p]"},    {']', "[\\"},
    {'\\', "]"},    {'a', "s"},     {'s', "ad"},    {'d', "sf"},
    {'f', "dg"},    {'g', "fh"},    {'h', "gj"},    {'j', "hk"},
    {'k', "jl"},    {'l', "k;"},    {';', "l'"},    {'\'', ";"},
    {'z', "x"},     {'x', "zc"},    {'c', "xv"},    {'v', "cb"},
    {'b', "vn"},    {'n', "bm"},    {'m', "n,"},    {',', "m."},
    {'.', ",/"},    {'/', "."},
};

// 標準 QWERTY 直向同一欄。Rime 的表大多不含這些。
const char* kColumns[] = {"1qaz", "2wsx", "3edc", "4rfv", "5tgb",
                          "6yhn", "7ujm", "8ik,", "9ol.", "0p;/"};

enum Kind {
  kNeighbor = 0,
  kVertical = 1,
  kExtra = 2,
  kMissing = 3,
  kKindCount = 4
};

const char* kKindName[] = {
    "鄰鍵（Rime 的表找得回）",
    "直向隔壁（不在 Rime 的表）",
    "多打同一個鍵",
    "少打一個鍵",
};

struct Case {
  std::wstring context;
  std::wstring zhuyin;
  std::wstring truth;
  std::string keys;
  std::string tags;
  std::vector<std::wstring> alts;
  std::string orig_keys;
  std::wstring trap;
};

struct Hit {
  std::wstring preview;
  std::vector<std::wstring> cands;
  double ms = 0;
};

struct Trial {
  int kind = 0;
  bool tone = false;
  int anchor_off = 0, anchor_on = 0;
  int truth_off = 0, truth_on = 0;
};

std::wstring Trim(std::wstring s) {
  while (!s.empty() &&
         (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\n'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && s[i] == L' ')
    ++i;
  return s.substr(i);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// 注音 → 大千按鍵。一聲用空白。與 TuneChoice::SyllableKeys 相同。
std::string SyllableKeys(const std::wstring& zhuyin) {
  static const wchar_t kSymbols[] =
      L"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤ"
      L"ㄥㄦ";
  static const char kKeys[] = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-";
  static const wchar_t kTones[] = L"ˊˇˋ˙";
  static const char kToneKeys[] = "6347";
  std::string out;
  std::string cur;
  auto flush = [&](char tone) {
    if (!cur.empty()) {
      out += cur;
      out += tone;
      cur.clear();
    }
  };
  for (wchar_t c : zhuyin) {
    if (const wchar_t* s = std::wcschr(kSymbols, c))
      cur += kKeys[s - kSymbols];
    else if (const wchar_t* t = std::wcschr(kTones, c))
      flush(kToneKeys[t - kTones]);
    else if (c == L' ')
      flush(' ');
    else
      return {};
  }
  flush(' ');
  return out;
}

bool IsToneKey(char c) {
  return c == ' ' || c == '6' || c == '3' || c == '4' || c == '7';
}

bool RimeCanRestore(char typed, char original) {
  const auto it = kRimeNeighbors.find(typed);
  return it != kRimeNeighbors.end() &&
         it->second.find(original) != std::string::npos;
}

std::vector<char> ReverseNeighbors(char original) {
  std::vector<char> out;
  for (const auto& [typed, neigh] : kRimeNeighbors) {
    if (typed != original && neigh.find(original) != std::string::npos)
      out.push_back(typed);
  }
  return out;
}

std::vector<char> VerticalMistypes(char original) {
  std::vector<char> out;
  for (const char* col : kColumns) {
    const char* p = std::strchr(col, original);
    if (!p)
      continue;
    if (p != col)
      out.push_back(p[-1]);
    if (p[1])
      out.push_back(p[1]);
    break;
  }
  out.erase(std::remove_if(
                out.begin(), out.end(),
                [&](char typed) { return RimeCanRestore(typed, original); }),
            out.end());
  return out;
}

// 選字紀錄的注音是最後送出的按鍵，不是當時打錯的鍵。這裡只拿來當句子，錯鍵另外抽。
bool LoadLog(const fs::path& file,
             std::vector<Case>* cases,
             size_t* skipped,
             size_t limit) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    return false;
  const std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  size_t pos = 0;
  while (pos + 4 <= data.size()) {
    uint32_t len = 0;
    std::memcpy(&len, data.data() + pos, 4);
    pos += 4;
    if (pos + len > data.size())
      break;
    std::string plain;
    const bool ok = personal_crypto::Unprotect(data.substr(pos, len), &plain);
    pos += len;
    if (!ok) {
      ++*skipped;
      continue;
    }
    const auto f = Split(utf8::ToWide(plain), L'\t');
    if (f.size() < 7 || f[2] == L"mixed" || f[4].empty() || f[6].empty()) {
      ++*skipped;
      continue;
    }
    Case c;
    c.zhuyin = Trim(f[4]);
    c.truth = Trim(f[6]);
    c.keys = SyllableKeys(c.zhuyin);
    if (c.keys.size() < 2 || c.truth.empty()) {
      ++*skipped;
      continue;
    }
    cases->push_back(std::move(c));
    if (limit && cases->size() >= limit)
      break;
  }
  return true;
}

bool LoadCases(const fs::path& file, std::vector<Case>* cases, size_t limit) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    return false;
  for (std::string line; std::getline(in, line);) {
    const std::wstring w = Trim(utf8::ToWide(line));
    if (w.empty() || w[0] == L'#')
      continue;
    const auto f = Split(w, L'|');
    if (f.size() < 4)
      continue;
    Case c;
    c.context = f[0];
    c.zhuyin = Trim(f[1]);
    c.truth = Trim(f[3]);
    if (f.size() > 5 && !Trim(f[5]).empty()) {
      for (wchar_t ch : Trim(f[5])) {
        if (ch >= 32 && ch < 128)
          c.keys.push_back(static_cast<char>(ch));
      }
    } else {
      c.keys = SyllableKeys(c.zhuyin);
    }
    if (f.size() > 6) {
      for (wchar_t ch : Trim(f[6])) {
        if (ch >= 32 && ch < 128)
          c.orig_keys.push_back(static_cast<char>(ch));
      }
    }
    if (f.size() > 7)
      c.trap = Trim(f[7]);
    if (c.keys.size() < 2 || c.truth.empty())
      continue;
    for (const auto& alt : Split(c.truth, L'/')) {
      const std::wstring a = Trim(alt);
      if (!a.empty())
        c.alts.push_back(a);
    }
    if (c.alts.empty())
      continue;
    c.truth = c.alts[0];
    if (f.size() > 4) {
      for (wchar_t ch : f[4]) {
        if (ch >= 32 && ch < 128)
          c.tags.push_back(static_cast<char>(ch));
      }
    }
    cases->push_back(std::move(c));
    if (limit && cases->size() >= limit)
      break;
  }
  return true;
}

// tone_move：0 聲調只在最後（現況），1 也可以打在音節最前，2
// 再允許插在第一個符號後面。 規則加在 keymap
// 之前，作用在內部的聲調數字上，所以打出來的是大千的聲調鍵。
void WriteExpressAlgebra(std::ostream& out, int tone_move) {
  out << "  speller/algebra:\n";
  const char* rules[] = {
      "xform/^m(\\d)$/mu$1/",
      "xform/^r5$/er5/",
      "xform/iu/iou/",
      "xform/ui/uei/",
      "xform/ong/ung/",
      "xform/^yi?/i/",
      "xform/^wu?/u/",
      "xform/iu/v/",
      "xform/^([jqx])u/$1v/",
      "xform/([iuv])n/$1en/",
      "xform/^zhi?/Z/",
      "xform/^chi?/C/",
      "xform/^shi?/S/",
      "xform/^([zcsr])i/$1/",
      "xform/ai/A/",
      "xform/ei/I/",
      "xform/ao/O/",
      "xform/ou/U/",
      "xform/ang/K/",
      "xform/eng/G/",
      "xform/an/M/",
      "xform/en/N/",
      "xform/er/R/",
      "xform/eh/E/",
      "xform/([iv])e/$1E/",
      "derive/([bpmfdtnlgkhjqxZCSrzcs])([iuv])/$2$1/",
      "derive/([iuv])([aoeEAIOUMNKGR])/$2$1/",
      "derive/([bpmfdtnlgkhjqxZCSrzcs])([aoeEAIOUMNKGR])/$2$1/",
      "derive/([bpmfdtnlgkhjqxZCSrzcs])([iuv])/$2$1/",
  };
  for (const char* rule : rules)
    out << "    - '" << rule << "'\n";
  // 1、2 才含「聲調打在最前」。3 只有接在第一個符號後面。
  if (tone_move == 1 || tone_move == 2)
    out << "    - "
           "'derive/^([bpmfdtnlgkhjqxZCSrzcsiuvaoeEAIOUMNKGR]+)([12345])$/$2$1/"
           "'\n";
  if (tone_move == 2 || tone_move == 3)
    out << "    - "
           "'derive/"
           "^([bpmfdtnlgkhjqxZCSrzcsiuvaoeEAIOUMNKGR])(["
           "bpmfdtnlgkhjqxZCSrzcsiuvaoeEAIOUMNKG"
           "R]+)([12345])$/$1$3$2/'\n";
  const char* tail[] = {
      "abbrev/^([bpmfdtnlgkhjqxZCSrzcs]).+$/$1/",
      "abbrev/^([A-Za-z]+)\\d$/$1/",
      "abbrev/^([bpmfdtnlgkhjqxZCSrzcs]).+(\\d)$/$1$2/",
      "xlit|bpmfdtnlgkhjqxZCSrzcsiuvaoeEAIOUMNKGR12345|"
      "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/- 6347|",
  };
  for (const char* rule : tail)
    out << "    - '" << rule << "'\n";
}

struct Engine {
  RimeApi* api = nullptr;
  RimeSessionId session = 0;
  bool ok = false;
  int keep_cands = 12;
  int scan_limit = 40;
  std::string shared_s, user_s;

  // schema 用 bopomofo 或 bopomofo_express。free_order 只在快打方案寫進拼寫；
  // 一般注音方案本來就有，這裡不覆寫它的拼寫。
  // collo_max / collo_min 為 0 表示不寫，沿用 octagram 的詞級預設（4 / 3）。
  // 字級模型必須自己寫，否則短於預設下限的搭配會吃 weak_collocation_penalty。
  bool Open(const fs::path& shared,
            const fs::path& user,
            const fs::path& grammar,
            bool correction,
            const std::string& schema = "bopomofo",
            bool free_order = false,
            int tone_move = -1,
            int max_homophones = 7,
            int max_sentences = 10,
            bool use_grammar = true,
            bool use_sentences = true,
            int collo_max = 0,
            int collo_min = 0) {
    std::error_code ec;
    fs::create_directories(user, ec);
    if (use_grammar)
      fs::copy_file(grammar, user / grammar.filename(),
                    fs::copy_options::overwrite_existing, ec);
    {
      std::ofstream out(user / "default.custom.yaml", std::ios::binary);
      out << "patch:\n  schema_list:\n    - schema: " << schema << "\n";
    }
    {
      std::ofstream out(user / (schema + ".custom.yaml"), std::ios::binary);
      out << "patch:\n";
      if (use_grammar) {
        out << "  grammar:\n    language: " << grammar.stem().string() << "\n";
        if (collo_max > 0)
          out << "    collocation_max_length: " << collo_max << "\n";
        if (collo_min > 0)
          out << "    collocation_min_length: " << collo_min << "\n";
        out << "  translator/contextual_suggestions: true\n"
               "  translator/max_homophones: "
            << max_homophones << "\n  translator/max_homographs: 7\n";
      } else {
        out << "  grammar: {}\n"
               "  translator/contextual_suggestions: false\n";
      }
      if (use_sentences) {
        out << "  translator/max_sentences: " << max_sentences
            << "\n  translator/sentence_cutoff_threshold: 100\n";
      }
      out << "  translator/enable_correction: "
          << (correction ? "true\n" : "false\n");
      if (schema == "bopomofo_express") {
        if (tone_move >= 0) {
          WriteExpressAlgebra(out, tone_move);
        } else {
          out << "  speller/algebra:\n"
                 "    __patch:\n"
                 "      - zhuyin:/pinyin_to_zhuyin\n";
          if (free_order)
            out << "      - zhuyin:/free_order\n";
          out << "      - zhuyin:/abbreviation\n"
                 "      - zhuyin:/keymap_bopomofo\n";
        }
      }
    }
    api = rime_get_api();
    shared_s = shared.string();
    user_s = user.string();
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = shared_s.c_str();
    traits.user_data_dir = user_s.c_str();
    traits.prebuilt_data_dir = shared_s.c_str();
    traits.app_name = "rime.typo_probe";
    traits.distribution_name = "TypoProbe";
    traits.distribution_code_name = "typo";
    traits.distribution_version = "0";
    traits.min_log_level = 2;
    api->setup(&traits);
    api->initialize(nullptr);
    if (api->start_maintenance(False))
      api->join_maintenance_thread();
    session = api->create_session();
    if (!session || !api->select_schema(session, schema.c_str())) {
      std::fprintf(stderr, "schema %s failed\n", schema.c_str());
      return false;
    }
    ok = true;
    const std::string gram_name = use_grammar ? grammar.stem().string() : "off";
    std::printf(
        "librime %s  schema=%s  correction=%s  free_order=%s  tone=%d  homo=%d "
        " sentences=%d  "
        "grammar=%s  collo=%d/%d\n",
        api->get_version ? api->get_version() : "?", schema.c_str(),
        correction ? "on" : "off", free_order ? "on" : "off", tone_move,
        max_homophones, use_sentences ? max_sentences : 0, gram_name.c_str(),
        collo_max, collo_min);
    return true;
  }

  void Close() {
    if (!api)
      return;
    if (session)
      api->destroy_session(session);
    api->finalize();
    api = nullptr;
    session = 0;
  }

  Hit Convert(const std::string& input) {
    Hit h;
    const auto t0 = std::chrono::steady_clock::now();
    api->set_input(session, input.c_str());
    RIME_STRUCT(RimeContext, ctx);
    if (api->get_context(session, &ctx)) {
      if (ctx.commit_text_preview)
        h.preview = utf8::ToWide(ctx.commit_text_preview);
      while (!h.preview.empty() && h.preview.back() < 0x80)
        h.preview.pop_back();
      api->free_context(&ctx);
    }
    RimeCandidateListIterator iter = {0};
    if (api->candidate_list_begin(session, &iter)) {
      for (int i = 0; i < scan_limit && (int)h.cands.size() < keep_cands &&
                      api->candidate_list_next(&iter);
           ++i) {
        if (!iter.candidate.text)
          continue;
        const std::wstring text = utf8::ToWide(iter.candidate.text);
        if (std::find(h.cands.begin(), h.cands.end(), text) == h.cands.end())
          h.cands.push_back(text);
      }
      api->candidate_list_end(&iter);
    }
    h.ms = std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
               .count();
    api->clear_composition(session);
    return h;
  }
};

int RankOf(const Hit& h, const std::wstring& target) {
  if (target.empty())
    return 0;
  if (h.preview == target)
    return 1;
  for (size_t i = 0; i < h.cands.size() && i < 10; ++i) {
    if (h.cands[i] == target)
      return static_cast<int>(i + 1);
  }
  return 0;
}

double Percentile(std::vector<double> v, double q) {
  if (v.empty())
    return NAN;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(
      std::min<double>(v.size() - 1, q * (v.size() - 1) + 0.5))];
}

struct Mut {
  Kind kind;
  std::string keys;
  bool tone;
};

std::vector<Mut> Mutations(const std::string& keys) {
  std::vector<Mut> out;
  for (size_t i = 0; i < keys.size(); ++i) {
    const char original = keys[i];
    for (char typed : ReverseNeighbors(original)) {
      std::string m = keys;
      m[i] = typed;
      out.push_back({kNeighbor, m, IsToneKey(original) || IsToneKey(typed)});
    }
    for (char typed : VerticalMistypes(original)) {
      std::string m = keys;
      m[i] = typed;
      out.push_back({kVertical, m, IsToneKey(original) || IsToneKey(typed)});
    }
    if (original != ' ') {
      std::string extra = keys;
      extra.insert(extra.begin() + static_cast<std::ptrdiff_t>(i), original);
      out.push_back({kExtra, extra, IsToneKey(original)});
    }
    if (keys.size() > 2) {
      std::string miss = keys;
      miss.erase(i, 1);
      out.push_back({kMissing, miss, IsToneKey(original)});
    }
  }
  return out;
}

std::vector<Mut> SampleOneEach(const std::vector<Mut>& all, uint32_t seed) {
  std::vector<Mut> out;
  for (int kind = 0; kind < kKindCount; ++kind) {
    std::vector<const Mut*> bucket;
    for (const auto& m : all)
      if (m.kind == kind)
        bucket.push_back(&m);
    if (bucket.empty())
      continue;
    std::mt19937 rng(seed + static_cast<uint32_t>(kind) * 9973u);
    std::uniform_int_distribution<size_t> dist(0, bucket.size() - 1);
    out.push_back(*bucket[dist(rng)]);
  }
  return out;
}

struct Bucket {
  size_t n = 0;
  size_t anchor_off1 = 0, anchor_on1 = 0, anchor_off5 = 0, anchor_on5 = 0,
         anchor_off10 = 0, anchor_on10 = 0;
  size_t truth_off1 = 0, truth_on1 = 0, truth_off10 = 0, truth_on10 = 0;
  void Add(const Trial& t) {
    ++n;
    anchor_off1 += t.anchor_off == 1;
    anchor_on1 += t.anchor_on == 1;
    anchor_off5 += t.anchor_off && t.anchor_off <= 5;
    anchor_on5 += t.anchor_on && t.anchor_on <= 5;
    anchor_off10 += t.anchor_off && t.anchor_off <= 10;
    anchor_on10 += t.anchor_on && t.anchor_on <= 10;
    truth_off1 += t.truth_off == 1;
    truth_on1 += t.truth_on == 1;
    truth_off10 += t.truth_off && t.truth_off <= 10;
    truth_on10 += t.truth_on && t.truth_on <= 10;
  }
};

void WriteReport(const fs::path& path,
                 const std::string& version,
                 const std::string& how,
                 size_t cases_n,
                 size_t clean_truth,
                 size_t clean_same,
                 size_t clean_changed,
                 size_t clean_to_truth,
                 size_t clean_from_truth,
                 size_t clean_other,
                 const std::vector<double>& clean_off_ms,
                 const std::vector<double>& clean_on_ms,
                 const std::vector<double>& err_off_ms,
                 const std::vector<double>& err_on_ms,
                 const std::vector<std::vector<Trial>>& by_case) {
  std::ofstream out(path, std::ios::binary);
  out << "\xEF\xBB\xBF";
  out << "注音打錯一鍵（沙盒 bopomofo，沒有選字記憶）\n";
  out << how << "\n";
  out << "librime " << version << "\n";
  out << "文法 zh-hant-t-essay-bgw，max_sentences "
         "10，sentence_cutoff_threshold 100\n";
  out << "每一題的乾淨按鍵先打一次。錯鍵是把其中一個鍵換掉、多打或少打，每次只"
         "動一鍵。\n";
  out << "「回到原句」＝送出預覽或前 10 個候選裡，出現沒打錯時的第一名。\n";
  out << "「等於標註」＝出現題目寫的正確句。這批題目本來就含同音錯字，兩件事不"
         "是同一件事。\n\n";

  auto pct = [](size_t a, size_t n) {
    if (!n)
      return std::string("—");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * a / n);
    return std::string(buf);
  };
  out << "乾淨輸入 " << cases_n << " 題\n";
  out << "  容錯關，第一名等於標註：" << clean_truth << "（"
      << pct(clean_truth, cases_n) << "）\n";
  out << "  容錯開之後第一名沒變：" << clean_same << "（"
      << pct(clean_same, cases_n) << "）\n";
  out << "  有變的 " << clean_changed << " 題裡：改成標註 " << clean_to_truth
      << "、從標註改掉 " << clean_from_truth << "、兩邊都不是標註 "
      << clean_other << "\n";
  out << "  延遲 P50/P95 容錯關 " << Percentile(clean_off_ms, 0.5) << " / "
      << Percentile(clean_off_ms, 0.95) << " ms\n";
  out << "  延遲 P50/P95 容錯開 " << Percentile(clean_on_ms, 0.5) << " / "
      << Percentile(clean_on_ms, 0.95) << " ms\n\n";

  out << "錯一鍵。每一列是這種錯誤的全部次數（一句話會貢獻好幾次）。\n";
  out << "名次看的是「沒打錯時的第一名」還在不在。\n\n";
  for (int kind = 0; kind < kKindCount; ++kind) {
    Bucket all, tone, body;
    std::vector<double> sent_on1, sent_on10;
    size_t sent_n = 0, sent_any = 0;
    for (const auto& trials : by_case) {
      size_t n = 0, on1 = 0, on10 = 0;
      for (const auto& t : trials) {
        if (t.kind != kind)
          continue;
        all.Add(t);
        (t.tone ? tone : body).Add(t);
        ++n;
        on1 += t.anchor_on == 1;
        on10 += t.anchor_on && t.anchor_on <= 10;
      }
      if (!n)
        continue;
      ++sent_n;
      sent_any += on10 > 0;
      sent_on1.push_back((double)on1 / n);
      sent_on10.push_back((double)on10 / n);
    }
    double mean1 = 0, mean10 = 0;
    for (double v : sent_on1)
      mean1 += v;
    for (double v : sent_on10)
      mean10 += v;
    if (!sent_on1.empty()) {
      mean1 /= sent_on1.size();
      mean10 /= sent_on10.size();
    }
    out << kKindName[kind] << "\n";
    out << "  次數 " << all.n << "，涵蓋 " << sent_n << " 句\n";
    out << "  容錯關  第一名 " << pct(all.anchor_off1, all.n) << "  前五 "
        << pct(all.anchor_off5, all.n) << "  前十 "
        << pct(all.anchor_off10, all.n) << "\n";
    out << "  容錯開  第一名 " << pct(all.anchor_on1, all.n) << "  前五 "
        << pct(all.anchor_on5, all.n) << "  前十 "
        << pct(all.anchor_on10, all.n) << "\n";
    out << "  按句子平均（容錯開，回到原句）：第一名 "
        << (sent_n ? pct((size_t)std::lround(mean1 * 1000), 1000) : "—")
        << "  前十 "
        << (sent_n ? pct((size_t)std::lround(mean10 * 1000), 1000) : "—")
        << "\n";
    out << "  至少有一次進前十的句子：" << sent_any << " / " << sent_n << "\n";
    out << "  其中聲調鍵 " << tone.n << " 次，容錯開前十 "
        << pct(tone.anchor_on10, tone.n) << "；其餘 " << body.n
        << " 次，容錯開前十 " << pct(body.anchor_on10, body.n) << "\n";
    out << "  標註正確句，容錯開：第一名 " << pct(all.truth_on1, all.n)
        << "  前十 " << pct(all.truth_on10, all.n) << "（容錯關前十 "
        << pct(all.truth_off10, all.n) << "）\n\n";
  }
  out << "錯鍵延遲 P50/P95：容錯關 " << Percentile(err_off_ms, 0.5) << " / "
      << Percentile(err_off_ms, 0.95) << " ms；容錯開 "
      << Percentile(err_on_ms, 0.5) << " / " << Percentile(err_on_ms, 0.95)
      << " ms\n";
}

// 音節內部的按鍵順序。鍵是大千排列；注音紀錄本身是聲介韻的正常順序。
bool IsInitialKey(char c) {
  return std::string("1qaz2wsxedcrfv5tgbyhn").find(c) != std::string::npos;
}
bool IsMedialKey(char c) {
  return c == 'u' || c == 'j' || c == 'm';
}
bool IsFinalKey(char c) {
  return std::string("8ik,9ol.0p;/-").find(c) != std::string::npos;
}

struct Syl {
  std::string body;
  char tone = 0;
  size_t begin = 0;
  size_t end = 0;
};

std::vector<Syl> SplitSyllables(const std::string& keys) {
  std::vector<Syl> out;
  size_t i = 0;
  while (i < keys.size()) {
    const size_t begin = i;
    while (i < keys.size() && !IsToneKey(keys[i]))
      ++i;
    if (i >= keys.size())
      break;
    out.push_back({keys.substr(begin, i - begin), keys[i], begin, i + 1});
    ++i;
  }
  return out;
}

// 0 單鍵或認不出，1 聲介，2 聲韻，3 介韻，4 聲介韻
int SylShape(const std::string& body) {
  if (body.size() == 2 && IsInitialKey(body[0]) && IsMedialKey(body[1]))
    return 1;
  if (body.size() == 2 && IsInitialKey(body[0]) && IsFinalKey(body[1]))
    return 2;
  if (body.size() == 2 && IsMedialKey(body[0]) && IsFinalKey(body[1]))
    return 3;
  if (body.size() == 3 && IsInitialKey(body[0]) && IsMedialKey(body[1]) &&
      IsFinalKey(body[2]))
    return 4;
  return 0;
}

enum OrderKind {
  kSwapIM = 0,   // ㄅㄧㄚ → ㄧㄅㄚ
  kSwapMF,       // ㄅㄧㄚ → ㄅㄚㄧ，或 ㄨㄛ → ㄛㄨ
  kSwapIF,       // ㄅㄚ → ㄚㄅ；三碼則 ㄅㄧㄚ → ㄚㄅㄧ
  kReverse,      // ㄅㄧㄚ → ㄚㄧㄅ
  kMedialFirst,  // ㄅㄧㄚ → ㄧㄚㄅ
  kToneFront,
  kToneMid,
  kSylSwap,
  kOrderCount
};

const char* kOrderName[] = {
    "聲母和介音對調（ㄅㄧ→ㄧㄅ）",
    "介音和韻母對調（ㄨㄛ→ㄛㄨ）",
    "聲母和韻母對調（ㄅㄚ→ㄚㄅ）",
    "三碼倒序（ㄅㄧㄚ→ㄚㄧㄅ）",
    "介音打最前（ㄅㄧㄚ→ㄧㄚㄅ）",
    "聲調打在音節最前",
    "聲調插在音節中間",
    "相鄰兩個音節對調",
};

struct OrderMut {
  int kind = 0;
  std::string keys;
};

std::string Splice(const std::string& keys,
                   size_t begin,
                   size_t end,
                   const std::string& rep) {
  return keys.substr(0, begin) + rep + keys.substr(end);
}

std::vector<OrderMut> OrderMutations(const std::string& keys, uint32_t seed) {
  const auto syls = SplitSyllables(keys);
  std::vector<std::vector<OrderMut>> buckets(kOrderCount);
  auto add = [&](int kind, size_t begin, size_t end, const std::string& rep) {
    if (rep == keys.substr(begin, end - begin))
      return;
    buckets[kind].push_back({kind, Splice(keys, begin, end, rep)});
  };
  for (const auto& s : syls) {
    const int shape = SylShape(s.body);
    if (shape == 1 || shape == 4) {
      std::string body = s.body;
      std::swap(body[0], body[1]);
      add(kSwapIM, s.begin, s.end, body + s.tone);
    }
    if (shape == 3) {
      std::string body = s.body;
      std::swap(body[0], body[1]);
      add(kSwapMF, s.begin, s.end, body + s.tone);
    }
    if (shape == 4) {
      std::string body = s.body;
      std::swap(body[1], body[2]);
      add(kSwapMF, s.begin, s.end, body + s.tone);
      add(kSwapIF, s.begin, s.end,
          std::string() + s.body[2] + s.body[0] + s.body[1] + s.tone);
      add(kReverse, s.begin, s.end,
          std::string() + s.body[2] + s.body[1] + s.body[0] + s.tone);
      add(kMedialFirst, s.begin, s.end,
          std::string() + s.body[1] + s.body[2] + s.body[0] + s.tone);
    }
    if (shape == 2) {
      std::string body = s.body;
      std::swap(body[0], body[1]);
      add(kSwapIF, s.begin, s.end, body + s.tone);
    }
    if (!s.body.empty())
      add(kToneFront, s.begin, s.end, std::string(1, s.tone) + s.body);
    if (s.body.size() >= 2)
      add(kToneMid, s.begin, s.end,
          s.body.substr(0, 1) + s.tone + s.body.substr(1));
  }
  if (syls.size() >= 2) {
    for (size_t i = 0; i + 1 < syls.size(); ++i) {
      const auto& a = syls[i];
      const auto& b = syls[i + 1];
      const std::string rep = keys.substr(b.begin, b.end - b.begin) +
                              keys.substr(a.begin, a.end - a.begin);
      add(kSylSwap, a.begin, b.end, rep);
    }
  }
  std::vector<OrderMut> out;
  for (int kind = 0; kind < kOrderCount; ++kind) {
    if (buckets[kind].empty())
      continue;
    std::mt19937 rng(seed + static_cast<uint32_t>(kind) * 9973u);
    std::uniform_int_distribution<size_t> dist(0, buckets[kind].size() - 1);
    out.push_back(buckets[kind][dist(rng)]);
  }
  return out;
}

int RunOrder(const fs::path& shared,
             const fs::path& work,
             const fs::path& grammar,
             const std::vector<Case>& cases) {
  std::vector<std::vector<OrderMut>> muts(cases.size());
  for (size_t i = 0; i < cases.size(); ++i)
    muts[i] =
        OrderMutations(cases[i].keys, static_cast<uint32_t>(i) * 17u + 1u);

  constexpr int kArms = 2;
  const int kArmMode[] = {0, 3};
  const char* kArm[] = {"聲調只准在最後（現況）", "聲調可以接在第一個符號後面"};
  std::vector<Hit> clean[kArms];
  std::vector<std::vector<Hit>> err[kArms];
  std::string version;
  for (int arm = 0; arm < kArms; ++arm) {
    clean[arm].resize(cases.size());
    err[arm].resize(cases.size());
    Engine eng;
    const fs::path dir = work / ("tone" + std::to_string(kArmMode[arm]));
    if (!eng.Open(shared, dir, grammar, false, "bopomofo_express", true,
                  kArmMode[arm]))
      return 1;
    if (version.empty() && eng.api->get_version)
      version = eng.api->get_version();
    for (size_t i = 0; i < cases.size(); ++i) {
      clean[arm][i] = eng.Convert(cases[i].keys);
      for (const auto& m : muts[i])
        err[arm][i].push_back(eng.Convert(m.keys));
      if ((i + 1) % 400 == 0 || i + 1 == cases.size())
        std::printf("tone%d %zu/%zu\n", arm, i + 1, cases.size());
    }
    eng.Close();
  }

  auto pct = [](size_t a, size_t n) {
    if (!n)
      return std::string("—");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * a / n);
    return std::string(buf);
  };
  size_t usable = 0;
  size_t truth_n[kArms] = {};
  size_t same_as_now[kArms] = {};
  size_t broke[kArms] = {}, fixed[kArms] = {}, other_change[kArms] = {};
  struct Acc {
    size_t n = 0, now1 = 0, now10 = 0, self1 = 0, self10 = 0, truth1 = 0,
           truth10 = 0;
  };
  Acc acc[kArms][kOrderCount];
  for (size_t i = 0; i < cases.size(); ++i) {
    if (clean[0][i].preview.empty())
      continue;
    ++usable;
    const bool base_right = clean[0][i].preview == cases[i].truth;
    for (int arm = 0; arm < kArms; ++arm) {
      const bool right = clean[arm][i].preview == cases[i].truth;
      const bool same = clean[arm][i].preview == clean[0][i].preview;
      truth_n[arm] += right;
      same_as_now[arm] += same;
      if (!same) {
        if (base_right && !right)
          ++broke[arm];
        else if (!base_right && right)
          ++fixed[arm];
        else
          ++other_change[arm];
      }
      for (size_t j = 0; j < muts[i].size(); ++j) {
        Acc& a = acc[arm][muts[i][j].kind];
        ++a.n;
        const int self = RankOf(err[arm][i][j], clean[arm][i].preview);
        const int now = RankOf(err[arm][i][j], clean[0][i].preview);
        const int truth = RankOf(err[arm][i][j], cases[i].truth);
        a.self1 += self == 1;
        a.self10 += self && self <= 10;
        a.now1 += now == 1;
        a.now10 += now && now <= 10;
        a.truth1 += truth == 1;
        a.truth10 += truth && truth <= 10;
      }
    }
  }

  const fs::path report = work / "typo-tone-report.txt";
  std::ofstream out(report, std::ios::binary);
  out << "\xEF\xBB\xBF";
  out << "聲調位置（快打 + 不分順序，沙盒，沒有選字記憶，鄰鍵容錯關）\n";
  out << "librime " << version << "\n";
  out << "兩種拼寫都有聲母介音韻母不分順序。差別只在聲調能不能接在第一個符號後"
         "面。\n";
  out << "題目是選字紀錄，鍵是正常順序；聲調移位是事後改的。沒有改你的方案。\n"
         "\n";
  out << "沒有故意打錯 " << usable << " 句\n";
  for (int arm = 0; arm < kArms; ++arm) {
    out << kArm[arm] << "\n";
    out << "  第一名等於送出的句子：" << truth_n[arm] << "（"
        << pct(truth_n[arm], usable) << "）\n";
    out << "  第一名和現況相同：" << same_as_now[arm] << "（"
        << pct(same_as_now[arm], usable) << "）\n";
    out << "  和現況不同的句子：把送出的改掉 " << broke[arm] << "、改成送出的 "
        << fixed[arm] << "、兩邊都不是送出的 " << other_change[arm] << "\n";
  }
  const int kinds[] = {kToneFront, kToneMid, kSwapIM, kReverse};
  out << "\n打錯之後，第一名還是不是「這個設定、沒打錯」的那句。\n";
  out << "括號裡是現況沒打錯的那句還在不在第一名。\n\n";
  for (int kind : kinds) {
    out << kOrderName[kind] << "\n";
    for (int arm = 0; arm < kArms; ++arm) {
      const Acc& a = acc[arm][kind];
      out << "  " << kArm[arm] << "  句數 " << a.n << "\n";
      out << "    第一名 " << pct(a.self1, a.n) << "（現況那句 "
          << pct(a.now1, a.n) << "）  前十 " << pct(a.self10, a.n)
          << "（現況那句 " << pct(a.now10, a.n) << "）\n";
      out << "    送出的句子：第一名 " << pct(a.truth1, a.n) << "  前十 "
          << pct(a.truth10, a.n) << "\n";
    }
    out << "\n";
  }
  std::printf("report %s\n", report.string().c_str());
  return 0;
}

int RankOfAny(const Hit& h, const std::vector<std::wstring>& alts) {
  int best = 0;
  for (const auto& alt : alts) {
    const int rank = RankOf(h, alt);
    if (rank && (!best || rank < best))
      best = rank;
  }
  return best;
}

bool HasTag(const std::string& tags, const char* tag) {
  const std::string pad = "," + tags + ",";
  return pad.find(std::string(",") + tag + ",") != std::string::npos;
}

std::string PairId(const std::string& tags) {
  std::string cur;
  auto take = [&](const std::string& part) -> std::string {
    if (part.size() > 4 && part.compare(0, 4, "pair") == 0)
      return part;
    return {};
  };
  for (char ch : tags) {
    if (ch == ',') {
      const std::string id = take(cur);
      if (!id.empty())
        return id;
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  return take(cur);
}

// 乾淨按鍵的選字。不造錯鍵。快打方案、聲調可接在第一個符號後面，和目前部署的拼寫相同。
// 沙盒沒有選字記憶。容錯關、開各打一輪，因為安裝版的開關目前是開。
int RunSelect(const fs::path& shared,
              const fs::path& work,
              const fs::path& grammar,
              const std::vector<Case>& cases,
              int max_homophones,
              int max_sentences,
              bool use_grammar,
              bool use_sentences,
              int collo_max,
              int collo_min) {
  struct Row {
    int off = 0;
    int on = 0;
    int strict_off = 0;
    std::wstring preview_off;
    std::wstring preview_on;
    double ms_off = 0;
    double ms_on = 0;
  };
  std::vector<Row> rows(cases.size());
  std::string version;
  {
    Engine eng;
    eng.keep_cands = 30;
    eng.scan_limit = 120;
    if (!eng.Open(shared, work / "select-off", grammar, false,
                  "bopomofo_express", false, 3, max_homophones, max_sentences,
                  use_grammar, use_sentences, collo_max, collo_min))
      return 1;
    version = eng.api->get_version ? eng.api->get_version() : "?";
    std::ofstream dump(work / "select-cands.jsonl", std::ios::binary);
    auto json_str = [](const std::wstring& w) {
      std::string s = utf8::FromWide(w);
      std::string o = "\"";
      for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\')
          o.push_back('\\');
        if (ch == '\n' || ch == '\r' || ch == '\t')
          o.push_back(' ');
        else
          o.push_back(static_cast<char>(ch));
      }
      o.push_back('"');
      return o;
    };
    for (size_t i = 0; i < cases.size(); ++i) {
      const Hit h = eng.Convert(cases[i].keys);
      rows[i].off = RankOfAny(h, cases[i].alts);
      rows[i].strict_off = RankOf(h, cases[i].truth);
      rows[i].preview_off = h.preview;
      rows[i].ms_off = h.ms;
      dump << "{\"context\":" << json_str(cases[i].context)
           << ",\"tags\":" << json_str(utf8::ToWide(cases[i].tags))
           << ",\"truth\":" << json_str(cases[i].truth) << ",\"alts\":[";
      for (size_t a = 0; a < cases[i].alts.size(); ++a)
        dump << (a ? "," : "") << json_str(cases[i].alts[a]);
      dump << "],\"preview\":" << json_str(h.preview) << ",\"cands\":[";
      for (size_t c = 0; c < h.cands.size(); ++c)
        dump << (c ? "," : "") << json_str(h.cands[c]);
      dump << "]}\n";
      if ((i + 1) % 100 == 0 || i + 1 == cases.size())
        std::printf("off %zu/%zu\n", i + 1, cases.size());
    }
    eng.Close();
  }
  {
    Engine eng;
    eng.keep_cands = 30;
    eng.scan_limit = 120;
    if (!eng.Open(shared, work / "select-on", grammar, true, "bopomofo_express",
                  false, 3, max_homophones, max_sentences, use_grammar,
                  use_sentences, collo_max, collo_min))
      return 1;
    for (size_t i = 0; i < cases.size(); ++i) {
      const Hit h = eng.Convert(cases[i].keys);
      rows[i].on = RankOfAny(h, cases[i].alts);
      rows[i].preview_on = h.preview;
      rows[i].ms_on = h.ms;
      if ((i + 1) % 100 == 0 || i + 1 == cases.size())
        std::printf("on %zu/%zu\n", i + 1, cases.size());
    }
    eng.Close();
  }

  auto pct = [](size_t a, size_t n) {
    if (!n)
      return std::string("—");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * a / n);
    return std::string(buf);
  };
  auto primary = [&](size_t i) {
    return !HasTag(cases[i].tags, "undecidable") &&
           !HasTag(cases[i].tags, "llavon");
  };
  struct Acc {
    size_t n = 0, off1 = 0, off10 = 0, on1 = 0, on10 = 0, strict1 = 0;
    size_t broke = 0, fixed = 0;
    std::vector<double> ms_off, ms_on;
    void Add(const Row& r) {
      ++n;
      off1 += r.off == 1;
      off10 += r.off && r.off <= 10;
      on1 += r.on == 1;
      on10 += r.on && r.on <= 10;
      strict1 += r.strict_off == 1;
      broke += r.off == 1 && r.on != 1;
      fixed += r.off != 1 && r.on == 1;
      ms_off.push_back(r.ms_off);
      ms_on.push_back(r.ms_on);
    }
  };
  Acc all, keep;
  std::map<std::string, Acc> by_tag;
  Acc len_short, len_mid, len_long;
  for (size_t i = 0; i < cases.size(); ++i) {
    all.Add(rows[i]);
    if (!primary(i))
      continue;
    keep.Add(rows[i]);
    const size_t nch = cases[i].truth.size();
    if (nch <= 3)
      len_short.Add(rows[i]);
    else if (nch <= 12)
      len_mid.Add(rows[i]);
    else
      len_long.Add(rows[i]);
    std::string cur;
    auto add_tag = [&](const std::string& tag) {
      if (!tag.empty())
        by_tag[tag].Add(rows[i]);
    };
    for (char ch : cases[i].tags) {
      if (ch == ',') {
        add_tag(cur);
        cur.clear();
      } else {
        cur.push_back(ch);
      }
    }
    add_tag(cur);
  }

  size_t pair_groups = 0, pair_same = 0, pair_split = 0;
  std::map<std::string, std::vector<size_t>> pairs;
  for (size_t i = 0; i < cases.size(); ++i) {
    const std::string id = PairId(cases[i].tags);
    if (!id.empty())
      pairs[id].push_back(i);
  }
  for (const auto& [id, idx] : pairs) {
    if (idx.size() < 2)
      continue;
    ++pair_groups;
    bool same = true;
    for (size_t k = 1; k < idx.size(); ++k)
      if (rows[idx[k]].preview_off != rows[idx[0]].preview_off)
        same = false;
    if (same)
      ++pair_same;
    else
      ++pair_split;
  }

  const fs::path report = work / "select-report.txt";
  std::ofstream out(report, std::ios::binary);
  out << "\xEF\xBB\xBF";
  out << "選字對照（沙盒 bopomofo_express，沒有選字記憶）\n";
  out << "librime " << version << "\n";
  out << "拼寫含聲母介音韻母不分順序，以及聲調可接在第一個符號後面。\n";
  out << (use_grammar ? "文法 " + grammar.stem().string() + " 開" : "文法關");
  if (collo_max > 0 || collo_min > 0)
    out << "，collocation_max_length " << collo_max
        << "，collocation_min_length " << collo_min;
  out << (use_sentences ? "，整句候選開" : "，整句候選關（回到方案預設）")
      << "，max_sentences " << max_sentences << "，max_homophones "
      << max_homophones << "。\n";
  out << "只打題目上的注音，不把前文送進輸入法。容錯關是建議的選字；容錯開是目"
         "前安裝版的開關。\n";
  out << "斜線隔開的寫法都算可接受。undecidable 與 llavon 不計入主數字。\n";
  out << "主數字用可接受寫法。嚴格第一名只認斜線前的那一個。\n\n";
  auto dump = [&](const char* name, const Acc& a) {
    out << name << "  " << a.n << " 題\n";
    out << "  容錯關 第一名 " << a.off1 << "（" << pct(a.off1, a.n)
        << "）  前十 " << a.off10 << "（" << pct(a.off10, a.n)
        << "）  不在前十 " << (a.n - a.off10) << "\n";
    out << "  容錯開 第一名 " << a.on1 << "（" << pct(a.on1, a.n) << "）  前十 "
        << a.on10 << "（" << pct(a.on10, a.n) << "）\n";
    out << "  嚴格第一名（容錯關，只認第一個寫法）" << a.strict1 << "（"
        << pct(a.strict1, a.n) << "）\n";
    out << "  容錯打開以後：把對的改掉 " << a.broke << "、把錯的改對 "
        << a.fixed << "\n";
    if (!a.ms_off.empty()) {
      out << "  延遲 P50/P95 容錯關 " << Percentile(a.ms_off, 0.5) << " / "
          << Percentile(a.ms_off, 0.95) << " ms";
      out << "  容錯開 " << Percentile(a.ms_on, 0.5) << " / "
          << Percentile(a.ms_on, 0.95) << " ms\n";
    }
  };
  dump("全部載入", all);
  out << "\n";
  dump("主集合（排除不可判、排除拉風驗證集）", keep);
  out << "\n長度（主集合，以第一個寫法的字數）\n";
  dump("  1～3 字", len_short);
  dump("  4～12 字", len_mid);
  dump("  13 字以上", len_long);
  out << "\n標籤（主集合裡帶這個標籤的題；一題可有多個標籤）\n";
  for (const auto& [tag, a] : by_tag) {
    if (tag == "undecidable" || tag == "llavon" ||
        tag.compare(0, 4, "pair") == 0)
      continue;
    dump(("  " + tag).c_str(), a);
  }
  out << "\n另計\n";
  Acc und, llavon;
  for (size_t i = 0; i < cases.size(); ++i) {
    if (HasTag(cases[i].tags, "undecidable"))
      und.Add(rows[i]);
    if (HasTag(cases[i].tags, "llavon"))
      llavon.Add(rows[i]);
  }
  dump("  undecidable（第一名落在列出的寫法裡就算）", und);
  dump("  llavon（只打答案的注音，不打前文）", llavon);
  out << "\n同鍵成對（標成 pair 的組，容錯關的第一名預覽）\n";
  out << "  組數 " << pair_groups << "  預覽完全相同 " << pair_same
      << "  預覽分開 " << pair_split << "\n";
  out << "前文沒有送進這次的輸入法。預覽相同表示這組同音詞沒有被句子以外的前文"
         "分開。\n";
  std::printf("report %s\n", report.string().c_str());
  std::printf("primary %zu  off1 %zu (%s)  off10 %zu (%s)  on1 %zu (%s)\n",
              keep.n, keep.off1, pct(keep.off1, keep.n).c_str(), keep.off10,
              pct(keep.off10, keep.n).c_str(), keep.on1,
              pct(keep.on1, keep.n).c_str());
  std::printf("pairs %zu same %zu split %zu  undecidable %zu  llavon %zu\n",
              pair_groups, pair_same, pair_split, und.n, llavon.n);
  return 0;
}

// 題目裡的鍵已經是亂序或打錯之後的大千鍵。orig_keys
// 是沒打錯的鍵，空的表示這題鍵是對的（同音錯字）。
int RunFault(const fs::path& shared,
             const fs::path& work,
             const fs::path& grammar,
             const std::vector<Case>& cases) {
  struct Row {
    int truth_off = 0, truth_on = 0;
    bool anchor_off = false, anchor_on = false;
    bool trap_off = false, trap_on = false;
    bool has_anchor = false, has_trap = false;
  };
  std::vector<Row> rows(cases.size());
  auto run_arm = [&](bool correction, bool on) {
    Engine eng;
    const fs::path dir = work / (correction ? "fault-on" : "fault-off");
    if (!eng.Open(shared, dir, grammar, correction, "bopomofo_express", false,
                  3))
      return std::string();
    std::map<std::string, Hit> clean;
    for (size_t i = 0; i < cases.size(); ++i) {
      if (cases[i].orig_keys.empty())
        continue;
      if (!clean.count(cases[i].orig_keys))
        clean.emplace(cases[i].orig_keys, eng.Convert(cases[i].orig_keys));
    }
    for (size_t i = 0; i < cases.size(); ++i) {
      const Hit h = eng.Convert(cases[i].keys);
      const int truth = RankOfAny(h, cases[i].alts);
      Row& r = rows[i];
      if (on) {
        r.truth_on = truth;
        r.trap_on = !cases[i].trap.empty() && h.preview == cases[i].trap;
      } else {
        r.truth_off = truth;
        r.trap_off = !cases[i].trap.empty() && h.preview == cases[i].trap;
      }
      r.has_trap = !cases[i].trap.empty();
      if (!cases[i].orig_keys.empty()) {
        r.has_anchor = true;
        const bool same = h.preview == clean[cases[i].orig_keys].preview;
        if (on)
          r.anchor_on = same;
        else
          r.anchor_off = same;
      }
      if ((i + 1) % 500 == 0 || i + 1 == cases.size())
        std::printf("%s %zu/%zu\n", on ? "on" : "off", i + 1, cases.size());
    }
    const std::string version =
        eng.api->get_version ? eng.api->get_version() : "?";
    eng.Close();
    return version;
  };
  const std::string version = run_arm(false, false);
  if (version.empty())
    return 1;
  if (run_arm(true, true).empty())
    return 1;

  auto pct = [](size_t a, size_t n) {
    if (!n)
      return std::string("—");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * a / n);
    return std::string(buf);
  };
  struct Acc {
    size_t n = 0, off1 = 0, off10 = 0, on1 = 0, on10 = 0;
    size_t anchor_n = 0, anchor_off = 0, anchor_on = 0;
    size_t trap_n = 0, trap_off = 0, trap_on = 0;
    void Add(const Row& r) {
      ++n;
      off1 += r.truth_off == 1;
      off10 += r.truth_off && r.truth_off <= 10;
      on1 += r.truth_on == 1;
      on10 += r.truth_on && r.truth_on <= 10;
      if (r.has_anchor) {
        ++anchor_n;
        anchor_off += r.anchor_off;
        anchor_on += r.anchor_on;
      }
      if (r.has_trap) {
        ++trap_n;
        trap_off += r.trap_off;
        trap_on += r.trap_on;
      }
    }
  };
  std::map<std::string, Acc> by_tag;
  Acc all;
  for (size_t i = 0; i < cases.size(); ++i) {
    all.Add(rows[i]);
    std::string cur;
    auto add = [&](const std::string& tag) {
      if (tag.size() >= 2 &&
          (tag[0] == 'o' || tag[0] == 'k' || tag[0] == 'h' || tag[0] == 'g') &&
          tag[1] == '-')
        by_tag[tag].Add(rows[i]);
    };
    for (char ch : cases[i].tags) {
      if (ch == ',') {
        add(cur);
        cur.clear();
      } else {
        cur.push_back(ch);
      }
    }
    add(cur);
  }

  const fs::path report = work / "fault-report.txt";
  std::ofstream out(report, std::ios::binary);
  out << "\xEF\xBB\xBF";
  out << "注音亂序與錯字（沙盒 bopomofo_express，沒有選字記憶）\n";
  out << "librime " << version << "\n";
  out << "拼寫含不分順序，以及聲調可接在第一個符號後面。鄰鍵容錯另列一欄。\n";
  out << "「回到原鍵第一名」＝亂序或打錯之後的預覽，和同一題沒打錯的第一名相同"
         "。\n";
  out << "「標註第一名」＝預覽等於題目寫的正確句。原鍵自己選錯時，兩者不會一樣"
         "。\n\n";
  auto dump = [&](const char* name, const Acc& a) {
    out << name << "  " << a.n << " 題\n";
    out << "  容錯關 標註第一名 " << a.off1 << "（" << pct(a.off1, a.n)
        << "）  前十 " << a.off10 << "（" << pct(a.off10, a.n) << "）\n";
    out << "  容錯開 標註第一名 " << a.on1 << "（" << pct(a.on1, a.n)
        << "）  前十 " << a.on10 << "（" << pct(a.on10, a.n) << "）\n";
    if (a.anchor_n) {
      out << "  回到原鍵第一名  容錯關 " << a.anchor_off << "（"
          << pct(a.anchor_off, a.anchor_n) << "）  容錯開 " << a.anchor_on
          << "（" << pct(a.anchor_on, a.anchor_n) << "）\n";
    }
    if (a.trap_n) {
      out << "  第一名正好是寫出的錯字  容錯關 " << a.trap_off << "（"
          << pct(a.trap_off, a.trap_n) << "）  容錯開 " << a.trap_on << "（"
          << pct(a.trap_on, a.trap_n) << "）\n";
    }
  };
  dump("全部", all);
  out << "\n";
  for (const auto& [tag, a] : by_tag)
    dump(tag.c_str(), a);
  std::printf("report %s\n", report.string().c_str());
  std::printf("fault %zu  off1 %zu (%s)  anchor %zu/%zu\n", all.n, all.off1,
              pct(all.off1, all.n).c_str(), all.anchor_off, all.anchor_n);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: typo_probe <rime_shared_dir> <work_dir> <cases|--> "
                 "<grammar.gram> "
                 "[limit] [--log file] [--sample] [--order] [--select] "
                 "[--fault] [--homo N] "
                 "[--sentences N] [--no-grammar] [--no-sentences] "
                 "[--collo-max N] [--collo-min N]\n");
    return 2;
  }
  const fs::path shared = argv[1], work = argv[2], grammar = argv[4];
  fs::path cases_path =
      std::strcmp(argv[3], "--") == 0 ? fs::path() : fs::path(argv[3]);
  fs::path log_path;
  size_t limit = 0;
  bool sample = false;
  bool order = false;
  bool select = false;
  bool fault = false;
  int max_homophones = 7;
  int max_sentences = 10;
  bool use_grammar = true;
  bool use_sentences = true;
  int collo_max = 0;
  int collo_min = 0;
  for (int i = 5; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--sample")
      sample = true;
    else if (a == "--order")
      order = true;
    else if (a == "--select")
      select = true;
    else if (a == "--fault")
      fault = true;
    else if (a == "--homo" && i + 1 < argc)
      max_homophones = std::atoi(argv[++i]);
    else if (a == "--no-grammar")
      use_grammar = false;
    else if (a == "--no-sentences")
      use_sentences = false;
    else if (a == "--sentences" && i + 1 < argc)
      max_sentences = std::atoi(argv[++i]);
    else if (a == "--collo-max" && i + 1 < argc)
      collo_max = std::atoi(argv[++i]);
    else if (a == "--collo-min" && i + 1 < argc)
      collo_min = std::atoi(argv[++i]);
    else if (a == "--log" && i + 1 < argc)
      log_path = argv[++i];
    else
      limit = (size_t)std::atoll(a.c_str());
  }
  std::vector<Case> cases;
  size_t skipped = 0;
  if (!log_path.empty()) {
    if (!LoadLog(log_path, &cases, &skipped, limit)) {
      std::fprintf(stderr, "cannot read log\n");
      return 1;
    }
  }
  if (!cases_path.empty() && !LoadCases(cases_path, &cases, limit)) {
    std::fprintf(stderr, "cannot read cases\n");
    return 1;
  }
  if (cases.empty()) {
    std::fprintf(stderr, "no cases\n");
    return 1;
  }
  std::printf("cases %zu  skipped %zu  sample %d\n", cases.size(), skipped,
              sample ? 1 : 0);
  if (select)
    return RunSelect(shared, work, grammar, cases, max_homophones,
                     max_sentences, use_grammar, use_sentences, collo_max,
                     collo_min);
  if (fault)
    return RunFault(shared, work, grammar, cases);
  if (order)
    return RunOrder(shared, work, grammar, cases);
  const std::string how =
      log_path.empty() ? "題目：公開題目。每句把每一種打錯都測過。"
                       : (sample ? "題目：選字紀錄。每句每種打錯只抽一次。注音"
                                   "是送出時的按鍵，錯鍵是事後加上的。"
                                 : "題目：選字紀錄。每句把每一種打錯都測過。");

  std::vector<Hit> clean_off(cases.size()), clean_on(cases.size());
  std::vector<std::vector<Hit>> err_off(cases.size()), err_on(cases.size());
  std::vector<std::vector<Mut>> muts(cases.size());
  for (size_t i = 0; i < cases.size(); ++i) {
    const auto all = Mutations(cases[i].keys);
    muts[i] =
        sample ? SampleOneEach(all, static_cast<uint32_t>(i) * 17u + 1u) : all;
  }
  std::vector<double> clean_off_ms, clean_on_ms, err_off_ms, err_on_ms;

  {
    Engine eng;
    if (!eng.Open(shared, work / "typo-off", grammar, false))
      return 1;
    for (size_t i = 0; i < cases.size(); ++i) {
      clean_off[i] = eng.Convert(cases[i].keys);
      clean_off_ms.push_back(clean_off[i].ms);
      err_off[i].reserve(muts[i].size());
      for (const auto& m : muts[i]) {
        err_off[i].push_back(eng.Convert(m.keys));
        err_off_ms.push_back(err_off[i].back().ms);
      }
      if ((i + 1) % 100 == 0 || i + 1 == cases.size())
        std::printf("off %zu/%zu  errors %zu\n", i + 1, cases.size(),
                    muts[i].size());
    }
    eng.Close();
  }
  std::string version;
  {
    Engine eng;
    if (!eng.Open(shared, work / "typo-on", grammar, true))
      return 1;
    version = eng.api->get_version ? eng.api->get_version() : "?";
    for (size_t i = 0; i < cases.size(); ++i) {
      clean_on[i] = eng.Convert(cases[i].keys);
      clean_on_ms.push_back(clean_on[i].ms);
      err_on[i].reserve(muts[i].size());
      for (const auto& m : muts[i]) {
        err_on[i].push_back(eng.Convert(m.keys));
        err_on_ms.push_back(err_on[i].back().ms);
      }
      if ((i + 1) % 100 == 0 || i + 1 == cases.size())
        std::printf("on %zu/%zu\n", i + 1, cases.size());
    }
    eng.Close();
  }

  size_t clean_truth = 0, clean_same = 0, usable = 0;
  size_t clean_to_truth = 0, clean_from_truth = 0, clean_other = 0;
  std::vector<std::vector<Trial>> by_case(cases.size());
  for (size_t i = 0; i < cases.size(); ++i) {
    if (clean_off[i].preview.empty())
      continue;
    ++usable;
    const bool off_right = clean_off[i].preview == cases[i].truth;
    const bool on_right = clean_on[i].preview == cases[i].truth;
    const bool same = clean_on[i].preview == clean_off[i].preview;
    clean_truth += off_right;
    clean_same += same;
    if (!same) {
      if (!off_right && on_right)
        ++clean_to_truth;
      else if (off_right && !on_right)
        ++clean_from_truth;
      else
        ++clean_other;
    }
    const std::wstring& anchor = clean_off[i].preview;
    for (size_t j = 0; j < muts[i].size(); ++j) {
      Trial t;
      t.kind = muts[i][j].kind;
      t.tone = muts[i][j].tone;
      t.anchor_off = RankOf(err_off[i][j], anchor);
      t.anchor_on = RankOf(err_on[i][j], anchor);
      t.truth_off = RankOf(err_off[i][j], cases[i].truth);
      t.truth_on = RankOf(err_on[i][j], cases[i].truth);
      by_case[i].push_back(t);
    }
  }
  const fs::path report =
      work / (log_path.empty() ? "typo-report.txt" : "typo-log-report.txt");
  WriteReport(report, version, how, usable, clean_truth, clean_same,
              usable - clean_same, clean_to_truth, clean_from_truth,
              clean_other, clean_off_ms, clean_on_ms, err_off_ms, err_on_ms,
              by_case);
  std::printf("report %s\n", report.string().c_str());
  return 0;
}
