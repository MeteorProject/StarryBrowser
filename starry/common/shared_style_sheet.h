// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_SHARED_STYLE_SHEET_H_
#define STARRY_COMMON_SHARED_STYLE_SHEET_H_

#include <string_view>

namespace starry {

// 注入样式表的开头写 `/*starry-shared:<键>*/` 时，同一个网页进程里同一个键共用一份
// 解析好的样式表（starry/renderer/shared_style_sheets.h，编进 Blink）。元素隐藏用
// （starry/renderer/content_filter/content_filter_agent.h）。
inline constexpr std::string_view kSharedStyleSheetPrefix = "/*starry-shared:";

}  // namespace starry

#endif  // STARRY_COMMON_SHARED_STYLE_SHEET_H_
