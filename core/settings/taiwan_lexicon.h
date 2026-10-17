#pragma once

// Bundled vocabulary initialization and management. No Python/runtime download is needed.
// Only our resource files, preference file and marked schema patches are managed.
#include <rime_api.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../third_party/nlohmann/json.hpp"

namespace settings::taiwan_lexicon {
namespace fs = std::filesystem;
using json = nlohmann::json;

inline constexpr const char* kLexiconId = "wisdom_taiwan";
inline constexpr const char* kExportFormat = "wisdom-lexicon-edits/v1";
inline constexpr int kUserFormatVersion = 1;
inline constexpr size_t kMinWordChars = 1;
inline constexpr size_t kMaxWordChars = 12;
inline constexpr size_t kMaxImportBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxImportEdits = 10000;

inline constexpr const char* kBegin = "  # >>> wisdom-taiwan-lexicon";
inline constexpr const char* kEnd = "  # <<< wisdom-taiwan-lexicon";
inline constexpr const char* kFiles[] = {
    "wisdom_taiwan.dict.yaml", "wisdom_taiwan.manifest.json", "wisdom_taiwan.NOTICE.txt",
    "wisdom_taiwan.sources.zip"};
inline constexpr const char* kApplicableSchemas[] = {"bopomofo", "bopomofo_express", "bopomofo_tw"};

inline std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read " + path.u8string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void Write(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || !(out << text)) throw std::runtime_error("Cannot write " + path.u8string());
  out.close();
  if (!out) throw std::runtime_error("Cannot flush " + path.u8string());
}

inline std::vector<std::string> Keys(RimeApi* api, RimeConfig* config, const char* path) {
  RimeConfigIterator iter = {};
  std::vector<std::string> keys;
  if (!api->config_begin_map(&iter, config, path))
    throw std::runtime_error("Expected a YAML map at " + std::string(path));
  while (api->config_next(&iter)) keys.emplace_back(iter.key);
  api->config_end(&iter);
  return keys;
}

inline bool Has(const std::vector<std::string>& keys, const char* name) {
  return std::find(keys.begin(), keys.end(), name) != keys.end();
}

// ---------------------------------------------------------------------------
// UTF-8 & Han Character Validation

inline bool HasControlOrTab(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7F) return true;
  }
  return false;
}

inline bool DecodeUtf8(const std::string& s, std::vector<uint32_t>* out) {
  out->clear();
  for (size_t i = 0; i < s.size();) {
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
      out->push_back(c0);
      ++i;
    } else if ((c0 & 0xE0) == 0xC0) {
      if (i + 1 >= s.size()) return false;
      const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c1 & 0xC0) != 0x80) return false;
      const uint32_t cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
      if (cp < 0x80) return false;
      out->push_back(cp);
      i += 2;
    } else if ((c0 & 0xF0) == 0xE0) {
      if (i + 2 >= s.size()) return false;
      const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
      const uint32_t cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
      out->push_back(cp);
      i += 3;
    } else if ((c0 & 0xF8) == 0xF0) {
      if (i + 3 >= s.size()) return false;
      const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
      const unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
      const uint32_t cp =
          ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
      if (cp < 0x10000 || cp > 0x10FFFF) return false;
      out->push_back(cp);
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

inline std::string EncodeUtf8(uint32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

inline bool IsHanChar(uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0x20000 && cp <= 0x323AF);
}

inline std::string TrimSpaces(const std::string& s) {
  std::vector<uint32_t> cps;
  if (!DecodeUtf8(s, &cps)) return s;
  auto is_space = [](uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0x3000;
  };
  size_t b = 0, e = cps.size();
  while (b < e && is_space(cps[b])) ++b;
  while (e > b && is_space(cps[e - 1])) --e;
  std::string out;
  for (size_t i = b; i < e; ++i) out += EncodeUtf8(cps[i]);
  return out;
}

// ---------------------------------------------------------------------------
// Pinyin <-> Zhuyin Conversion & Syllable Validation

inline std::string PinyinSyllableToZhuyinUncached(std::string s) {
  if (s.empty()) return "";
  if (s.size() == 2 && s[0] == 'm' && s[1] >= '1' && s[1] <= '5') {
    s = std::string("mu") + s[1];
  }
  if (s == "r5" || s == "er5") {
    s = "er5";
  }
  auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
      str.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all(s, "iu", "iou");
  replace_all(s, "ui", "uei");
  replace_all(s, "ong", "ung");
  if (s.rfind("yi", 0) == 0) s.replace(0, 2, "i");
  else if (s.rfind("y", 0) == 0) s.replace(0, 1, "i");
  if (s.rfind("wu", 0) == 0) s.replace(0, 2, "u");
  else if (s.rfind("w", 0) == 0) s.replace(0, 1, "u");
  replace_all(s, "iu", "v");
  if (s.size() >= 2 && (s[0] == 'j' || s[0] == 'q' || s[0] == 'x') && s[1] == 'u') {
    s[1] = 'v';
  }
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if ((s[i] == 'i' || s[i] == 'u' || s[i] == 'v') && s[i + 1] == 'n') {
      s.insert(i + 1, "e");
      ++i;
    }
  }
  if (s.rfind("zhi", 0) == 0) s.replace(0, 3, "Z");
  else if (s.rfind("zh", 0) == 0) s.replace(0, 2, "Z");
  else if (s.rfind("chi", 0) == 0) s.replace(0, 3, "C");
  else if (s.rfind("ch", 0) == 0) s.replace(0, 2, "C");
  else if (s.rfind("shi", 0) == 0) s.replace(0, 3, "S");
  else if (s.rfind("sh", 0) == 0) s.replace(0, 2, "S");
  else if (s.size() >= 2 && (s[0] == 'z' || s[0] == 'c' || s[0] == 's' || s[0] == 'r') &&
           s[1] == 'i') {
    s.erase(1, 1);
  }
  const std::pair<const char*, const char*> finals[] = {
      {"ai", "A"}, {"ei", "I"},  {"ao", "O"},  {"ou", "U"}, {"ang", "K"},
      {"eng", "G"}, {"an", "M"}, {"en", "N"}, {"er", "R"}, {"eh", "E"}};
  for (const auto& [from, to] : finals) replace_all(s, from, to);
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if ((s[i] == 'i' || s[i] == 'v') && s[i + 1] == 'e') s[i + 1] = 'E';
  }
  replace_all(s, "1", "");

  static const std::string kSrc = "bpmfdtnlgkhjqxZCSrzcsiuvaoeEAIOUMNKGR2345";
  static const char* const kDst[] = {
      u8"ㄅ", u8"ㄆ", u8"ㄇ", u8"ㄈ", u8"ㄉ", u8"ㄊ", u8"ㄋ", u8"ㄌ", u8"ㄍ", u8"ㄎ", u8"ㄏ",
      u8"ㄐ", u8"ㄑ", u8"ㄒ", u8"ㄓ", u8"ㄔ", u8"ㄕ", u8"ㄖ", u8"ㄗ", u8"ㄘ", u8"ㄙ", u8"ㄧ",
      u8"ㄨ", u8"ㄩ", u8"ㄚ", u8"ㄛ", u8"ㄜ", u8"ㄝ", u8"ㄞ", u8"ㄟ", u8"ㄠ", u8"ㄡ", u8"ㄢ",
      u8"ㄣ", u8"ㄤ", u8"ㄥ", u8"ㄦ", u8"ˊ",  u8"ˇ",  u8"ˋ",  u8"˙"};
  std::string out;
  for (char c : s) {
    const size_t pos = kSrc.find(c);
    if (pos != std::string::npos) out += kDst[pos];
    else out.push_back(c);
  }
  return out;
}

