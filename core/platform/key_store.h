#pragma once

// 個人資料加密用到的平台功能。每個平台各一份實作（platform/win/…、platform/mac/…），
// 其餘加密邏輯與檔案格式在 crypto/personal_crypto.cpp 共用。
#include <cstddef>
#include <cstdint>
#include <string>

namespace personal_crypto {
namespace platform {

constexpr size_t kKeySize = 32;

// 作業系統的密碼學亂數
bool RandomBytes(void* buf, size_t size);

// 取得主金鑰；第一次使用時產生並交給作業系統保管。
// 金鑰已存在但解不開時回傳 false，絕不以新金鑰覆蓋（否則舊資料永遠讀不回來）。
bool LoadOrCreateKey(uint8_t key[kKeySize]);

// 解開舊版格式（Windows：直接以 DPAPI 加密的資料）；沒有舊版格式的平台回傳 false。
bool LegacyUnprotect(const std::string& in, std::string* out);

}  // namespace platform
}  // namespace personal_crypto
