// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_NETWORK_FILTER_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_NETWORK_FILTER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "starry/components/content_filter/content_filter_types.h"
#include "starry/components/content_filter/domain_set.h"

namespace re2 {
class RE2;
}

namespace starry::content_filter {

class ReplaceFilter;
class Request;

// 网络规则是做什么的。
enum class NetworkFilterKind : uint8_t {
  // 拦截（例外是放行）。
  kBlock,
  // 被拦下时换成替身资源（$redirect、$redirect-rule）。value 是资源名。
  kRedirect,
  // 给页面加内容安全策略（$csp）。value 是策略。
  kCsp,
  // 去掉网址参数（$removeparam）。value 是参数名、`/正则/` 或空（全部）。
  kRemoveParam,
  // 改响应的内容（$replace，只收信得过的列表里的）。value 是 `replace=` 后面的写法（例外按
  // 它对，空的例外是全部），编好的在 replace。
  kReplace,
};

// 一条网络规则（Adblock Plus / uBlock Origin / AdGuard 共同的写法）。网址模式在解析时
// 规范好：去掉开头的 `||`、`|` 和结尾的 `|`（记成标志），连续的 `*` 并成一个，不区分
// 大小写时转成小写。
struct NetworkFilter {
  enum Flag : uint32_t {
    kException = 1u << 0,
    kImportant = 1u << 1,
    kMatchCase = 1u << 2,
    // |模式：从网址开头匹配。
    kLeftAnchor = 1u << 3,
    // ||模式：从主机名的开头或某个点后面匹配。
    kHostAnchor = 1u << 4,
    // 模式|：匹配到网址结尾。
    kRightAnchor = 1u << 5,
    kRegex = 1u << 6,
    // 第一方、第三方的请求（两个都没有时都算）。
    kFirstParty = 1u << 7,
    kThirdParty = 1u << 8,
    // 按主机名而不是按可注册的网站判断第一方、第三方（strict1p、strict3p）。
    kStrictParty = 1u << 9,
    // 模式里有 `*` 或 `^`。
    kWildcard = 1u << 10,
    // 写了类型选项（不是默认的类型）。
    kExplicitTypes = 1u << 11,
  };

  NetworkFilter();
  NetworkFilter(NetworkFilter&&);
  NetworkFilter& operator=(NetworkFilter&&);
  ~NetworkFilter();

  bool is_exception() const { return flags & kException; }
  bool is_important() const { return flags & kImportant; }
  // 主机名部分（只有能按网站建索引的 `||网站^…` 才有）。
  std::string_view host_part() const {
    return std::string_view(pattern).substr(0, host_length);
  }
  // 只按网站就能定下来：`||网站^`，没有别的选项。
  bool IsPlainHost() const;

  bool MatchesOptions(const Request& request) const;
  bool MatchesPattern(const Request& request) const;
  bool Matches(const Request& request) const {
    return MatchesOptions(request) && MatchesPattern(request);
  }

  // 可以用来建索引的分词（散列）。没有可用的分词时为空。
  std::vector<uint32_t> TokenCandidates() const;

  NetworkFilterKind kind = NetworkFilterKind::kBlock;
  uint32_t flags = 0;
  uint32_t types = kNetworkTypes;
  // 整页例外（只在例外上）：DocumentFlag 的并集。
  uint32_t doc_flags = 0;
  uint16_t methods = 0;
  bool methods_negated = false;
  // 来自第几份列表。
  uint16_t list = 0;
  // `||网站^…` 里网站的长度，0 表示不能按网站建索引。
  uint16_t host_length = 0;
  std::string pattern;
  std::string value;
  std::unique_ptr<re2::RE2> regex;
  // $removeparam=/正则/ 的正则（对 `名=值`）。
  std::unique_ptr<re2::RE2> value_regex;
  // $replace 编好的（例外没有）。
  std::unique_ptr<ReplaceFilter> replace;
  // $domain=：发出请求的网页。
  std::unique_ptr<DomainSet> domains;
  // $to= / $denyallow=：请求的网站。
  std::unique_ptr<DomainSet> to_domains;
};

// 带 `*`、`^` 的模式从 text 的 pos 处开始匹配（第一段必须从 pos 开始）。
// right_anchor：最后一段要匹配到结尾。
bool MatchPatternAt(std::string_view pattern,
                    std::string_view text,
                    size_t pos,
                    bool right_anchor);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_NETWORK_FILTER_H_