inline std::string PinyinSyllableToZhuyin(const std::string& s) {
  if (s.empty()) return "";
  thread_local std::unordered_map<std::string, std::string> cache;
  auto it = cache.find(s);
  if (it != cache.end()) return it->second;
  std::string res = PinyinSyllableToZhuyinUncached(s);
  if (cache.size() < 8192) cache.emplace(s, res);
  return res;
}

inline std::string CodeToZhuyin(const std::string& code) {
  std::string out;
  size_t i = 0;
  const size_t n = code.size();
  while (i < n) {
    while (i < n && (code[i] == ' ' || code[i] == '\t')) ++i;
    if (i >= n) break;
    size_t j = i;
    while (j < n && code[j] != ' ' && code[j] != '\t') ++j;
    if (!out.empty()) out.push_back(' ');
    out += PinyinSyllableToZhuyin(code.substr(i, j - i));
    i = j;
  }
  return out;
}

inline std::string CompactZhuyin(const std::string& zhuyin) {
  std::vector<uint32_t> cps;
  if (!DecodeUtf8(zhuyin, &cps)) return zhuyin;
  std::string out;
  for (uint32_t cp : cps) {
    if (cp == ' ' || cp == 0x3000 || cp == 0x02C9 || cp == 0xFF3E) continue;
    out += EncodeUtf8(cp);
  }
  return out;
}

struct SyllableTable {
  std::unordered_map<std::string, std::string> code_to_zh;
  std::unordered_map<std::string, std::string> zh_to_code;

  void AddSyllable(const std::string& py) {
    if (py.empty() || code_to_zh.find(py) != code_to_zh.end()) return;
    const std::string zh = PinyinSyllableToZhuyin(py);
    if (zh.empty()) return;
    code_to_zh[py] = zh;
    // Prefer canonical er5 over r5, weng4 over wong4
    auto it = zh_to_code.find(zh);
    if (it == zh_to_code.end() || (it->second == "r5" && py == "er5") ||
        (it->second == "wong4" && py == "weng4")) {
      zh_to_code[zh] = py;
    }
  }
};

inline void AddSyllablesFromDictFile(const fs::path& dict_path, SyllableTable* table) {
  std::ifstream in(dict_path, std::ios::binary);
  if (!in) return;
  std::string line;
  bool in_body = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!in_body) {
      if (line == "...") in_body = true;
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    const size_t end = (t2 == std::string::npos) ? line.size() : t2;
    size_t i = t1 + 1;
    while (i < end) {
      while (i < end && (line[i] == ' ' || line[i] == '\t')) ++i;
      if (i >= end) break;
      size_t j = i;
      while (j < end && line[j] != ' ' && line[j] != '\t') ++j;
      table->AddSyllable(line.substr(i, j - i));
      i = j;
    }
  }
}

inline SyllableTable LoadSyllableTable(const fs::path& shared) {
  SyllableTable table;
  AddSyllablesFromDictFile(shared / "terra_pinyin.dict.yaml", &table);
  AddSyllablesFromDictFile(shared / "wisdom_taiwan.dict.yaml", &table);
  return table;
}

inline bool LoadSyllableTable(const fs::path& lexicon_dir, const fs::path& rime_shared_dir,
                              SyllableTable* out) {
  *out = SyllableTable{};
  AddSyllablesFromDictFile(rime_shared_dir / "terra_pinyin.dict.yaml", out);
  if (lexicon_dir != rime_shared_dir)
    AddSyllablesFromDictFile(lexicon_dir / "terra_pinyin.dict.yaml", out);
  AddSyllablesFromDictFile(lexicon_dir / "wisdom_taiwan.dict.yaml", out);
  return !out->code_to_zh.empty();
}

inline int ZhuyinCharKind(uint32_t cp) {
  if (cp >= 0x3105 && cp <= 0x3119) return 1;  // Initial (聲母 ㄅ..ㄙ)
  if (cp >= 0x3127 && cp <= 0x3129) return 2;  // Medial (介音 ㄧㄨㄩ)
  if (cp >= 0x311A && cp <= 0x3126) return 3;  // Final (韻母 ㄚ..ㄦ)
  if (cp == 0x02C9 || cp == 0xFF3E || cp == 0x02CA || cp == 0x02C7 || cp == 0x02CB || cp == 0x02D9)
    return 4;  // Tone (聲調 ˉˊˇˋ˙)
  return 0;
}

