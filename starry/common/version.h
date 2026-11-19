// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_VERSION_H_
#define STARRY_COMMON_VERSION_H_

#include "starry/common/version_values.h"

namespace starry {

inline constexpr char kVersion[] = STARRY_VERSION_STRING;
inline constexpr int kVersionMajor = STARRY_VERSION_MAJOR;
inline constexpr int kVersionMinor = STARRY_VERSION_MINOR;
inline constexpr int kVersionPatch = STARRY_VERSION_PATCH;
inline constexpr char kVersionPrerelease[] = STARRY_VERSION_PRERELEASE;
inline constexpr char kVersionBuild[] = STARRY_VERSION_BUILD;
inline constexpr char kWindowsVersion[] = STARRY_WINDOWS_VERSION_STRING;

}  // namespace starry

#endif  // STARRY_COMMON_VERSION_H_
