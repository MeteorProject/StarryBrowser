// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UPDATER_SEMVER_H_
#define STARRY_BROWSER_UPDATER_SEMVER_H_

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace starry::updater {

// SemVer 2.0.0 版本号，与 starry/build/version.py 保持一致。
struct SemVer {
  SemVer();
  SemVer(const SemVer&);
  SemVer& operator=(const SemVer&);
  SemVer(SemVer&&) noexcept;
  SemVer& operator=(SemVer&&) noexcept;
  ~SemVer();

  uint64_t major = 0;
  uint64_t minor = 0;
  uint64_t patch = 0;
  std::vector<std::string> prerelease;
  std::string build;

  bool IsPrerelease() const { return !prerelease.empty(); }
  // 返回不含前导 'v' 的规范版本号（含 prerelease / build）。
  std::string Format() const;

  // 按 SemVer 2.0.0 §11 比较优先级（忽略 build 元数据）。
  bool operator==(const SemVer& other) const;
  std::strong_ordering operator<=>(const SemVer& other) const;
};

// 解析 SemVer 字符串（允许可选前导 'v' / 'V'，如 GitHub tag "v1.2.3"）。
std::optional<SemVer> ParseSemVer(std::string_view input);

// 判断 candidate 是否是可向 current 推送的新版本：
// 稳定版不会自动升级到预发布版；预发布版可以升级到更高的预发布版或正式版。
bool IsNewerVersion(const SemVer& candidate, const SemVer& current);

}  // namespace starry::updater

#endif  // STARRY_BROWSER_UPDATER_SEMVER_H_