// Normalize a single Zhuyin syllable's codepoints: check ordering, at most 1 tone, move ˙ to end,
// strip ˉ.
inline bool NormalizeSingleZhuyinSyllable(const std::vector<uint32_t>& cps, std::string* canonical,
                                          std::string* raw_str, std::string* error) {
  *raw_str = "";
  for (uint32_t cp : cps) *raw_str += EncodeUtf8(cp);
  uint32_t tone = 0;
  std::vector<uint32_t> body;
  int last_kind = 0;
  for (size_t i = 0; i < cps.size(); ++i) {
    const uint32_t cp = cps[i];
    const int k = ZhuyinCharKind(cp);
    if (k == 0) {
      if (error) *error = u8"讀音包含無效字元：「" + *raw_str + u8"」。";
      return false;
    }
    if (k == 4) {
      if (tone != 0) {
        if (error) *error = u8"音節「" + *raw_str + u8"」包含重複的聲調符號。";
        return false;
      }
      // Leading tone is only valid for neutral tone ˙ (U+02D9)
      if (body.empty() && cp != 0x02D9) {
        if (error) *error = u8"聲調符號不能單獨或置於音節開頭：「" + *raw_str + u8"」。";
        return false;
      }
      tone = cp;
    } else {
      if (tone != 0 && !(tone == 0x02D9 && body.empty())) {
        if (error) *error = u8"音節「" + *raw_str + u8"」的聲調位置不正確。";
        return false;
      }
      if (k <= last_kind) {
        if (error) *error = u8"音節「" + *raw_str + u8"」的聲母、介音、韻母順序不正確。";
        return false;
      }
      last_kind = k;
      body.push_back(cp);
    }
  }
  if (body.empty()) {
    if (error) *error = u8"聲調符號缺少對應的注音符號：「" + *raw_str + u8"」。";
    return false;
  }
  *canonical = "";
  for (uint32_t cp : body) *canonical += EncodeUtf8(cp);
  if (tone != 0 && tone != 0x02C9 && tone != 0xFF3E) {
    *canonical += EncodeUtf8(tone);
  }
  return true;
}

// Segment a contiguous sequence of Zhuyin codepoints into `target_count` valid syllables (if
// target_count > 0), or greedily if target_count == 0.
inline bool SegmentZhuyinChunk(const std::vector<uint32_t>& cps, const SyllableTable& table,
                               size_t target_count, std::vector<std::string>* out_codes,
                               std::vector<std::string>* out_zhs, std::string* error) {
  const size_t n = cps.size();
  if (n == 0) return true;

  // Try exact valid syllable DP when target_count > 0
  if (target_count > 0) {
    struct State {
      bool reachable = false;
      size_t prev_pos = 0;
      std::string code;
      std::string zh;
    };
    std::vector<std::vector<State>> dp(target_count + 1, std::vector<State>(n + 1));
    dp[0][0].reachable = true;
    for (size_t step = 0; step < target_count; ++step) {
      for (size_t i = 0; i < n; ++i) {
        if (!dp[step][i].reachable) continue;
        // A single Zhuyin syllable has 1..4 codepoints
        for (size_t len = 1; len <= 4 && i + len <= n; ++len) {
          std::vector<uint32_t> sub(cps.begin() + i, cps.begin() + i + len);
          std::string canon, raw;
          if (!NormalizeSingleZhuyinSyllable(sub, &canon, &raw, nullptr)) continue;
          auto it = table.zh_to_code.find(canon);
          if (it == table.zh_to_code.end()) continue;
          // Prefer greedy longest first syllable
          if (!dp[step + 1][i + len].reachable) {
            dp[step + 1][i + len] = {true, i, it->second, canon};
          }
        }
      }
    }
    if (dp[target_count][n].reachable) {
      std::vector<std::string> codes(target_count), zhs(target_count);
      size_t cur = n;
      for (size_t step = target_count; step > 0; --step) {
        codes[step - 1] = dp[step][cur].code;
        zhs[step - 1] = dp[step][cur].zh;
        cur = dp[step][cur].prev_pos;
      }
      out_codes->insert(out_codes->end(), codes.begin(), codes.end());
      out_zhs->insert(out_zhs->end(), zhs.begin(), zhs.end());
      return true;
    }
  }

  // Fallback: structural segmentation to provide accurate error message
  std::vector<std::vector<uint32_t>> raw_syllables;
  std::vector<uint32_t> cur;
  int last_kind = 0;
  bool has_body = false;
  bool has_trailing_tone = false;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t cp = cps[i];
    const int k = ZhuyinCharKind(cp);
    if (k == 0) {
      if (error) *error = u8"讀音包含非注音字元：「" + EncodeUtf8(cp) + u8"」。";
      return false;
    }
    if (k == 4) {
      if (cp == 0x02D9 && (!has_body || has_trailing_tone)) {
        if (!cur.empty()) raw_syllables.push_back(cur);
        cur = {cp};
        last_kind = 0;
        has_body = false;
        has_trailing_tone = false;
      } else {
        cur.push_back(cp);
        has_trailing_tone = true;
      }
    } else {
      if (has_trailing_tone || (has_body && k <= last_kind)) {
        raw_syllables.push_back(cur);
        cur.clear();
        last_kind = 0;
        has_body = false;
        has_trailing_tone = false;
      }
      cur.push_back(cp);
      last_kind = k;
      has_body = true;
    }
  }
  if (!cur.empty()) raw_syllables.push_back(cur);

  for (const auto& sub : raw_syllables) {
    std::string canon, raw, sub_err;
    if (!NormalizeSingleZhuyinSyllable(sub, &canon, &raw, &sub_err)) {
      if (error) *error = sub_err;
      return false;
    }
    auto it = table.zh_to_code.find(canon);
    if (it == table.zh_to_code.end()) {
      if (error) *error = u8"注音音節「" + raw + u8"」不在支援的音節表中。";
      return false;
    }
    out_codes->push_back(it->second);
    out_zhs->push_back(canon);
  }
  return true;
}

inline bool ValidateWord(const std::string& raw_word, std::string* out_word, size_t* out_char_count,
                         std::string* error) {
  if (HasControlOrTab(raw_word)) {
    if (error) *error = u8"詞彙不能包含 Tab、換行或控制字元。";
    return false;
  }
  const std::string word = TrimSpaces(raw_word);
  if (word.empty()) {
    if (error) *error = u8"請輸入詞彙。";
    return false;
  }
  std::vector<uint32_t> cps;
  if (!DecodeUtf8(word, &cps)) {
    if (error) *error = u8"詞彙的 UTF-8 編碼無效。";
    return false;
  }
  if (cps.size() < kMinWordChars || cps.size() > kMaxWordChars) {
    if (error)
      *error = u8"內建詞庫詞彙長度須為 " + std::to_string(kMinWordChars) + u8"～" +
               std::to_string(kMaxWordChars) + u8" 個漢字（目前為 " + std::to_string(cps.size()) +
               u8" 字）。";
    return false;
  }
  for (uint32_t cp : cps) {
    if (!IsHanChar(cp)) {
      if (error) *error = u8"詞彙只能包含漢字（不可含空白、標點或英數字）：「" + EncodeUtf8(cp) + u8"」。";
      return false;
    }
  }
  if (out_word) *out_word = word;
  if (out_char_count) *out_char_count = cps.size();
  return true;
}

