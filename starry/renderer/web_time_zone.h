// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_WEB_TIME_ZONE_H_
#define STARRY_RENDERER_WEB_TIME_ZONE_H_

#include <string>

#include "third_party/blink/public/platform/web_common.h"

namespace starry {

// Blink 初始化时、任何网页和 Worker 的 V8 isolate 建立之前调用。
void InitializeWebTimeZone();

// 浏览器推来的新时区（starry.mojom.WebTimeZone，在主线程）。同系统时区变化：成为
// 这个进程的默认时区，通知主线程和所有 Worker 的 V8；DevTools 的临时覆盖仍优先。
// 给 Blink 外面的 starry/renderer 用（编进 Blink core，所以用 BLINK_EXPORT）。
BLINK_EXPORT void SetWebTimeZone(const std::string& id);

}  // namespace starry

#endif  // STARRY_RENDERER_WEB_TIME_ZONE_H_
