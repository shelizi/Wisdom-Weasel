#include "choice_log.h"

#include <PersonalCrypto.h>

#include <fstream>
#include <sstream>

#include "../base/utf8.h"

namespace ime {

const char* ChoiceMethod(bool mixed, bool correction, bool llm, bool focus, bool changed) {
  return mixed        ? "mixed"
         : correction ? "correction"
         : llm        ? "llm"
         : focus && changed ? "focus"
         : changed    ? "changed"
                      : "direct";
}

std::wstring ChoiceContext(std::wstring recent, const std::wstring& text, size_t max) {
  if (recent.size() >= text.size() &&
      recent.compare(recent.size() - text.size(), text.size(), text) == 0)
    recent.resize(recent.size() - text.size());
  if (recent.size() > max)
    recent = recent.substr(recent.size() - max);
  return recent;
}

std::string FormatChoiceRecord(const ChoiceRecord& r) {
  auto clean = [](std::wstring s) {
    for (auto& c : s)
      if (c == L'\t' || c == L'\r' || c == L'\n')
        c = L' ';
    return utf8::FromWide(s);
  };
  std::string app = r.app;
  for (auto& c : app)
    if (c == '\t' || c == '\r' || c == '\n')
      c = ' ';
  std::ostringstream record;
  record << r.time << '\t' << app << '\t' << r.method << '\t' << clean(r.context) << '\t'
         << clean(r.zhuyin) << '\t' << clean(r.default_text) << '\t' << clean(r.text);
  // 整句候選（U+001E 分隔）
  std::wstring shown;
  for (const auto& s : r.shown) {
    std::wstring t = s;
    for (auto& c : t)
      if (c == kChoiceListSeparator)
        c = L' ';
    shown += (shown.empty() ? L"" : std::wstring(1, kChoiceListSeparator)) + t;
  }
  record << '\t' << clean(shown) << '\t';
  // 模型分數／個人詞庫分數（逗號分隔；後面的欄位一律補上，讀端依欄位數辨識新舊格式）
  record.precision(4);
  for (size_t i = 0; i < r.shown_scores.size(); ++i)
    record << (i ? "," : "") << std::fixed << r.shown_scores[i];
  record << '\t';
  for (size_t i = 0; i < r.shown_personal.size(); ++i)
    record << (i ? "," : "") << std::fixed << r.shown_personal[i];
  record << '\t' << r.rec << '\t' << r.took << '\t' << clean(r.context_key);
  return record.str();
}

bool AppendChoiceRecord(const std::filesystem::path& personal_dir, const ChoiceRecord& record) {
  std::string cipher;
  if (!personal_crypto::Protect(FormatChoiceRecord(record), &cipher))
    return false;
  std::error_code ec;
  std::filesystem::create_directories(personal_dir, ec);
  std::ofstream out(personal_dir / "choice_log.dat", std::ios::binary | std::ios::app);
  const uint32_t len = (uint32_t)cipher.size();
  out.write((const char*)&len, sizeof(len));
  out.write(cipher.data(), (std::streamsize)cipher.size());
  return (bool)out;
}

}  // namespace ime
