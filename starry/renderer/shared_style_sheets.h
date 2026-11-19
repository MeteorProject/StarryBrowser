// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_SHARED_STYLE_SHEETS_H_
#define STARRY_RENDERER_SHARED_STYLE_SHEETS_H_

#include "starry/common/shared_style_sheet.h"

// 编进 Blink 的 core（patches/third_party/blink/renderer/core/BUILD.gn.patch），
// WebDocument::InsertStyleSheet 先问它（web_document.cc.patch）。

namespace blink {
class StyleSheetContents;
class String;
}  // namespace blink

namespace starry {

// 注入样式表的开头写 `/*starry-shared:<键>*/` 时，同一个网页进程里同一个键共用一份
// 解析好的 StyleSheetContents（和它的 RuleSet）：不限网站的元素隐藏有上万条选择器，
// 每个文档、每个 iframe 都解析一遍要好几毫秒，共用以后只在进程第一次用到时解析。
// 不是这种写法的返回空，照原来的办法解析。最多留最近的几份（规则更新时换新的键）。

blink::StyleSheetContents* SharedStyleSheetContents(const blink::String& source);

}  // namespace starry

#endif  // STARRY_RENDERER_SHARED_STYLE_SHEETS_H_
