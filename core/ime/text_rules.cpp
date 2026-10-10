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

std::wstring PreferTaiwanForms(std::wstring text) {
  struct Pair {
    wchar_t from;
    wchar_t to;
  };
  // 異體。目標字不再出現在來源裡，所以下面再掃一次也不會改回去。
  static constexpr Pair kGlyph[] = {
      {L'\u7232', L'\u70BA'},  // 爲 → 為
      {L'\u88CF', L'\u88E1'},  // 裏 → 裡
      {L'\u55AB', L'\u5403'},  // 喫 → 吃
      {L'\u8988', L'\u6838'},  // 覈 → 核
      {L'\u4F48', L'\u5E03'},  // 佈 → 布
      {L'\u7740', L'\u8457'},  // 着 → 著
      {L'\u9EAA', L'\u9EB5'},  // 麪 → 麵
      {L'\u81FA', L'\u53F0'},  // 臺 → 台
      {L'\u3007', L'\u96F6'},  // 〇 → 零
  };
  for (wchar_t& ch : text) {
    for (const Pair& pair : kGlyph) {
      if (ch == pair.from) {
        ch = pair.to;
        break;
      }
    }
  }
  auto pronoun = [](wchar_t ch) {
    switch (ch) {
      case L'\u6211':  // 我
      case L'\u4F60':  // 你
      case L'\u4ED6':  // 他
      case L'\u5979':  // 她
      case L'\u5B83':  // 它
      case L'\u7260':  // 牠
        return true;
      default:
        return false;
    }
  };
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i + 1] == L'\u9580' && pronoun(text[i]))  // 門 → 們
      text[i + 1] = L'\u5011';
  }
  // 至少三個字，而且倒數第二個字不是「媽」，才把句尾的「媽」換成「嗎」
  if (text.size() >= 3 && text.back() == L'\u5ABD' && text[text.size() - 2] != L'\u5ABD')
    text.back() = L'\u55CE';
  return text;
}

}  // namespace ime
