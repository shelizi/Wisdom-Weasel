// macOS：主金鑰存在登入鑰匙圈（generic password，服務 Wisdom-Weasel、帳號 personal-key），
// 只在這台電腦上、登入後可用，不跟著 iCloud 同步（與 Windows 的 DPAPI 對應）。
// 注意：尚未在 macOS 上編譯驗證。
//
// 鑰匙圈項目的存取權限屬於建立它的程式：個人詞庫與設定頁都在鼠鬚管（輸入法）行程裡執行，
// 同一個程式讀寫不會跳出詢問。若日後改由其他程式讀取，第一次會詢問使用者是否允許。
#include "../key_store.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cstring>

namespace personal_crypto {
namespace platform {

namespace {

constexpr const char* kService = "Wisdom-Weasel";
constexpr const char* kAccount = "personal-key";

// CFTypeRef 的自動釋放
template <typename T>
struct CF {
  T ref = nullptr;
  CF() = default;
  explicit CF(T r) : ref(r) {}
  ~CF() {
    if (ref)
      CFRelease(ref);
  }
  CF(const CF&) = delete;
  CF& operator=(const CF&) = delete;
};

CFStringRef Str(const char* s) {
  return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

// 查詢／新增共用的條件
CFMutableDictionaryRef BaseQuery() {
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
  CF<CFStringRef> service(Str(kService));
  CF<CFStringRef> account(Str(kAccount));
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrService, service.ref);
  CFDictionarySetValue(q, kSecAttrAccount, account.ref);
  return q;
}

enum class ReadResult { kOk, kMissing, kFailed };

ReadResult ReadKey(uint8_t key[kKeySize]) {
  CF<CFMutableDictionaryRef> q(BaseQuery());
  CFDictionarySetValue(q.ref, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(q.ref, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef result = nullptr;
  const OSStatus status = SecItemCopyMatching(q.ref, &result);
  if (status == errSecItemNotFound)
    return ReadResult::kMissing;
  if (status != errSecSuccess || !result)
    return ReadResult::kFailed;  // 例如使用者拒絕存取：不可當成沒有金鑰而另建一把
  CF<CFTypeRef> data(result);
  if (CFGetTypeID(result) != CFDataGetTypeID() || CFDataGetLength((CFDataRef)result) != (CFIndex)kKeySize)
    return ReadResult::kFailed;
  std::memcpy(key, CFDataGetBytePtr((CFDataRef)result), kKeySize);
  return ReadResult::kOk;
}

// 新增金鑰；已經有了（另一個執行緒或行程搶先）時回傳 true，之後以鑰匙圈裡的為準
bool AddKey(const uint8_t key[kKeySize]) {
  CF<CFMutableDictionaryRef> q(BaseQuery());
  CF<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, key, (CFIndex)kKeySize));
  CF<CFStringRef> label(Str("Wisdom-Weasel personal lexicon key"));
  CFDictionarySetValue(q.ref, kSecValueData, data.ref);
  CFDictionarySetValue(q.ref, kSecAttrLabel, label.ref);
  // 登入鑰匙圈（檔案型）的項目只在這台電腦上，不會同步到 iCloud
  const OSStatus status = SecItemAdd(q.ref, nullptr);
  return status == errSecSuccess || status == errSecDuplicateItem;
}

}  // namespace

bool RandomBytes(void* buf, size_t size) {
  return SecRandomCopyBytes(kSecRandomDefault, size, buf) == errSecSuccess;
}

bool LoadOrCreateKey(uint8_t key[kKeySize]) {
  switch (ReadKey(key)) {
    case ReadResult::kOk:
      return true;
    case ReadResult::kFailed:
      return false;
    case ReadResult::kMissing:
      break;
  }
  uint8_t fresh[kKeySize];
  if (!RandomBytes(fresh, sizeof(fresh)))
    return false;
  const bool added = AddKey(fresh);
  std::memset(fresh, 0, sizeof(fresh));
  return added && ReadKey(key) == ReadResult::kOk;
}

bool LegacyUnprotect(const std::string&, std::string*) {
  return false;  // macOS 沒有舊版格式
}

}  // namespace platform
}  // namespace personal_crypto
