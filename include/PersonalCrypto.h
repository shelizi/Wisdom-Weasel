#pragma once

// 個人資料的檔案加密（詞庫、精煉規則、輸入與選字紀錄）。
// 輸入法服務與設定程式共用。實作在 core/crypto/personal_crypto.cpp：
// 資料以 XChaCha20-Poly1305 加密，格式在各平台相同；只有 32 位元組的主金鑰
// 交給作業系統保管（Windows：DPAPI，只有目前的帳號能解開；macOS：Keychain）。
// 舊版直接以 DPAPI 加密的資料仍可讀取，下次寫入時改存新格式。
#include <filesystem>
#include <string>

namespace personal_crypto {

bool Protect(const std::string& in, std::string* out);
bool Unprotect(const std::string& in, std::string* out);

// 先寫暫存檔再取代，避免存到一半斷電留下壞檔
bool WriteFileAtomic(const std::filesystem::path& path,
                     const std::string& data);

// 加密寫檔 / 讀檔解密
bool WriteProtected(const std::filesystem::path& path,
                    const std::string& plain);
bool ReadProtected(const std::filesystem::path& path, std::string* plain);

}  // namespace personal_crypto
