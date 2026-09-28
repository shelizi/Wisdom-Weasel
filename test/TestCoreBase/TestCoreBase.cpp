// core/base 的測試：UTF-8 轉換要和 Windows API
// 結果一致；std::filesystem::rename 會取代舊檔
#include "../../core/base/clock.h"
#include "../../core/base/devlog.h"
#include "../../core/base/utf8.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static std::wstring WinToWide(const std::string& s) {
  if (s.empty())
    return std::wstring();
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

static std::string WinFromWide(const std::wstring& w) {
  if (w.empty())
    return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr,
                      nullptr);
  return s;
}

struct Collect : DevLog {
  std::string text;
  bool IsEnabled() const override { return true; }
  void Write(const std::string& s) override { text += s; }
};

int main() {
  // 合法字串：來回轉換不變，和 Windows API 一致
  const std::vector<std::string> valid = {"",
                                          "abc",
                                          "注音輸入法",
                                          "\xF0\x9F\x98\x80 emoji",
                                          "\xE4\xB8\x80\xF0\xA0\x80\x80",
                                          "\x7F\xC2\x80\xDF\xBF",
                                          "\xEF\xBF\xBF\xF4\x8F\xBF\xBF"};
  for (const std::string& s : valid) {
    CHECK(utf8::ToWide(s) == WinToWide(s));
    CHECK(utf8::FromWide(utf8::ToWide(s)) == s);
  }
  // 不合法的位元組：換成 U+FFFD，和 MultiByteToWideChar 一樣
  const std::vector<std::string> invalid = {
      "\x80",         "a\xC3",        "\xC0\xAF",
      "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80",
      "\xFF\xFEx",    "\xE4\xB8",     "\xE4\xB8x"};
  for (const std::string& s : invalid) {
    const std::wstring mine = utf8::ToWide(s), win = WinToWide(s);
    if (mine != win) {
      std::printf("  mismatch for");
      for (unsigned char c : s)
        std::printf(" %02X", c);
      std::printf(" ->");
      for (wchar_t c : mine)
        std::printf(" %04X", (unsigned)c);
      std::printf(" / windows");
      for (wchar_t c : win)
        std::printf(" %04X", (unsigned)c);
      std::printf("\n");
    }
    CHECK(mine == win);
  }
  // 隨機位元組
  std::mt19937 rng(1);
  int mismatches = 0;
  for (int i = 0; i < 20000; ++i) {
    std::string s(rng() % 12, '\0');
    for (char& c : s)
      c = (char)(rng() % 4 == 0 ? rng() % 0x80 : 0x80 + rng() % 0x80);
    if (utf8::ToWide(s) != WinToWide(s))
      ++mismatches;
  }
  std::printf("  random byte strings that differ from Windows: %d / 20000\n",
              mismatches);
  CHECK(mismatches == 0);
  // 落單的代理字元
  CHECK(utf8::FromWide(std::wstring(1, (wchar_t)0xD800)) ==
        WinFromWide(std::wstring(1, (wchar_t)0xD800)));
  CHECK(utf8::FromWide(L"a\xDC00"
                       L"b") == WinFromWide(L"a\xDC00"
                                            L"b"));

  // 日誌介面：WriteLine 加上換行，寬字串轉成 UTF-8
  Collect log;
  log.WriteLine(L"注音");
  log.Write("x");
  CHECK(log.text == "\xE6\xB3\xA8\xE9\x9F\xB3\r\nx");

  // 時間
  const uint64_t t0 = base::MonotonicMs();
  Sleep(20);
  CHECK(base::MonotonicMs() - t0 >= 15);
  const std::tm now = base::LocalTime();
  CHECK(now.tm_year + 1900 >= 2024);

  // rename 會取代已存在的檔案（個人詞表寫入暫存檔後取代）
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / L"TestCoreBase-注音";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  std::ofstream(dir / L"a.txt") << "old";
  std::ofstream(dir / L"a.txt.tmp") << "new";
  fs::rename(dir / L"a.txt.tmp", dir / L"a.txt", ec);
  CHECK(!ec);
  std::string content;
  std::getline(std::ifstream(dir / L"a.txt"), content);
  CHECK(content == "new");
  CHECK(!fs::exists(dir / L"a.txt.tmp"));
  // 檔案時間換成 Unix 時間，和現在差不多
  const auto since =
      fs::last_write_time(dir / L"a.txt") - fs::file_time_type::clock::now();
  const long long unix_time =
      std::chrono::duration_cast<std::chrono::seconds>(
          (std::chrono::system_clock::now() + since).time_since_epoch())
          .count();
  CHECK(std::llabs(unix_time - (long long)std::time(nullptr)) < 60);
  fs::remove_all(dir, ec);

  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
