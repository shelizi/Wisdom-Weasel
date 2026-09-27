// 個人資料加密的測試。以 run.bat 編譯執行；金鑰檔放在暫存資料夾，不動到真正的金鑰。
#include <PersonalCrypto.h>

#include "../../core/platform/key_store.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace personal_crypto;

static int failures = 0;
#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                             \
    }                                                         \
  } while (0)

static std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// 舊版格式：直接以 DPAPI 加密
static std::string LegacyProtect(const std::string& in) {
  const char* entropy = "Wisdom-Weasel personal lexicon v1";
  DATA_BLOB input{(DWORD)in.size(), (BYTE*)in.data()};
  DATA_BLOB ent{(DWORD)strlen(entropy), (BYTE*)entropy};
  DATA_BLOB output{};
  CryptProtectData(&input, L"Weasel personal lexicon", &ent, nullptr, nullptr,
                   CRYPTPROTECT_UI_FORBIDDEN, &output);
  std::string out((const char*)output.pbData, output.cbData);
  LocalFree(output.pbData);
  return out;
}

int main() {
  const fs::path key_file = PERSONAL_CRYPTO_TEST_KEY_FILE;
  const fs::path dir = key_file.parent_path();
  std::error_code ec;
  fs::remove_all(dir, ec);

  // 多個執行緒同時第一次建立金鑰：大家拿到同一把
  {
    std::vector<std::vector<uint8_t>> keys(8, std::vector<uint8_t>(platform::kKeySize));
    std::vector<std::thread> threads;
    std::vector<int> ok(keys.size());
    for (size_t i = 0; i < keys.size(); ++i)
      threads.emplace_back([&, i] { ok[i] = platform::LoadOrCreateKey(keys[i].data()); });
    for (auto& t : threads)
      t.join();
    for (size_t i = 0; i < keys.size(); ++i) {
      CHECK(ok[i]);
      CHECK(keys[i] == keys[0]);
    }
    CHECK(fs::exists(key_file));
    size_t leftovers = 0;
    for (auto& e : fs::directory_iterator(dir))
      leftovers += e.path() != key_file;
    CHECK(leftovers == 0);  // 暫存檔都清掉了
  }

  // 來回加解密；空字串與含 NUL 的資料
  for (const std::string plain : {std::string(), std::string("WWPL1\n詞\t3\n"),
                                  std::string("a\0b", 3), std::string(100000, 'x')}) {
    std::string cipher, back;
    CHECK(Protect(plain, &cipher));
    CHECK(cipher.size() == plain.size() + 44);
    CHECK(cipher.compare(0, 4, "WWE1") == 0);
    CHECK(Unprotect(cipher, &back));
    CHECK(back == plain);
  }

  // 同樣的內容每次密文都不同（隨機 nonce）
  {
    std::string a, b;
    Protect("same", &a);
    Protect("same", &b);
    CHECK(a != b);
  }

  // 竄改任何一個位元組都解不開
  {
    std::string cipher, back;
    Protect("hello personal lexicon", &cipher);
    for (size_t i = 0; i < cipher.size(); ++i) {
      std::string bad = cipher;
      bad[i] ^= 1;
      CHECK(!Unprotect(bad, &back));
    }
    CHECK(!Unprotect(cipher.substr(0, 20), &back));
    CHECK(!Unprotect(std::string("WWE1"), &back));
  }

  // 舊版 DPAPI 資料仍可讀
  {
    std::string back;
    CHECK(Unprotect(LegacyProtect("舊版詞庫"), &back));
    CHECK(back == "舊版詞庫");
    CHECK(!Unprotect(std::string("garbage"), &back));
  }

  // 寫檔、讀檔
  {
    const fs::path f = dir / L"lexicon.dat";
    CHECK(WriteProtected(f, "one"));
    CHECK(WriteProtected(f, "two"));  // 取代既有的檔案
    std::string back;
    CHECK(ReadProtected(f, &back));
    CHECK(back == "two");
    CHECK(!fs::exists(dir / L"lexicon.dat.tmp"));
    CHECK(!ReadProtected(dir / L"missing.dat", &back));
  }

  // 金鑰檔損毀：回傳失敗，而且不會被新金鑰覆蓋
  {
    std::ofstream(key_file, std::ios::binary | std::ios::trunc) << "WWK1broken";
    uint8_t key[platform::kKeySize];
    CHECK(!platform::LoadOrCreateKey(key));
    CHECK(ReadAll(key_file) == "WWK1broken");
  }

  fs::remove_all(dir, ec);
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
