#include <PersonalCrypto.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include "../platform/key_store.h"
#include "../third_party/monocypher/monocypher.h"

namespace personal_crypto {

namespace {

// 格式：magic(4) | nonce(24) | mac(16) | 密文；magic 同時作為附加驗證資料
constexpr char kMagic[4] = {'W', 'W', 'E', '1'};
constexpr size_t kNonceSize = 24;
constexpr size_t kMacSize = 16;
constexpr size_t kHeaderSize = sizeof(kMagic) + kNonceSize + kMacSize;

const uint8_t* Bytes(const char* p) {
  return reinterpret_cast<const uint8_t*>(p);
}

// 主金鑰只載入一次；載入失敗時下次再試
bool DataKey(uint8_t out[platform::kKeySize]) {
  static std::mutex mutex;
  static bool loaded = false;
  static uint8_t key[platform::kKeySize];
  std::lock_guard<std::mutex> lock(mutex);
  if (!loaded)
    loaded = platform::LoadOrCreateKey(key);
  if (loaded)
    std::memcpy(out, key, sizeof(key));
  return loaded;
}

}  // namespace

bool Protect(const std::string& in, std::string* out) {
  uint8_t key[platform::kKeySize];
  if (!DataKey(key))
    return false;
  std::string result(kHeaderSize + in.size(), '\0');
  uint8_t* p = reinterpret_cast<uint8_t*>(&result[0]);
  std::memcpy(p, kMagic, sizeof(kMagic));
  uint8_t* nonce = p + sizeof(kMagic);
  uint8_t* mac = nonce + kNonceSize;
  bool ok = platform::RandomBytes(nonce, kNonceSize);
  if (ok)
    crypto_aead_lock(p + kHeaderSize, mac, key, nonce, Bytes(kMagic), sizeof(kMagic),
                     Bytes(in.data()), in.size());
  crypto_wipe(key, sizeof(key));
  if (ok)
    *out = std::move(result);
  return ok;
}

bool Unprotect(const std::string& in, std::string* out) {
  if (in.size() < sizeof(kMagic) || std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0)
    return platform::LegacyUnprotect(in, out);
  if (in.size() < kHeaderSize)
    return false;
  uint8_t key[platform::kKeySize];
  if (!DataKey(key))
    return false;
  const uint8_t* p = Bytes(in.data());
  const uint8_t* nonce = p + sizeof(kMagic);
  const uint8_t* mac = nonce + kNonceSize;
  const size_t size = in.size() - kHeaderSize;
  std::string plain(size, '\0');
  const bool ok = crypto_aead_unlock(reinterpret_cast<uint8_t*>(&plain[0]), mac, key, nonce,
                                     Bytes(kMagic), sizeof(kMagic), p + kHeaderSize, size) == 0;
  crypto_wipe(key, sizeof(key));
  if (ok)
    *out = std::move(plain);
  return ok;
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& data) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out.write(data.data(), (std::streamsize)data.size());
    if (!out)
      return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);  // 取代既有的檔案
  return !ec;
}

bool WriteProtected(const std::filesystem::path& path, const std::string& plain) {
  std::string cipher;
  return Protect(plain, &cipher) && WriteFileAtomic(path, cipher);
}

bool ReadProtected(const std::filesystem::path& path, std::string* plain) {
  std::string cipher;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return false;
    cipher.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  return Unprotect(cipher, plain);
}

}  // namespace personal_crypto
