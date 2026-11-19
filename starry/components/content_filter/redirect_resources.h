// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_REDIRECT_RESOURCES_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_REDIRECT_RESOURCES_H_

#include <string_view>

namespace starry::content_filter {

// 被拦下的请求换成的替身资源（$redirect=）：空的脚本、透明的图片，以及几个常见的
// 统计、广告脚本的空实现（网页调用它们时不报错、等着回调的照样能继续）。都是自己写的，
// 不用 uBlock Origin 的 GPLv3 资源；名字和它的一样，过滤列表直接能用。
struct RedirectResource {
  std::string_view name;
  std::string_view mime_type;
  std::string_view body;
};

// 规范名字（CanonicalRedirectName 返回的）找资源；没有时返回空。
const RedirectResource* FindRedirectResource(std::string_view name);

// 规则里写的名字（别名、Adblock Plus 的 `abp-resource:` 名字、带 `:优先级`）换成规范
// 名字；不认识的返回空。
std::string_view CanonicalRedirectName(std::string_view name);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_REDIRECT_RESOURCES_H_
