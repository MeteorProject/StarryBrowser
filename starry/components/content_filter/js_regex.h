// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_JS_REGEX_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_JS_REGEX_H_

#include <string>
#include <string_view>

namespace starry::content_filter {

// 过滤列表里的正则是 JS 的写法（`/…/标志` 中间的部分），这里改成 C++ 能用的引擎的写法。
// 标志另外处理（i、s 是引擎的选项，m 见下面）。

// RE2 的写法（线性时间，优先用）。`\uXXXX` → `\x{XXXX}`，命名分组 `(?<名字>` →
// `(?P<名字>`，`[^]` → `[\s\S]`，认不得的转义是字符本身。RE2 做不到的（前瞻、后顾、反向
// 引用）返回 false；重复次数超过 1000 的 RE2 编译时会失败。
bool TranslateJsRegexToRe2(std::string_view source, std::string* out);

// ICU 的写法（icu::RegexPattern，回溯的引擎，前瞻、后顾、反向引用都能用，只在 RE2 做不到
// 时用）。multiline 是 m 标志：没有时 `$` 只在结尾（ICU 的 `$` 还认结尾前的换行）。字符类
// 里 ICU 有特别意思的 `[`、`&`、`{`、`}`、`$` 加上转义，不成重复的 `{`、单独的 `}`、`]` 也是。
bool TranslateJsRegexToIcu(std::string_view source,
                           bool multiline,
                           std::string* out);

// 有没有命名分组（替换的 `$<名字>` 只在有命名分组时才认）。
bool JsRegexHasNamedGroups(std::string_view source);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_JS_REGEX_H_
