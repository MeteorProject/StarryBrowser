// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_CONTENT_FILTER_TYPES_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_CONTENT_FILTER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

// 过滤引擎共用的小东西：请求类型、整页例外、跳过的原因、
// 散列和分词。不依赖 content、Blink，浏览器进程和单元测试都用它。
namespace starry::content_filter {

// 请求的类型，每个请求正好一种（同 Adblock Plus 的类型选项）。规则里是这些位的并集。
enum RequestType : uint32_t {
  kTypeOther = 1u << 0,
  kTypeScript = 1u << 1,
  kTypeImage = 1u << 2,
  kTypeStylesheet = 1u << 3,
  kTypeObject = 1u << 4,
  kTypeXmlHttpRequest = 1u << 5,
  kTypeSubdocument = 1u << 6,
  kTypePing = 1u << 7,
  kTypeMedia = 1u << 8,
  kTypeFont = 1u << 9,
  kTypeWebSocket = 1u << 10,
  // 顶层页面。网络规则不拦顶层页面，只有去参数、整页例外用到。
  kTypeDocument = 1u << 11,
  // 网页打开的新窗口（window.open、target=_blank）。
  kTypePopup = 1u << 12,
};

// 不写类型时规则管的类型：除了顶层页面和弹出窗口的全部（同 Adblock Plus）。
inline constexpr uint32_t kNetworkTypes = (1u << 11) - 1;
inline constexpr uint32_t kAllTypes = (1u << 13) - 1;

// 整页例外（`@@…$document` 这类）：这个页面上关掉哪些拦截。
enum DocumentFlag : uint32_t {
  // $document：整个页面都不拦（网络、元素隐藏、脚本片段）。
  kDocDocument = 1u << 0,
  // $elemhide：不隐藏元素。
  kDocElemHide = 1u << 1,
  // $generichide：不用不限网站的元素隐藏。
  kDocGenericHide = 1u << 2,
  // $specifichide：不用限定网站的元素隐藏。
  kDocSpecificHide = 1u << 3,
  // $genericblock：不用不限网站的网络规则。
  kDocGenericBlock = 1u << 4,
  // $urlblock：不拦网络请求（元素隐藏照常）。
  kDocUrlBlock = 1u << 5,
  // $jsinject（AdGuard）：不跑脚本片段。
  kDocJsInject = 1u << 6,
};

// 跳过一条规则的原因（设置页的「跳过 N 条」）。
enum class SkipReason : uint8_t {
  kOption,       // 不支持的选项
  kPattern,      // 网址模式太宽（什么都不限制）或写错了
  kRegex,        // 正则编译不了（前瞻、反向引用……）
  kDomain,       // 网站的写法不支持（正则网站、IDN 转不了）
  kDocumentBlock,  // 拦整个页面（$document 拦截）
  kExtendedCss,  // 扩展选择器：不支持的（:-abp-properties()、:shadow()）、不限网站的
  kScriptlet,    // 脚本片段：不认识的、任意的脚本（#%#）、信不过的列表里的 trusted-
  kHtmlFilter,   // HTML 过滤（##^、$$）
  kRedirect,     // 不认识的替身资源
  kCosmetic,     // 元素隐藏的选择器写错了
  kCount,
};
inline constexpr size_t kSkipReasonCount =
    static_cast<size_t>(SkipReason::kCount);

// ---- 散列 ----

// FNV-1a。分词的散列只用来缩小候选范围（命中后还要逐条核对），32 位够了；
// 网站的散列直接决定拦不拦（纯网站规则不再核对），用 64 位。
inline uint32_t HashToken(std::string_view text) {
  uint32_t hash = 2166136261u;
  for (char c : text) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

inline uint64_t HashHost(std::string_view text) {
  uint64_t hash = 14695981039346656037ull;
  for (char c : text) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

// 分词用的字符（输入已经是小写）：字母、数字和 %（同 uBlock Origin）。
inline bool IsTokenChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '%';
}

// Adblock Plus 的分隔符 `^`：字母、数字和 `_-.%` 以外的任何字符（网址结尾也算，
// 在匹配的地方处理）。
inline bool IsSeparatorChar(char c) {
  return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
           c == '%');
}

// 索引用的分词至少这么长：一个字母的分词到处都是，没有区分度。
inline constexpr size_t kMinTokenLength = 2;
// 网址只看前面这么长（同 uBlock Origin），太长的查询串里的分词不再展开。
inline constexpr size_t kMaxTokenizedUrlLength = 2048;

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_CONTENT_FILTER_TYPES_H_
