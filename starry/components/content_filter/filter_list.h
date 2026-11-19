// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_FILTER_LIST_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_FILTER_LIST_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/function_ref.h"
#include "base/time/time.h"

namespace starry::content_filter {

// 列表开头的 `! Title:`、`! Expires:` 这些。
struct FilterListMetadata {
  std::optional<std::string> title;
  std::optional<std::string> homepage;
  std::optional<std::string> version;
  // `! Expires: 4 days (update frequency)`、`! Expires: 12 hours`。
  std::optional<base::TimeDelta> expires;
};

FilterListMetadata ParseMetadata(std::string_view text);

// 判断下载的内容是否像过滤列表（非 HTML 网页且非空）。
bool LooksLikeFilterList(std::string_view text);

// `!#include 文件` 的文件名，按出现的顺序。
std::vector<std::string> IncludeDirectives(std::string_view text);

// 把 `!#include` 换成取到的内容（fetch 返回空时去掉这一行）。只展开一层。
std::string ExpandIncludes(
    std::string_view text,
    base::FunctionRef<std::optional<std::string>(std::string_view)> fetch);

// 预处理：`!#if 条件` … `!#else` … `!#endif`，把不成立的部分去掉之后，一行一行交给
// visit。成立的条件：env_chromium、ext_ublock、cap_user_stylesheet（Starry 是 Chromium，
// 能执行的规则和 uBlock Origin 的接近）；AdGuard、Safari、移动版的都不成立。
// cap_html_filtering（uBlock Origin 在 Firefox 上能改响应：HTML 过滤和 `$replace`）在
// replace_allowed（信得过的列表，`$replace` 能用）时成立；否则只对 HTML 过滤的行（`##^`、
// `#@#^`）成立，别的行用 `!#else` 里的替代规则（`$replace` 会被跳过）。
void ForEachActiveLine(std::string_view text,
                       base::FunctionRef<void(std::string_view)> visit,
                       bool replace_allowed = false);

// 单独求一个 `!#if` 的条件（测试用）。
bool EvaluatePreprocessorCondition(std::string_view condition);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_FILTER_LIST_H_
