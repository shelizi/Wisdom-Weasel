#pragma once

// 個人詞庫的檔案加密（Windows DPAPI，只有目前的 Windows 帳號能解開）。
// 輸入法服務與設定程式共用：設定程式讀寫詞彙清單、維護精煉規則時用同一組 entropy。
#include <windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#pragma comment(lib, "crypt32.lib")

namespace personal_crypto {

// 額外的 DPAPI entropy：避免其他程式把同帳號下別的 DPAPI 資料誤當成詞庫
inline const char* Entropy() {
  return "Wisdom-Weasel personal lexicon v1";
}

inline bool Protect(const std::string& in, std::string* out) {
  DATA_BLOB input{(DWORD)in.size(), (BYTE*)in.data()};
  DATA_BLOB entropy{(DWORD)strlen(Entropy()), (BYTE*)Entropy()};
  DATA_BLOB output{};
  if (!CryptProtectData(&input, L"Weasel personal lexicon", &entropy, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output))
    return false;
  out->assign((const char*)output.pbData, output.cbData);
  LocalFree(output.pbData);
  return true;
}

inline bool Unprotect(const std::string& in, std::string* out) {
  DATA_BLOB input{(DWORD)in.size(), (BYTE*)in.data()};
  DATA_BLOB entropy{(DWORD)strlen(Entropy()), (BYTE*)Entropy()};
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
    return false;
  out->assign((const char*)output.pbData, output.cbData);
  LocalFree(output.pbData);
  return true;
}

// 先寫暫存檔再取代，避免存到一半斷電留下壞檔
inline bool WriteFileAtomic(const std::filesystem::path& path, const std::string& data) {
  const std::filesystem::path tmp = path.wstring() + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out.write(data.data(), (std::streamsize)data.size());
    if (!out)
      return false;
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// 加密寫檔 / 讀檔解密
inline bool WriteProtected(const std::filesystem::path& path, const std::string& plain) {
  std::string cipher;
  return Protect(plain, &cipher) && WriteFileAtomic(path, cipher);
}

inline bool ReadProtected(const std::filesystem::path& path, std::string* plain) {
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
