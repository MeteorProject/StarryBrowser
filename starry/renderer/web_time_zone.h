// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_WEB_TIME_ZONE_H_
#define STARRY_RENDERER_WEB_TIME_ZONE_H_

namespace starry {

// Blink 初始化时、任何网页和 Worker 的 V8 isolate 建立之前调用。
void InitializeWebTimeZone();

}  // namespace starry

#endif  // STARRY_RENDERER_WEB_TIME_ZONE_H_
