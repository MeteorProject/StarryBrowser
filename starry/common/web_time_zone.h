// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_WEB_TIME_ZONE_H_
#define STARRY_COMMON_WEB_TIME_ZONE_H_

#include <memory>
#include <string_view>

#include "base/strings/string_util.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"

namespace starry {

// 只传给网页进程；浏览器自己的时区仍跟随系统。
inline constexpr char kWebTimeZoneSwitch[] = "starry-web-time-zone";

// 只接受 ICU 认识的 IANA 时区（包括旧名字），不把无效值悄悄当作 GMT。
inline std::unique_ptr<icu::TimeZone> CreateWebTimeZone(std::string_view id) {
  if (id.empty() || id.size() > 256 || !base::IsStringASCII(id)) {
    return nullptr;
  }
  const icu::UnicodeString name = icu::UnicodeString::fromUTF8(id);
  icu::UnicodeString iana;
  UErrorCode status = U_ZERO_ERROR;
  icu::TimeZone::getIanaID(name, iana, status);
  if (U_FAILURE(status)) {
    return nullptr;
  }
  return std::unique_ptr<icu::TimeZone>(icu::TimeZone::createTimeZone(name));
}

}  // namespace starry

#endif  // STARRY_COMMON_WEB_TIME_ZONE_H_
