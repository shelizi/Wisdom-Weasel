#pragma once
// Builds the preedit shown for style/preedit_type: preview, in the manner of
// 新注音: completed syllables show as converted text, the syllable still being
// typed stays as zhuyin, and while choosing a word backwards the text after
// the caret stays visible with the chosen word marked.
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <WeaselUtility.h>

struct ZhuyinSpeller {
  std::string finals = " 6347";  // keys that end a syllable (tones)
  char delimiter = '\'';
  std::map<wchar_t, wchar_t> xlit;  // raw key -> zhuyin, from preedit_format
  std::string alphabet;             // keys the speller takes (speller/alphabet)
};

struct ZhuyinPreview {
  std::wstring text;
  int sel_start = 0;  // marked (chosen) range, empty if none
  int sel_end = 0;
  int cursor = 0;
};

namespace zhuyin_preview {

inline bool IsBopomofo(wchar_t c) {
  return (c >= 0x3105 && c <= 0x312F) || (c >= 0x31A0 && c <= 0x31BF);
}

inline bool IsTone(wchar_t c) {
  return c == 0x02C9 || c == 0x02CA || c == 0x02C7 || c == 0x02CB ||
         c == 0x02D9;
}

inline bool IsBoundary(const ZhuyinSpeller& sp, char c) {
  return c == sp.delimiter || sp.finals.find(c) != std::string::npos;
}

inline size_t CountSyllables(const ZhuyinSpeller& sp, const std::string& raw) {
  size_t n = 0;
  bool open = false;
  for (char c : raw) {
    if (sp.finals.find(c) != std::string::npos) {
      ++n;
      open = false;
    } else if (c == sp.delimiter) {
      if (open)
        ++n;
      open = false;
    } else {
      open = true;
    }
  }
  return open ? n + 1 : n;
}

// raw keys split into syllables, each ending with its tone key if any
inline std::vector<std::string> SplitSyllables(const ZhuyinSpeller& sp,
                                               const std::string& raw) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : raw) {
    if (sp.finals.find(c) != std::string::npos) {
      out.push_back(cur + c);
      cur.clear();
    } else if (c == sp.delimiter) {
      if (!cur.empty())
        out.push_back(cur + c);
      else if (!out.empty())
        out.back() += c;
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

inline std::wstring Format(const ZhuyinSpeller& sp, const std::string& raw) {
  std::wstring out;
  for (char c : raw) {
    auto it = sp.xlit.find((wchar_t)(unsigned char)c);
    wchar_t w = it != sp.xlit.end() ? it->second : (wchar_t)(unsigned char)c;
    if (w != L' ')
      out += w;
  }
  return out;
}

// trailing zhuyin syllables of the preedit that have no tone mark yet
inline std::vector<std::wstring> PendingSyllables(const std::wstring& pre) {
  std::vector<std::wstring> pending;
  size_t end = pre.size();
  while (end > 0) {
    while (end > 0 && pre[end - 1] == L' ')
      --end;
    size_t start = end;
    while (start > 0 && IsBopomofo(pre[start - 1]))
      --start;
    if (start == end || (start > 0 && IsTone(pre[start - 1])))
      break;  // toned syllable or non-zhuyin text
    // 只有正在拼的最後一個音節留成注音；前面省略聲調的音節照樣轉成字（快打常整句不打聲調）
    pending.push_back(pre.substr(start, end - start));
    break;
  }
  return pending;
}

inline std::vector<std::wstring> SplitChars(const std::wstring& s) {
  std::vector<std::wstring> units;
  for (size_t i = 0; i < s.size(); ++i) {
    if (IS_HIGH_SURROGATE(s[i]) && i + 1 < s.size() &&
        IS_LOW_SURROGATE(s[i + 1])) {
      units.push_back(s.substr(i, 2));
      ++i;
    } else {
      units.push_back(s.substr(i, 1));
    }
  }
  return units;
}

inline std::wstring Join(const std::vector<std::wstring>& units,
                         size_t from = 0,
                         size_t to = std::wstring::npos) {
  std::wstring out;
  for (size_t i = from; i < units.size() && i < to; ++i)
    out += units[i];
  return out;
}

// 每個音節（字）對應幾個按鍵，依 Rime 組字的切法：注音之間以空白分開，省略聲調的連打也切得對，
// 每個注音符號或聲調對應一個按鍵。前面已確定成中文的部分沿用上一次的切法。對不上時回傳空的
inline std::vector<size_t> SyllableLengths(const ZhuyinSpeller& sp,
                                           const std::wstring& preedit,
                                           const std::string& input,
                                           const std::string& old_input,
                                           const std::vector<size_t>& old_lens) {
  std::vector<size_t> lens;
  if (input.find(sp.delimiter) != std::string::npos) {
    // 手動分隔的輸入：依聲調與分隔符切
    for (const auto& s : SplitSyllables(sp, input))
      lens.push_back(s.size());
    return lens;
  }
  size_t last_other = std::wstring::npos;  // 最後一個不是注音的字（已確定的中文）
  for (size_t i = 0; i < preedit.size(); ++i) {
    const wchar_t c = preedit[i];
    if (!IsBopomofo(c) && !IsTone(c) && c != L' ')
      last_other = i;
  }
  std::vector<size_t> tokens;
  size_t current = 0;
  for (size_t i = last_other == std::wstring::npos ? 0 : last_other + 1; i < preedit.size(); ++i) {
    if (preedit[i] == L' ') {
      if (current)
        tokens.push_back(current);
      current = 0;
    } else {
      ++current;
    }
  }
  if (current)
    tokens.push_back(current);
  size_t zhuyin_keys = 0;
  for (size_t t : tokens)
    zhuyin_keys += t;
  if (zhuyin_keys > input.size())
    return {};
  const size_t prefix_keys = input.size() - zhuyin_keys;
  if (prefix_keys > 0) {
    if (old_input.size() < prefix_keys || old_input.compare(0, prefix_keys, input, 0, prefix_keys))
      return {};
    size_t sum = 0;
    for (size_t l : old_lens) {
      if (sum >= prefix_keys)
        break;
      sum += l;
      lens.push_back(l);
    }
    if (sum != prefix_keys)
      return {};
  } else if (last_other != std::wstring::npos) {
    return {};
  }
  lens.insert(lens.end(), tokens.begin(), tokens.end());
  return lens;
}

}  // namespace zhuyin_preview

// preview: RIME's commit_text_preview, which covers the input up to the caret;
// input/caret: the whole raw input and caret position;
// cand: text of the highlighted candidate; cache_*: kept per session.
inline ZhuyinPreview BuildZhuyinPreview(const std::string& preview,
                                        const std::string& preedit,
                                        const std::string& input,
                                        size_t caret,
                                        const std::string& cand,
                                        int highlighted,
                                        const ZhuyinSpeller& sp,
                                        std::string& cache_input,
                                        std::vector<std::wstring>& cache_units,
                                        std::vector<size_t>& cache_lens,
                                        bool focused = false) {
  using namespace zhuyin_preview;
  ZhuyinPreview out;
  // 組字顯示在候選窗時 Weasel 開啟 soft_cursor，preedit 裡會插入游標符號 ‸，解析前先拿掉
  std::wstring pre = u8tow(preedit);
  pre.erase(std::remove(pre.begin(), pre.end(), L'‸'), pre.end());
  std::wstring head = u8tow(preview);
  // keys of the active input not covered by the highlighted candidate
  std::string rem;
  while (!head.empty() && head.back() < 0x80) {
    rem.insert(rem.begin(), (char)head.back());
    head.pop_back();
  }
  caret = (std::min)(caret, input.size());
  // with the caret at the very start RIME converts the whole input
  const bool at_start = caret == 0;
  if (at_start)
    caret = input.size();
  if (rem.size() > caret || input.compare(caret - rem.size(), rem.size(), rem))
    rem.clear();

  if (caret == input.size() && rem.empty()) {
    // typing at the end: convert all but the syllables still being typed
    auto units = SplitChars(head);
    auto pending = PendingSyllables(pre);
    if (pending.size() <= units.size()) {
      units.resize(units.size() - pending.size());
      units.insert(units.end(), pending.begin(), pending.end());
    }
    auto lens = SyllableLengths(sp, pre, input, cache_input, cache_lens);
    if (!lens.empty() && lens.size() == units.size()) {
      cache_input = input;
      cache_units = units;
      cache_lens = std::move(lens);
    } else {
      cache_input.clear();
      cache_units.clear();
      cache_lens.clear();
    }
    out.text = Join(units);
    out.sel_start = out.sel_end = out.cursor =
        at_start ? 0 : (int)out.text.size();
    // 框住的是最後一個字（游標仍在句尾）：一樣標示出來
    const std::wstring cw = u8tow(cand);
    if (focused && !cw.empty() && out.text.size() >= cw.size() &&
        out.text.compare(out.text.size() - cw.size(), cw.size(), cw) == 0)
      out.sel_start = (int)(out.text.size() - cw.size());
    return out;
  }

  // choosing backwards: keep the converted text after the chosen word,
  // taken from the last full conversion where the keys still match
  const size_t tail_start = caret - rem.size();
  const std::wstring cw = u8tow(cand);
  const bool choosing = caret < input.size() || highlighted > 0 || focused;
  auto finish = [&](const std::wstring& rem_text, const std::wstring& after_text) {
    out.text = head + rem_text + after_text;
    out.cursor = (int)(head.size() + rem_text.size());
    out.sel_start = out.sel_end = out.cursor;
    if (choosing && !cw.empty() && head.size() >= cw.size() &&
        head.compare(head.size() - cw.size(), cw.size(), cw) == 0) {
      out.sel_start = (int)(head.size() - cw.size());
      out.sel_end = (int)head.size();
    }
    return out;
  };
  // 輸入和最後一次整句轉換相同：照記下的切法取後面的字
  if (cache_input == input && !cache_lens.empty() && cache_lens.size() == cache_units.size()) {
    size_t sum = 0, k = std::string::npos, m = std::string::npos;
    for (size_t i = 0; i <= cache_lens.size(); ++i) {
      if (sum == tail_start && k == std::string::npos)
        k = i;
      if (sum == caret) {
        m = i;
        break;
      }
      if (i < cache_lens.size())
        sum += cache_lens[i];
    }
    if (k != std::string::npos && m != std::string::npos && k <= m)
      return finish(Join(cache_units, k, m), Join(cache_units, m));
  }
  const auto syllables = SplitSyllables(sp, input.substr(tail_start));
  std::vector<std::wstring> tail_units;
  size_t matched = syllables.size();
  if (tail_start == 0 || IsBoundary(sp, input[tail_start - 1])) {
    std::string suffix;
    for (size_t j = syllables.size(); j-- > 0;) {
      suffix = syllables[j] + suffix;
      const size_t n = syllables.size() - j;
      if (n > cache_units.size() || cache_input.size() < suffix.size())
        break;
      const size_t off = cache_input.size() - suffix.size();
      if (cache_input.compare(off, suffix.size(), suffix) != 0)
        break;
      if (off == 0 || IsBoundary(sp, cache_input[off - 1]))
        matched = j;
    }
  }
  for (size_t j = 0; j < matched; ++j)
    tail_units.push_back(Format(sp, syllables[j]));
  const size_t n_cached = syllables.size() - matched;
  tail_units.insert(tail_units.end(), cache_units.end() - n_cached,
                    cache_units.end());
  const size_t n_rem = CountSyllables(sp, rem);
  return finish(Join(tail_units, 0, n_rem), Join(tail_units, n_rem));
}
