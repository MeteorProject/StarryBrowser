// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_TRACKING_PARAMETERS_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_TRACKING_PARAMETERS_H_

#include <string_view>

#include "base/functional/function_ref.h"
#include "url/gurl.h"

namespace starry::content_filter {

// 链接里只用来跨网站追踪的参数（点了哪个广告、哪封邮件、哪条帖子）：使用固定名单，
// 不做启发式猜测（避免误删用于网站功能的参数导致页面异常）。
bool IsTrackingParameter(std::string_view name);

// 去掉 should_remove 认为要去掉的参数；其余参数和它们的编码、顺序原样保留。没有可去
// 的（或者不是 http、https）时返回空的 GURL。should_remove 拿到的是解码前的参数名。
GURL RemoveQueryParameters(
    const GURL& url,
    base::FunctionRef<bool(std::string_view name, std::string_view value)>
        should_remove);

// 去掉跟踪参数（设置 › 隐私 › 跟踪保护）。
GURL StripTrackingParameters(const GURL& url);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_TRACKING_PARAMETERS_H_
