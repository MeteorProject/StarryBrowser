// Copyright 2026 The Starry Browser Authors

#include "starry/browser/importer/chromium_passwords.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <iterator>
#include <optional>
#include <string>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#include "base/strings/sys_string_conversions.h"

namespace starry {

// 同 crypto::apple::KeychainV2::FindGenericPassword（它的头文件是 Objective-C 的）：
// 找登录钥匙串里的通用密码项目。项目是别的 App 建的，系统会弹出对话框问用户允不允许，
// 这个调用一直等到用户回答。
std::optional<std::string> ReadKeychainPassword(const std::string& service,
                                                const std::string& account) {
  base::apple::ScopedCFTypeRef<CFStringRef> service_ref =
      base::SysUTF8ToCFStringRef(service);
  base::apple::ScopedCFTypeRef<CFStringRef> account_ref =
      base::SysUTF8ToCFStringRef(account);
  const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                        kSecReturnData, kSecMatchLimit};
  const void* values[] = {kSecClassGenericPassword, service_ref.get(),
                          account_ref.get(), kCFBooleanTrue, kSecMatchLimitOne};
  base::apple::ScopedCFTypeRef<CFDictionaryRef> query(CFDictionaryCreate(
      kCFAllocatorDefault, keys, values, std::size(keys),
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  base::apple::ScopedCFTypeRef<CFTypeRef> result;
  const OSStatus status =
      SecItemCopyMatching(query.get(), result.InitializeInto());
  if (status != errSecSuccess || !result ||
      CFGetTypeID(result.get()) != CFDataGetTypeID()) {
    return std::nullopt;
  }
  const base::span<const uint8_t> bytes =
      base::apple::CFDataToSpan(static_cast<CFDataRef>(result.get()));
  return std::string(base::as_string_view(bytes));
}

}  // namespace starry