inline bool ParseAndValidateReading(const std::string& raw_word, const std::string& raw_reading,
                                    const SyllableTable& table, std::string* out_word,
                                    std::string* out_code, std::string* out_zhuyin,
                                    std::string* error) {
  std::string word;
  size_t char_count = 0;
  if (!ValidateWord(raw_word, &word, &char_count, error)) return false;

  if (HasControlOrTab(raw_reading)) {
    if (error) *error = u8"讀音不能包含 Tab、換行或控制字元。";
    return false;
  }
  const std::string reading = TrimSpaces(raw_reading);
  if (reading.empty()) {
    if (error) *error = u8"請輸入完整注音讀音。";
    return false;
  }
  std::vector<uint32_t> cps;
  if (!DecodeUtf8(reading, &cps)) {
    if (error) *error = u8"讀音的 UTF-8 編碼無效。";
    return false;
  }

  auto is_space = [](uint32_t cp) { return cp == ' ' || cp == 0x3000; };
  bool all_ascii = true;
  for (uint32_t cp : cps) {
    if (!is_space(cp) && !(cp >= 'a' && cp <= 'z') && !(cp >= 'A' && cp <= 'Z') &&
        !(cp >= '1' && cp <= '5')) {
      all_ascii = false;
      break;
    }
  }

  std::vector<std::string> codes, zhs;
  if (all_ascii) {
    std::string cur;
    std::vector<std::string> tokens;
    for (uint32_t cp : cps) {
      if (is_space(cp)) {
        if (!cur.empty()) {
          tokens.push_back(cur);
          cur.clear();
        }
      } else {
        cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(cp))));
      }
    }
    if (!cur.empty()) tokens.push_back(cur);
    for (std::string tok : tokens) {
      if (tok.back() < '1' || tok.back() > '5') tok.push_back('1');
      if (tok == "r5") tok = "er5";
      if (tok == "wong4") tok = "weng4";
      auto it = table.code_to_zh.find(tok);
      if (it == table.code_to_zh.end()) {
        if (error) *error = u8"拼音音節「" + tok + u8"」不在支援的音節表中。";
        return false;
      }
      codes.push_back(tok);
      zhs.push_back(it->second);
    }
  } else {
    std::vector<std::vector<uint32_t>> chunks;
    std::vector<uint32_t> cur;
    for (uint32_t cp : cps) {
      if (is_space(cp)) {
        if (!cur.empty()) {
          chunks.push_back(cur);
          cur.clear();
        }
      } else {
        cur.push_back(cp);
      }
    }
    if (!cur.empty()) chunks.push_back(cur);
    // Merge standalone leading neutral tone ˙ with following chunk
    for (size_t i = 0; i + 1 < chunks.size();) {
      if (chunks[i].size() == 1 && chunks[i][0] == 0x02D9) {
        chunks[i + 1].insert(chunks[i + 1].begin(), 0x02D9);
        chunks.erase(chunks.begin() + i);
      } else {
        ++i;
      }
    }
    if (chunks.size() == 1) {
      if (!SegmentZhuyinChunk(chunks[0], table, char_count, &codes, &zhs, error)) return false;
    } else {
      for (const auto& chunk : chunks) {
        const size_t target = (chunks.size() == char_count) ? 1 : 0;
        if (!SegmentZhuyinChunk(chunk, table, target, &codes, &zhs, error)) return false;
      }
    }
  }

  if (codes.size() != char_count) {
    if (error)
      *error = u8"詞彙字數（" + std::to_string(char_count) + u8" 字）與注音音節數（" +
               std::to_string(codes.size()) + u8" 個音節）不一致。";
    return false;
  }

  if (out_word) *out_word = word;
  if (out_code) {
    out_code->clear();
    for (size_t i = 0; i < codes.size(); ++i) {
      if (i) out_code->push_back(' ');
      *out_code += codes[i];
    }
  }
  if (out_zhuyin) {
    out_zhuyin->clear();
    for (size_t i = 0; i < zhs.size(); ++i) {
      if (i) out_zhuyin->push_back(' ');
      *out_zhuyin += zhs[i];
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Personal Modifications & Effective Lexicon Generation

struct UserEdit {
  std::string word;
  std::string code;
  bool hidden = false;
  std::string pref = "normal";  // "low", "normal", "high"
  bool origin_base = false;
  int base_weight = 1000;
  std::string replaced_from_word;
  std::string replaced_from_code;

  bool operator==(const UserEdit& o) const {
    return word == o.word && code == o.code && hidden == o.hidden && pref == o.pref &&
           origin_base == o.origin_base && base_weight == o.base_weight &&
           replaced_from_word == o.replaced_from_word && replaced_from_code == o.replaced_from_code;
  }
  bool operator!=(const UserEdit& o) const { return !(*this == o); }
};

inline bool IsValidPref(const std::string& pref) {
  return pref == "low" || pref == "normal" || pref == "high";
}

inline int EffectiveWeight(bool in_base, int base_weight, const std::string& pref) {
  const int w = (in_base && base_weight > 0) ? base_weight : 1000;
  if (pref == "low") return (std::max)(10, (std::min)(99, w / 10));
  if (pref == "high") return (std::max)(10000, (std::min)(50000, w * 5));
  return w;
}

inline json EditToJson(const UserEdit& e) {
  return json{{"word", e.word},
              {"code", e.code},
              {"zhuyin", CodeToZhuyin(e.code)},
              {"hidden", e.hidden},
              {"pref", e.pref},
              {"origin_base", e.origin_base},
              {"base_weight", e.base_weight},
              {"replaced_from_word", e.replaced_from_word},
              {"replaced_from_code", e.replaced_from_code}};
}

inline bool EditFromJson(const json& j, UserEdit* out) {
  if (!j.is_object()) return false;
  out->word = j.value("word", "");
  out->code = j.value("code", "");
  out->hidden = j.value("hidden", false);
  out->pref = j.value("pref", "normal");
  if (!IsValidPref(out->pref)) out->pref = "normal";
  out->origin_base = j.value("origin_base", false);
  out->base_weight = j.value("base_weight", 1000);
  out->replaced_from_word = j.value("replaced_from_word", "");
  out->replaced_from_code = j.value("replaced_from_code", "");
  return !out->word.empty() && !out->code.empty();
}

struct UserConfig {
  int version = 0;  // 0 = legacy (only enabled), 1 = v1
  bool enabled = true;
  int edits_revision = 0;
  std::string deployed_base_version;
  std::string deployed_base_sha256;
  int deployed_edits_revision = 0;
  std::vector<UserEdit> edits;
};

inline bool ParseUserConfigText(RimeApi* api, const std::string& text, UserConfig* out,
                                std::string* error = nullptr) {
  *out = UserConfig{};
  if (api) {
    RimeConfig config = {};
    if (!api->config_load_string(&config, text.c_str())) {
      if (error) *error = "Invalid wisdom_taiwan.user.yaml";
      return false;
    }
    Bool flag = True;
    const bool valid = api->config_get_bool(&config, "enabled", &flag);
    if (!valid) {
      api->config_close(&config);
      if (error) *error = "Missing enabled in wisdom_taiwan.user.yaml";
      return false;
    }
    out->enabled = !!flag;
    int ver = 0;
    if (api->config_get_int(&config, "version", &ver)) {
      if (ver < 0 || ver > kUserFormatVersion) {
        api->config_close(&config);
        if (error) *error = "Unsupported version in wisdom_taiwan.user.yaml";
        return false;
      }
      out->version = ver;
    }
    int rev = 0;
    if (api->config_get_int(&config, "edits_revision", &rev) && rev >= 0) out->edits_revision = rev;
    int dep_rev = 0;
    if (api->config_get_int(&config, "deployed_edits_revision", &dep_rev) && dep_rev >= 0)
      out->deployed_edits_revision = dep_rev;
    if (const char* s = api->config_get_cstring(&config, "deployed_base_version"))
      out->deployed_base_version = s;
    if (const char* s = api->config_get_cstring(&config, "deployed_base_sha256"))
      out->deployed_base_sha256 = s;
    if (const char* edits_str = api->config_get_cstring(&config, "edits_json")) {
      const json arr = json::parse(edits_str, nullptr, false);
      if (arr.is_array()) {
        for (const auto& item : arr) {
          UserEdit e;
          if (EditFromJson(item, &e)) out->edits.push_back(std::move(e));
        }
      }
    }
    api->config_close(&config);
    return true;
  }
  // Fallback line parser when RimeApi is null
  std::istringstream in(text);
  std::string line;
  bool found_enabled = false;
  while (std::getline(in, line)) {
    const std::string t = TrimSpaces(line);
    if (t.rfind("enabled:", 0) == 0) {
      const std::string v = TrimSpaces(t.substr(8));
      out->enabled = (v == "true" || v == "True" || v == "1");
      found_enabled = true;
    } else if (t.rfind("version:", 0) == 0) {
      out->version = std::atoi(TrimSpaces(t.substr(8)).c_str());
    } else if (t.rfind("edits_revision:", 0) == 0) {
      out->edits_revision = std::atoi(TrimSpaces(t.substr(15)).c_str());
    } else if (t.rfind("edits_json:", 0) == 0) {
      std::string rest = TrimSpaces(t.substr(11));
      if (rest.size() >= 2 && rest.front() == '\'' && rest.back() == '\'')
        rest = rest.substr(1, rest.size() - 2);
      const json arr = json::parse(rest, nullptr, false);
      if (arr.is_array()) {
        for (const auto& item : arr) {
          UserEdit e;
          if (EditFromJson(item, &e)) out->edits.push_back(std::move(e));
        }
      }
    }
  }
  if (!found_enabled) {
    if (error) *error = "Missing enabled in wisdom_taiwan.user.yaml";
    return false;
  }
  return true;
}

inline bool LoadUserConfig(RimeApi* api, const fs::path& user, UserConfig* out,
                           std::string* error = nullptr) {
  const auto preference = user / "wisdom_taiwan.user.yaml";
  if (!fs::exists(preference)) {
    *out = UserConfig{};
    return true;
  }
  try {
    return ParseUserConfigText(api, Read(preference), out, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

inline std::string EscapeSingleQuotedYaml(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

inline std::string FormatUserConfigYaml(const UserConfig& cfg) {
  if (cfg.version == 0 && cfg.edits.empty() && cfg.edits_revision == 0 &&
      cfg.deployed_base_version.empty()) {
    return std::string("# Bundled Taiwan vocabulary; set false to disable.\nenabled: ") +
           (cfg.enabled ? "true\n" : "false\n");
  }
  json arr = json::array();
  for (const auto& e : cfg.edits) arr.push_back(EditToJson(e));
  std::ostringstream out;
  out << "# Bundled Taiwan vocabulary; set false to disable.\n"
      << "version: " << kUserFormatVersion << "\n"
      << "enabled: " << (cfg.enabled ? "true" : "false") << "\n"
      << "edits_revision: " << cfg.edits_revision << "\n";
  if (!cfg.deployed_base_version.empty())
    out << "deployed_base_version: " << EscapeSingleQuotedYaml(cfg.deployed_base_version) << "\n";
  if (!cfg.deployed_base_sha256.empty())
    out << "deployed_base_sha256: " << EscapeSingleQuotedYaml(cfg.deployed_base_sha256) << "\n";
  out << "deployed_edits_revision: " << cfg.deployed_edits_revision << "\n";
  out << "edits_json: " << EscapeSingleQuotedYaml(arr.dump()) << "\n";
  return out.str();
}

struct BaseEntry {
  std::string word;
  std::string code;
  std::string zhuyin;
  std::string zhuyin_compact;
  int weight = 0;
};

struct BaseLexicon {
  std::string header;
  std::string version;
  std::vector<BaseEntry> entries;
  std::map<std::pair<std::string, std::string>, int> weights;
};

inline bool ParseDictText(const std::string& text, BaseLexicon* out, std::string* error = nullptr) {
  out->header.clear();
  out->version.clear();
  out->entries.clear();
  out->weights.clear();
  const size_t sep = text.find("...\n");
  const size_t sep_crlf = text.find("...\r\n");
  size_t body_start = std::string::npos;
  if (sep != std::string::npos && (sep_crlf == std::string::npos || sep < sep_crlf)) {
    out->header = text.substr(0, sep + 4);
    body_start = sep + 4;
  } else if (sep_crlf != std::string::npos) {
    out->header = text.substr(0, sep_crlf + 5);
    body_start = sep_crlf + 5;
  } else {
    if (error) *error = u8"內建詞庫格式損壞：缺少 YAML 標頭結尾 (...)。";
    return false;
  }

  // Extract version from header
  {
    std::istringstream hs(out->header);
    std::string line;
    while (std::getline(hs, line)) {
      const std::string t = TrimSpaces(line);
      if (t.rfind("version:", 0) == 0) {
        std::string v = TrimSpaces(t.substr(8));
        if (v.size() >= 2 && ((v.front() == '\'' && v.back() == '\'') ||
                              (v.front() == '"' && v.back() == '"'))) {
          v = v.substr(1, v.size() - 2);
        }
        out->version = v;
        break;
      }
    }
  }

  out->entries.reserve(170000);
  size_t pos = body_start;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pos = nl + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t t1 = line.find('\t');
    const size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos) {
      if (error) *error = u8"內建詞庫資料列格式損壞。";
      return false;
    }
    BaseEntry e;
    e.word = line.substr(0, t1);
    e.code = line.substr(t1 + 1, t2 - t1 - 1);
    e.weight = std::atoi(line.substr(t2 + 1).c_str());
    e.zhuyin = CodeToZhuyin(e.code);
    e.zhuyin_compact.reserve(e.zhuyin.size());
    for (char c : e.zhuyin) {
      if (c != ' ') e.zhuyin_compact.push_back(c);
    }
    out->weights[{e.word, e.code}] = e.weight;
    out->entries.push_back(std::move(e));
  }
  return true;
}

inline std::string EffectiveDictVersion(const std::string& base_version,
                                        const std::vector<UserEdit>& edits, int edits_revision) {
  if (edits.empty()) return base_version;
  return base_version + ".u" + std::to_string((std::max)(1, edits_revision));
}

inline std::string BuildEffectiveDictText(const std::string& shared_dict_text,
                                          const std::vector<UserEdit>& edits, int edits_revision) {
  if (edits.empty()) return shared_dict_text;
  BaseLexicon base;
  std::string err;
  if (!ParseDictText(shared_dict_text, &base, &err)) {
    throw std::runtime_error(err);
  }
  std::map<std::pair<std::string, std::string>, int> merged = base.weights;
  for (const auto& edit : edits) {
    const std::pair<std::string, std::string> key{edit.word, edit.code};
    const auto base_it = base.weights.find(key);
    const bool in_base = (base_it != base.weights.end());
    if (edit.hidden) {
      merged.erase(key);
      continue;
    }
    if (!in_base && edit.origin_base) {
      // Upstream removed an entry that the user had modified: do not silently re-add old reading!
      merged.erase(key);
      continue;
    }
    const int base_w = in_base ? base_it->second : edit.base_weight;
    // Deduplicate and apply user preference without summing weights
    merged[key] = EffectiveWeight(in_base, base_w, edit.pref);
  }

  const std::string eff_ver = EffectiveDictVersion(base.version, edits, edits_revision);
  std::string header = base.header;
  const std::string old_ver_line = "version: '" + base.version + "'";
  const size_t vpos = header.find(old_ver_line);
  if (vpos != std::string::npos) {
    header.replace(vpos, old_ver_line.size(), "version: '" + eff_ver + "'");
  }

  std::string out = header;
  out.reserve(shared_dict_text.size() + edits.size() * 32);
  for (const auto& [key, weight] : merged) {
    out += key.first;
    out.push_back('\t');
    out += key.second;
    out.push_back('\t');
    out += std::to_string(weight);
    out.push_back('\n');
  }
  return out;
}

inline bool ValidateEditsList(const std::vector<UserEdit>& edits, const SyllableTable& table,
                              std::vector<UserEdit>* normalized_out, std::string* error) {
  if (edits.size() > kMaxImportEdits) {
    if (error) *error = u8"個人修改筆數超過上限（最多 " + std::to_string(kMaxImportEdits) + u8" 筆）。";
    return false;
  }
  std::set<std::pair<std::string, std::string>> seen;
  std::vector<UserEdit> norm;
  norm.reserve(edits.size());
  for (const auto& raw : edits) {
    UserEdit e = raw;
    std::string zh;
    if (!ParseAndValidateReading(raw.word, raw.code, table, &e.word, &e.code, &zh, error)) {
      return false;
    }
    if (!IsValidPref(e.pref)) {
      if (error) *error = u8"無效的排序偏好設定：「" + e.pref + u8"」。";
      return false;
    }
    if (!seen.insert({e.word, e.code}).second) {
      if (error) *error = u8"重複的個人修改項目：「" + e.word + u8"（" + zh + u8"）」。";
      return false;
    }
    norm.push_back(std::move(e));
  }
  if (normalized_out) *normalized_out = std::move(norm);
  return true;
}

inline std::string Patch(RimeApi* api, std::string original, bool enable, bool child) {
  const std::string bom = original.rfind("\xef\xbb\xbf", 0) == 0 ? "\xef\xbb\xbf" : "";
  if (!bom.empty()) original.erase(0, bom.size());
  const std::string newline = original.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines;
  bool inside = false;
  for (size_t start = 0; start < original.size();) {
    const size_t end = original.find('\n', start);
    const auto line = original.substr(start, end == std::string::npos ? end : end - start + 1);
    auto content = line;
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) content.pop_back();
    if (content == kBegin) {
      if (inside) throw std::runtime_error("Repeated Taiwan vocabulary marker");
      inside = true;
    } else if (content == kEnd) {
      if (!inside) throw std::runtime_error("Unmatched Taiwan vocabulary marker");
      inside = false;
    } else if (!inside) {
      lines.push_back(line);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (inside) throw std::runtime_error("Unclosed Taiwan vocabulary marker");
  std::string cleaned;
  for (const auto& line : lines) cleaned += line;
  const std::regex header("patch:[ \\t]*(#[^\\r\\n]*)?[\\r\\n]*");
  // An empty `patch:` is YAML null (common after removing our only block).
  // Normalize it to a map only for validation; retain the original file bytes.
  auto parse_lines = lines;
  bool has_settings = false;
  for (size_t i = 0; i < parse_lines.size(); ++i) {
    const auto first = parse_lines[i].find_first_not_of(" \t\r\n");
    if (first != std::string::npos && parse_lines[i][first] != '#') has_settings = true;
    if (!std::regex_match(parse_lines[i], header)) continue;
    bool child_value = false;
    for (size_t j = i + 1; j < parse_lines.size(); ++j) {
      const auto next = parse_lines[j].find_first_not_of(" \t\r\n");
      if (next == std::string::npos || parse_lines[j][next] == '#') continue;
      child_value = next > 0;
      break;
    }
    if (!child_value) parse_lines[i] = "patch: {}\n";
  }
  std::string parse_text;
  for (const auto& line : parse_lines) parse_text += line;
  RimeConfig config = {};
  if (!api->config_load_string(&config, has_settings ? parse_text.c_str() : "{}"))
    throw std::runtime_error("Cannot parse schema custom YAML");
  bool replaces_packs = false, has_patch = false;
  try {
    const auto root = Keys(api, &config, "");
    has_patch = Has(root, "patch");
    if (has_patch) {
      // Literal patch keys contain '/': enumerate keys instead of splitting them as paths.
      RimeConfig patch = {};
      if (api->config_get_item(&config, "patch", &patch)) {
        try {
          const auto keys = Keys(api, &patch, "");
          replaces_packs = Has(keys, "translator/packs");
          if (Has(keys, "translator")) replaces_packs |= Has(Keys(api, &patch, "translator"), "packs");
          if (enable && (!child || replaces_packs) && Has(keys, "translator/packs/+"))
            throw std::runtime_error("Custom translator/packs/+ already exists; left unchanged");
        } catch (...) { api->config_close(&patch); throw; }
        api->config_close(&patch);
      }
    }
  } catch (...) { api->config_close(&config); throw; }
  api->config_close(&config);
  if (enable && (!child || replaces_packs)) {
    auto found = std::find_if(lines.begin(), lines.end(), [&](const std::string& line) {
      return std::regex_match(line, header);
    });
    const auto block = std::string(kBegin) + newline + "  translator/packs/+:" + newline +
                       "    - wisdom_taiwan" + newline + kEnd + newline;
    if (found == lines.end()) {
      // Never replace an existing inline patch map or its comments.
      if (has_patch)
        throw std::runtime_error("Inline patch map left unchanged");
      if (!cleaned.empty() && cleaned.back() != '\n') cleaned += newline;
      cleaned += "patch:" + newline + block;
    } else {
      if (found->empty() || found->back() != '\n') *found += newline;
      lines.insert(found + 1, block);
      cleaned.clear();
      for (const auto& line : lines) cleaned += line;
    }
  }
  return bom + cleaned;
}

inline bool Prepare(RimeApi* api, const fs::path& shared, const fs::path& user,
                    std::string* error, bool* changed = nullptr, std::string* stage = nullptr) {
  if (error) error->clear();
  if (changed) *changed = false;
  if (stage) *stage = u8"初始化檢查";
  struct Change { fs::path path; std::string before, after; bool existed; };
  std::vector<Change> changes;
  fs::path temporary;
  try {
    fs::create_directories(user);
    const auto preference = user / "wisdom_taiwan.user.yaml";
    UserConfig user_cfg;
    if (fs::exists(preference)) {
      if (stage) *stage = u8"讀取使用者設定 (wisdom_taiwan.user.yaml)";
      std::string cfg_err;
      if (!ParseUserConfigText(api, Read(preference), &user_cfg, &cfg_err))
        throw std::runtime_error(cfg_err);
    }
    const bool enabled = user_cfg.enabled;
    const auto add = [&](const fs::path& path, const std::string& text) {
      const bool exists = fs::exists(path);
      const auto before = exists ? Read(path) : std::string();
      if (!exists || before != text) changes.push_back({path, before, text, exists});
    };
    if (stage) *stage = u8"驗證內建詞庫資源與產生有效詞庫";
    // Preflight every file before writing anything. Bundled data is installed even
    // when disabled, so the associated attribution always accompanies the data.
    for (const auto name : kFiles) {
      const std::string shared_bytes = Read(shared / name);
      if (std::string(name) == "wisdom_taiwan.dict.yaml" && !user_cfg.edits.empty()) {
        add(user / name,
            BuildEffectiveDictText(shared_bytes, user_cfg.edits, user_cfg.edits_revision));
      } else {
        add(user / name, shared_bytes);
      }
    }
    if (!fs::exists(preference))
      add(preference, "# Bundled Taiwan vocabulary; set false to disable.\nenabled: true\n");
    if (stage) *stage = u8"檢查並更新輸入方案設定 (custom.yaml)";
    for (const auto schema : kApplicableSchemas) {
      const auto path = user / (std::string(schema) + ".custom.yaml");
      const bool exists = fs::exists(path);
      const auto before = exists ? Read(path) : std::string();
      const auto after = Patch(api, before, enabled, std::string(schema) != "bopomofo");
      if (exists || !after.empty()) add(path, after);
    }
    if (changes.empty()) {
      if (stage) stage->clear();
      return true;
    }
    if (stage) *stage = u8"備份現有詞庫與方案檔案";
    const auto id = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto backup = user / ("backup-taiwan-native-" + id);
    fs::create_directory(backup);
    for (const auto& change : changes)
      if (change.existed) Write(backup / change.path.filename(), change.before);
    if (stage) *stage = u8"寫入有效詞庫與方案設定";
    size_t applied = 0;
    try {
      for (const auto& change : changes) {
        temporary = change.path;
        temporary += ".tmp-" + id;
        Write(temporary, change.after);
        fs::rename(temporary, change.path);
        ++applied;
      }
    } catch (...) {
      for (size_t i = 0; i < applied; ++i) {
        if (changes[i].existed) Write(changes[i].path, changes[i].before);
        else fs::remove(changes[i].path);
      }
      throw;
    }
    if (stage) stage->clear();
    if (changed) *changed = true;
    return true;
  } catch (const std::exception& ex) {
    if (!temporary.empty()) { std::error_code ec; fs::remove(temporary, ec); }
    if (error) *error = ex.what();
    return false;
  }
}

inline bool SaveUserConfigWithMigrationBackup(RimeApi* api, const fs::path& user,
                                              const UserConfig& new_cfg, std::string* error) {
  try {
    fs::create_directories(user);
    const fs::path pref = user / "wisdom_taiwan.user.yaml";
    if (fs::exists(pref)) {
      const std::string old_text = Read(pref);
      UserConfig old_cfg;
      if (ParseUserConfigText(api, old_text, &old_cfg) && old_cfg.version == 0 &&
          new_cfg.version >= 1) {
        // Explicit migration backup when upgrading from v0 (enabled-only) to v1
        Write(user / "wisdom_taiwan.user.yaml.v0.bak", old_text);
      }
    }
    const std::string text = FormatUserConfigYaml(new_cfg);
    const auto id =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const fs::path tmp = user / ("wisdom_taiwan.user.yaml.tmp-" + id);
    Write(tmp, text);
    fs::rename(tmp, pref);
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

inline std::string ExportEditsJson(const std::string& base_version,
                                   const std::vector<UserEdit>& edits,
                                   const std::string& exported_at = "") {
  json arr = json::array();
  for (const auto& e : edits) arr.push_back(EditToJson(e));
  json root = {{"format", kExportFormat},
               {"lexicon_id", kLexiconId},
               {"base_version", base_version},
               {"exported_at", exported_at},
               {"edits", arr}};
  return root.dump(2) + "\n";
}

inline bool PreviewImportEdits(const std::string& content, const BaseLexicon& base,
                               const SyllableTable& table,
                               const std::vector<UserEdit>& current_edits, json* preview_out,
                               std::string* error) {
  if (content.size() > kMaxImportBytes) {
    if (error) *error = u8"匯入檔案過大（上限為 2 MB）。";
    return false;
  }
  const json root = json::parse(content, nullptr, false);
  if (!root.is_object()) {
    if (error) *error = u8"匯入檔案不是有效的 JSON 格式。";
    return false;
  }
  const std::string fmt = root.value("format", "");
  if (fmt != kExportFormat) {
    if (error)
      *error = u8"匯入檔案格式不相容（需為 " + std::string(kExportFormat) +
               u8"，不可混用 Rime 使用者詞典快照）。";
    return false;
  }
  const std::string lex_id = root.value("lexicon_id", "");
  if (lex_id != kLexiconId) {
    if (error) *error = u8"匯入檔案的詞庫識別不符：「" + lex_id + u8"」。";
    return false;
  }
  if (!root.contains("edits") || !root["edits"].is_array()) {
    if (error) *error = u8"匯入檔案缺少 edits 清單。";
    return false;
  }
  const json& raw_edits = root["edits"];
  if (raw_edits.size() > kMaxImportEdits) {
    if (error)
      *error = u8"匯入項目超過上限（最多 " + std::to_string(kMaxImportEdits) + u8" 筆）。";
    return false;
  }

  std::map<std::pair<std::string, std::string>, UserEdit> cur_map;
  for (const auto& e : current_edits) cur_map[{e.word, e.code}] = e;

  std::set<std::pair<std::string, std::string>> seen_in_file;
  json added = json::array();
  json changed = json::array();
  json conflicts = json::array();
  json invalid = json::array();
  std::map<std::pair<std::string, std::string>, UserEdit> merged_map = cur_map;

  for (size_t i = 0; i < raw_edits.size(); ++i) {
    const json& item = raw_edits[i];
    if (!item.is_object()) {
      invalid.push_back({{"index", i + 1}, {"word", ""}, {"reading", ""}, {"reason", u8"項目不是物件"}});
      continue;
    }
    const std::string raw_word = item.value("word", "");
    const std::string raw_reading =
        !item.value("code", "").empty() ? item.value("code", "") : item.value("zhuyin", "");
    std::string word, code, zh, item_err;
    if (!ParseAndValidateReading(raw_word, raw_reading, table, &word, &code, &zh, &item_err)) {
      invalid.push_back(
          {{"index", i + 1}, {"word", raw_word}, {"reading", raw_reading}, {"reason", item_err}});
      continue;
    }
    const std::string pref = item.value("pref", "normal");
    if (!IsValidPref(pref)) {
      invalid.push_back({{"index", i + 1},
                         {"word", word},
                         {"reading", zh},
                         {"reason", u8"無效的排序偏好：" + pref}});
      continue;
    }
    if (!seen_in_file.insert({word, code}).second) {
      invalid.push_back({{"index", i + 1},
                         {"word", word},
                         {"reading", zh},
                         {"reason", u8"匯入檔案內重複的詞與讀音"}});
      continue;
    }

    const auto base_it = base.weights.find({word, code});
    const bool in_base = (base_it != base.weights.end());
    UserEdit e;
    e.word = word;
    e.code = code;
    e.hidden = item.value("hidden", false);
    e.pref = pref;
    e.origin_base = item.value("origin_base", in_base);
    e.base_weight = in_base ? base_it->second : item.value("base_weight", 1000);
    e.replaced_from_word = item.value("replaced_from_word", "");
    e.replaced_from_code = item.value("replaced_from_code", "");
    if (HasControlOrTab(e.replaced_from_word) || HasControlOrTab(e.replaced_from_code)) {
      e.replaced_from_word.clear();
      e.replaced_from_code.clear();
    }

    json entry_json = EditToJson(e);
    if (!in_base && e.origin_base) {
      entry_json["reason"] = u8"原始項目已在上游移除，請檢查";
      conflicts.push_back(entry_json);
    } else {
      const auto cur_it = cur_map.find({word, code});
      if (cur_it == cur_map.end()) {
        added.push_back(entry_json);
      } else if (cur_it->second != e) {
        entry_json["reason"] = u8"覆蓋現有個人修改";
        changed.push_back(entry_json);
        conflicts.push_back(entry_json);
      }
    }
    merged_map[{word, code}] = std::move(e);
  }

  json merged_arr = json::array();
  for (const auto& [k, e] : merged_map) merged_arr.push_back(EditToJson(e));

  *preview_out = {{"format", fmt},
                  {"lexicon_id", lex_id},
                  {"source_base_version", root.value("base_version", "")},
                  {"total", raw_edits.size()},
                  {"added", added},
                  {"changed", changed},
                  {"conflicts", conflicts},
                  {"invalid", invalid},
                  {"merged_edits", merged_arr}};
  return true;
}

}  // namespace settings::taiwan_lexicon
