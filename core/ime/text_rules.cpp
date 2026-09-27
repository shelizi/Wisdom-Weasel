#include "text_rules.h"

#include <algorithm>
#include <cwchar>

namespace ime {

bool IsSeparatorOrPunctuation(wchar_t ch) {
  if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r')
    return true;
  // 常見中文標點
  if (ch == L'，' || ch == L'。' || ch == L'、' || ch == L'；' ||
      ch == L'：' || ch == L'？' || ch == L'！' || ch == L'…' ||
      ch == L'—' || ch == L'–' || ch == L'（' || ch == L'）' ||
      ch == L'【' || ch == L'】' || ch == L'《' || ch == L'》' ||
      ch == L'“' || ch == L'”' || ch == L'‘' || ch == L'’')
    return true;
  // 常見英文／通用標點
  if (ch == L',' || ch == L'.' || ch == L';' || ch == L':' ||
      ch == L'?' || ch == L'!' || ch == L'-' || ch == L'_' ||
      ch == L'(' || ch == L')' || ch == L'[' || ch == L']' ||
      ch == L'{' || ch == L'}' || ch == L'"' || ch == L'\'' ||
      ch == L'/' || ch == L'\\' || ch == L'*' || ch == L'#' ||
      ch == L'@' || ch == L'$' || ch == L'%' || ch == L'^' ||
      ch == L'&' || ch == L'+' || ch == L'=' || ch == L'~' ||
      ch == L'`' || ch == L'|' || ch == L'<' || ch == L'>')
    return true;
  return false;
}

bool HasMeaningfulContent(const std::wstring& text) {
  for (wchar_t ch : text) {
    if (!IsSeparatorOrPunctuation(ch))
      return true;
  }
  return false;
}

namespace {

bool IsControl(wchar_t c) {
  return c < 0x20 || c == 0x7f;
}

std::wstring FirstLine(const std::wstring& raw) {
  std::wstring s = raw.substr(0, raw.find_first_of(L"\r\n"));
  s.erase(std::remove_if(s.begin(), s.end(), IsControl), s.end());
  return s;
}

std::wstring Trim(const std::wstring& s, const wchar_t* chars) {
  const size_t b = s.find_first_not_of(chars);
  return b == std::wstring::npos ? std::wstring() : s.substr(b, s.find_last_not_of(chars) - b + 1);
}

}  // namespace

std::vector<std::wstring> CleanCandidates(const std::vector<std::wstring>& raw,
                                          const std::wstring& prefix) {
  static const wchar_t kTrimChars[] =
      L" \t　\"'`“”‘’「」『』"
      L"()[]{}<>（）【】《》"
      L",.;:!?，。、；：！？…";
  std::vector<std::wstring> cleaned;
  for (const auto& r : raw) {
    std::wstring s = Trim(FirstLine(r), kTrimChars);
    if (!s.empty())
      s = prefix + s;
    if (!s.empty() && std::find(cleaned.begin(), cleaned.end(), s) == cleaned.end())
      cleaned.push_back(std::move(s));
  }
  return cleaned;
}

std::wstring CleanCorrection(const std::wstring& raw, const std::wstring& draft) {
  static const wchar_t kTrimChars[] = L" \t　\"'`“”‘’「」『』。.";
  std::wstring s = FirstLine(raw);
  // 模型偶爾會把「校正：」一起輸出
  for (const wchar_t* label : {L"校正：", L"校正:"}) {
    const size_t pos = s.find(label);
    if (pos != std::wstring::npos)
      s = s.substr(pos + std::wcslen(label));
  }
  s = Trim(s, kTrimChars);
  if (s.empty() || s == draft)
    return L"";
  if (s.size() > draft.size() + 2 || s.size() + 2 < draft.size())
    return L"";
  return s;
}

std::vector<std::wstring> MergeCandidates(std::vector<std::wstring> merged,
                                          const std::vector<std::wstring>& more,
                                          size_t max) {
  for (const auto& c : more) {
    if (merged.size() >= max)
      break;
    if (std::find(merged.begin(), merged.end(), c) == merged.end())
      merged.push_back(c);
  }
  return merged;
}

}  // namespace ime
