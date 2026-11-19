// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/web_time_zone.h"

#include "base/command_line.h"
#include "starry/common/web_time_zone.h"

namespace starry {

void InitializeWebTimeZone() {
  auto zone = CreateWebTimeZone(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kWebTimeZoneSwitch));
  if (zone) {
    // 这是网页进程的默认时区，不占用 DevTools 的临时覆盖。TimeZoneController
    // 随后记住它；浏览器的时区通知也会使用这个值，清除 DevTools 覆盖时可恢复。
    icu::TimeZone::adoptDefault(zone.release());
  }
}

}  // namespace starry
