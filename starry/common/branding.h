// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_BRANDING_H_
#define STARRY_COMMON_BRANDING_H_

#include <string>
#include <string_view>

#include "base/version_info/version_info_values.h"
#include "starry/common/platform_branding.h"

namespace starry {

// Persistent identifiers use BRANDING's PRODUCT_FULLNAME, never a translation.
inline constexpr char kProductName[] = PRODUCT_NAME;
inline constexpr char kProductId[] = STARRY_PRODUCT_ID;
inline constexpr char kApplicationId[] = STARRY_APPLICATION_ID;
inline constexpr char kSafeStorageServiceName[] = PRODUCT_NAME " Safe Storage";
inline constexpr char kSafeStorageAccountName[] = PRODUCT_NAME;
inline constexpr char kWalletFolderName[] = PRODUCT_NAME " Keys";
inline constexpr char kLinuxPolicyPath[] = STARRY_LINUX_POLICY_PATH;
inline constexpr char kLinuxDesktopFile[] = STARRY_LINUX_DESKTOP_FILE;
inline constexpr char kInitialPreferencesFileName[] =
    PRODUCT_NAME " Initial Preferences";
inline constexpr char kLegacyInitialPreferencesFileName[] =
    PRODUCT_NAME " Master Preferences";
inline constexpr char kWebAppsDirectoryName[] = PRODUCT_NAME " Apps.localized";
inline constexpr char kPdfViewerName[] = PRODUCT_NAME " PDF Viewer";
inline constexpr char16_t kPdfPluginName[] = u"" PRODUCT_NAME " PDF Plugin";

inline std::string LinuxChannelSuffix(std::string_view channel) {
  if (channel == "beta") {
    return "-beta";
  }
  if (channel == "dev" || channel == "unstable") {
    return "-unstable";
  }
  if (channel == "canary") {
    return "-canary";
  }
  return {};
}

}  // namespace starry

#endif  // STARRY_COMMON_BRANDING_H_
