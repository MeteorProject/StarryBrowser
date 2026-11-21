// Copyright 2026 The Starry Browser Authors

#include "starry/browser/updater/semver.h"

#include <algorithm>
#include <cctype>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace starry::updater {

namespace {

bool IsAsciiDigit(char ch) {
  return ch >= '0' && ch <= '9';
}

bool IsIdentChar(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= 'a' && ch <= 'z') || ch == '-';
}

bool IsAllDigits(std::string_view s) {
  return !s.empty() && std::ranges::all_of(s, IsAsciiDigit);
}

bool ParseNumericComponent(std::string_view part, uint64_t* out) {
  if (part.empty() || !IsAllDigits(part)) {
    return false;
  }
  if (part.size() > 1 && part[0] == '0') {
    return false;
  }
  return base::StringToUint64(part, out);
}

}  // namespace

SemVer::SemVer() = default;
SemVer::SemVer(const SemVer&) = default;
SemVer& SemVer::operator=(const SemVer&) = default;
SemVer::SemVer(SemVer&&) noexcept = default;
SemVer& SemVer::operator=(SemVer&&) noexcept = default;
SemVer::~SemVer() = default;

std::string SemVer::Format() const {
  std::string out = base::StrCat({base::NumberToString(major), ".",
                                  base::NumberToString(minor), ".",
                                  base::NumberToString(patch)});
  if (!prerelease.empty()) {
    out.push_back('-');
    out.append(base::JoinString(prerelease, "."));
  }
  if (!build.empty()) {
    out.push_back('+');
    out.append(build);
  }
  return out;
}

bool SemVer::operator==(const SemVer& other) const {
  return (*this <=> other) == std::strong_ordering::equal;
}

std::strong_ordering SemVer::operator<=>(const SemVer& other) const {
  if (auto cmp = major <=> other.major; cmp != 0) {
    return cmp;
  }
  if (auto cmp = minor <=> other.minor; cmp != 0) {
    return cmp;
  }
  if (auto cmp = patch <=> other.patch; cmp != 0) {
    return cmp;
  }
  // 正式版优先于预发布版。
  if (prerelease.empty() && other.prerelease.empty()) {
    return std::strong_ordering::equal;
  }
  if (prerelease.empty()) {
    return std::strong_ordering::greater;
  }
  if (other.prerelease.empty()) {
    return std::strong_ordering::less;
  }

  const size_t count = std::min(prerelease.size(), other.prerelease.size());
  for (size_t i = 0; i < count; ++i) {
    const std::string& a = prerelease[i];
    const std::string& b = other.prerelease[i];
    if (a == b) {
      continue;
    }
    const bool a_num = IsAllDigits(a);
    const bool b_num = IsAllDigits(b);
    if (a_num && b_num) {
      uint64_t av = 0;
      uint64_t bv = 0;
      if (base::StringToUint64(a, &av) && base::StringToUint64(b, &bv)) {
        return av <=> bv;
      }
      if (a.size() != b.size()) {
        return a.size() <=> b.size();
      }
      return a <=> b;
    }
    if (a_num != b_num) {
      // 数字标识符优先级低于非数字标识符。
      return a_num ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return a <=> b;
  }
  return prerelease.size() <=> other.prerelease.size();
}

std::optional<SemVer> ParseSemVer(std::string_view input) {
  std::string_view text = base::TrimWhitespaceASCII(input, base::TRIM_ALL);
  if (text.empty()) {
    return std::nullopt;
  }
  if (text[0] == 'v' || text[0] == 'V') {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  std::string_view build;
  if (const size_t plus = text.find('+'); plus != std::string_view::npos) {
    build = text.substr(plus + 1);
    text = text.substr(0, plus);
    if (build.empty()) {
      return std::nullopt;
    }
    for (std::string_view part : base::SplitStringPiece(
             build, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
      if (part.empty() || !std::ranges::all_of(part, IsIdentChar)) {
        return std::nullopt;
      }
    }
  }

  std::string_view prerelease_str;
  if (const size_t dash = text.find('-'); dash != std::string_view::npos) {
    prerelease_str = text.substr(dash + 1);
    text = text.substr(0, dash);
    if (prerelease_str.empty()) {
      return std::nullopt;
    }
  }

  const std::vector<std::string_view> core = base::SplitStringPiece(
      text, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (core.size() != 3) {
    return std::nullopt;
  }

  SemVer ver;
  if (!ParseNumericComponent(core[0], &ver.major) ||
      !ParseNumericComponent(core[1], &ver.minor) ||
      !ParseNumericComponent(core[2], &ver.patch)) {
    return std::nullopt;
  }

  if (!prerelease_str.empty()) {
    for (std::string_view part : base::SplitStringPiece(
             prerelease_str, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
      if (part.empty() || !std::ranges::all_of(part, IsIdentChar)) {
        return std::nullopt;
      }
      if (IsAllDigits(part) && part.size() > 1 && part[0] == '0') {
        return std::nullopt;
      }
      ver.prerelease.emplace_back(part);
    }
  }

  ver.build = std::string(build);
  return ver;
}

bool IsNewerVersion(const SemVer& candidate, const SemVer& current) {
  if (!current.IsPrerelease() && candidate.IsPrerelease()) {
    return false;
  }
  return candidate > current;
}

}  // namespace starry::updater
