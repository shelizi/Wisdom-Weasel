#pragma once

// UTF-8 與 std::wstring 互轉（Windows 的 wchar_t 是 UTF-16，mac 是 UTF-32）。
// 不合法的位元組換成 U+FFFD，和 Windows 的 MultiByteToWideChar 一樣
#include <string>

namespace utf8 {

inline std::wstring ToWide(const std::string& s) {
  std::wstring out;
  out.reserve(s.size());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
      out.push_back((wchar_t)c);
      ++i;
      continue;
    }
    // 依開頭位元組決定長度與第二個位元組的合法範圍（排除過長編碼、代理區與超出範圍）
    size_t len = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    char32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0F;
      if (c == 0xE0)
        lo = 0xA0;
      else if (c == 0xED)
        hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
      if (c == 0xF0)
        lo = 0x90;
      else if (c == 0xF4)
        hi = 0x8F;
    }
    // 不合法時，開頭加上已對上的後續位元組算一個 U+FFFD，從下一個位元組重新開始
    size_t k = 1;
    if (len) {
      for (; k < len && i + k < n; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if (cc < lo || cc > hi) {
          // 第二個位元組是後續位元組但範圍不對（過長、代理區、超出範圍）：兩個一起算
          if (k == 1 && cc >= 0x80 && cc <= 0xBF)
            ++k;
          break;
        }
        cp = (cp << 6) | (cc & 0x3F);
        lo = 0x80;
        hi = 0xBF;
      }
    }
    if (!len || k < len) {
      out.push_back(L'\xFFFD');
      i += k;
      continue;
    }
    i += len;
    if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back((wchar_t)(0xD800 + (cp >> 10)));
      out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back((wchar_t)cp);
    }
  }
  return out;
}

inline std::string FromWide(const std::wstring& w) {
  std::string out;
  out.reserve(w.size() * 3);
  const size_t n = w.size();
  for (size_t i = 0; i < n; ++i) {
    char32_t cp = (char32_t)w[i];
    if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && w[i + 1] >= 0xDC00 &&
        w[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + ((char32_t)w[i + 1] - 0xDC00);
      ++i;
    } else if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      cp = 0xFFFD;  // 落單的代理字元
    }
    if (cp < 0x80) {
      out.push_back((char)cp);
    } else if (cp < 0x800) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (cp >> 18)));
      out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

}  // namespace utf8
