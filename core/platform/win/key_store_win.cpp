// Windows：主金鑰以 DPAPI 包起來存在 %LOCALAPPDATA%\Wisdom-Weasel\personal.key，
// 只有目前的 Windows 帳號能解開（與 macOS 的 Keychain 對應，不跟著 Rime 使用者資料夾同步）。
#include "../key_store.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace personal_crypto {
namespace platform {

namespace {

namespace fs = std::filesystem;

constexpr char kKeyMagic[4] = {'W', 'W', 'K', '1'};

// 額外的 DPAPI entropy：避免其他程式把同帳號下別的 DPAPI 資料誤當成詞庫。
// 舊版資料也是用這組 entropy 直接加密的。
const char* Entropy() {
  return "Wisdom-Weasel personal lexicon v1";
}

bool DpapiProtect(const std::string& in, std::string* out) {
  DATA_BLOB input{(DWORD)in.size(), (BYTE*)in.data()};
  DATA_BLOB entropy{(DWORD)strlen(Entropy()), (BYTE*)Entropy()};
  DATA_BLOB output{};
  if (!CryptProtectData(&input, L"Wisdom-Weasel personal key", &entropy, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output))
    return false;
  out->assign((const char*)output.pbData, output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return true;
}

bool DpapiUnprotect(const std::string& in, std::string* out) {
  DATA_BLOB input{(DWORD)in.size(), (BYTE*)in.data()};
  DATA_BLOB entropy{(DWORD)strlen(Entropy()), (BYTE*)Entropy()};
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
    return false;
  out->assign((const char*)output.pbData, output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return true;
}

fs::path KeyFilePath() {
#ifdef PERSONAL_CRYPTO_TEST_KEY_FILE
  return fs::path(PERSONAL_CRYPTO_TEST_KEY_FILE);
#else
  PWSTR dir = nullptr;
  fs::path path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir)))
    path = fs::path(dir) / L"Wisdom-Weasel" / L"personal.key";
  CoTaskMemFree(dir);
  return path;
#endif
}

// kBusy：檔案在但暫時打不開（例如另一個執行緒正在改名），稍後再試
enum class ReadResult { kOk, kMissing, kBusy, kFailed };

ReadResult ReadKeyFile(const fs::path& path, uint8_t key[kKeySize]) {
  std::string data;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return fs::exists(path) ? ReadResult::kBusy : ReadResult::kMissing;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string plain;
  if (data.size() <= sizeof(kKeyMagic) ||
      std::memcmp(data.data(), kKeyMagic, sizeof(kKeyMagic)) != 0 ||
      !DpapiUnprotect(data.substr(sizeof(kKeyMagic)), &plain) || plain.size() != kKeySize)
    return ReadResult::kFailed;
  std::memcpy(key, plain.data(), kKeySize);
  SecureZeroMemory(&plain[0], plain.size());
  return ReadResult::kOk;
}

// 新金鑰先寫暫存檔，再以「不取代」的方式改名：輸入法服務與設定程式同時第一次啟動時，
// 只有一方的金鑰會留下，另一方改讀它。
bool CreateKeyFile(const fs::path& path, const uint8_t key[kKeySize]) {
  std::string wrapped;
  if (!DpapiProtect(std::string((const char*)key, kKeySize), &wrapped))
    return false;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path tmp = path;
  // 暫存檔名要每次都不同：同一個行程的多個執行緒也可能同時建立
  uint32_t nonce = 0;
  RandomBytes(&nonce, sizeof(nonce));
  tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetCurrentThreadId()) + L"-" +
         std::to_wstring(nonce) + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(kKeyMagic, sizeof(kKeyMagic));
    out.write(wrapped.data(), (std::streamsize)wrapped.size());
    if (!out)
      return false;
  }
  const bool moved = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
  if (!moved)
    fs::remove(tmp, ec);
  return moved;
}

}  // namespace

bool RandomBytes(void* buf, size_t size) {
  return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)size,
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool LoadOrCreateKey(uint8_t key[kKeySize]) {
  const fs::path path = KeyFilePath();
  if (path.empty())
    return false;
  // 暫時打不開就重試（最多約一秒）
  const auto read = [&] {
    ReadResult r = ReadKeyFile(path, key);
    for (int i = 0; r == ReadResult::kBusy && i < 100; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r = ReadKeyFile(path, key);
    }
    return r;
  };
  switch (read()) {
    case ReadResult::kOk:
      return true;
    case ReadResult::kBusy:
    case ReadResult::kFailed:
      return false;
    case ReadResult::kMissing:
      break;
  }
  uint8_t fresh[kKeySize];
  if (!RandomBytes(fresh, sizeof(fresh)))
    return false;
  // 建立失敗多半是另一個行程搶先建立了；不論哪一種都以檔案裡的為準
  CreateKeyFile(path, fresh);
  SecureZeroMemory(fresh, sizeof(fresh));
  return read() == ReadResult::kOk;
}

bool LegacyUnprotect(const std::string& in, std::string* out) {
  return DpapiUnprotect(in, out);
}

}  // namespace platform
}  // namespace personal_crypto
