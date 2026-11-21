// Copyright 2026 The Starry Browser Authors

#include "starry/browser/updater/platform_updater.h"

#include <array>

#include "base/files/file.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "starry/common/branding.h"

namespace starry::updater {

InstallEnvironment::InstallEnvironment() = default;
InstallEnvironment::InstallEnvironment(const InstallEnvironment&) = default;
InstallEnvironment& InstallEnvironment::operator=(const InstallEnvironment&) =
    default;
InstallEnvironment::~InstallEnvironment() = default;

StageResult::StageResult() = default;
StageResult::StageResult(const StageResult&) = default;
StageResult& StageResult::operator=(const StageResult&) = default;
StageResult::~StageResult() = default;

const char* InstallKindName(InstallKind kind) {
  switch (kind) {
    case InstallKind::kMacApp:
      return "macApp";
    case InstallKind::kMacReadOnly:
      return "macReadOnly";
    case InstallKind::kWinInstaller:
      return "winInstaller";
    case InstallKind::kWinPortable:
      return "winPortable";
    case InstallKind::kLinuxDeb:
      return "linuxDeb";
    case InstallKind::kLinuxRpm:
      return "linuxRpm";
    case InstallKind::kManual:
      return "manual";
  }
}

std::string CurrentArch() {
#if defined(ARCH_CPU_ARM64)
  return "arm64";
#elif defined(ARCH_CPU_X86_64)
  return "x64";
#elif defined(ARCH_CPU_X86)
  return "x86";
#else
  return "unknown";
#endif
}

std::string PackageProductName() {
  std::string name;
  for (char ch : std::string_view(starry::kProductName)) {
    if (ch != ' ') {
      name.push_back(ch);
    }
  }
  return name;
}

std::string ExpectedAssetName(const InstallEnvironment& env,
                              std::string_view version) {
  const std::string product = PackageProductName();
  const std::string& arch = env.arch;
  switch (env.kind) {
    case InstallKind::kMacApp:
      return base::StrCat({product, "-", version, "-mac-", arch, ".zip"});
    case InstallKind::kMacReadOnly:
      return base::StrCat({product, "-", version, "-mac-", arch, ".dmg"});
    case InstallKind::kWinInstaller:
      return base::StrCat({product, "-", version, "-win-", arch, "-setup.exe"});
    case InstallKind::kWinPortable:
      return base::StrCat(
          {product, "-", version, "-win-", arch, "-portable.zip"});
    case InstallKind::kLinuxDeb:
      return base::StrCat({product, "-", version, "-linux-", arch, ".deb"});
    case InstallKind::kLinuxRpm:
      return base::StrCat({product, "-", version, "-linux-", arch, ".rpm"});
    case InstallKind::kManual:
      return {};
  }
}

std::optional<std::string> ComputeFileSha256(const base::FilePath& path) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return std::nullopt;
  }
  std::array<uint8_t, crypto::hash::kSha256Size> digest{};
  if (!crypto::hash::HashFile(crypto::hash::kSha256, &file, digest)) {
    return std::nullopt;
  }
  return base::HexEncodeLower(digest);
}

}  // namespace starry::updater
