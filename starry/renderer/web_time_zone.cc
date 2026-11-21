// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/web_time_zone.h"

#include "base/command_line.h"
#include "starry/common/web_time_zone.h"
#include "third_party/blink/renderer/core/timezone/timezone_controller.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

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

void SetWebTimeZone(const std::string& id) {
  if (!CreateWebTimeZone(id)) {
    return;
  }
  // 就是系统时区通知的路径（TimeZoneController::OnTimeZoneChange）：记住为这个进程的
  // 时区，没有 DevTools 覆盖时立刻生效。用它公开的入口，不再改上游的头文件。
  blink::TimeZoneController::ChangeTimeZoneForTesting(
      blink::String::FromUtf8(id));
}

}  // namespace starry
