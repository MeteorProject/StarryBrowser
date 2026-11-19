// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_FILTER_PARSER_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_FILTER_PARSER_H_

#include <string>
#include <string_view>
#include <vector>

#include "starry/components/content_filter/content_filter_types.h"
#include "starry/components/content_filter/domain_set.h"
#include "starry/components/content_filter/network_filter.h"
#include "starry/components/content_filter/procedural_selector.h"

namespace starry::content_filter {

// 一条元素隐藏规则（`网站##选择器`、`网站#@#选择器`）。
struct CosmeticFilter {
  enum class Kind : uint8_t {
    // 藏起来（display: none）。
    kHide,
    // 加样式（uBlock Origin 的 `:style()`、AdGuard 的 `#$#选择器 { … }`），只能限定网站。
    kStyle,
    // 扩展选择器（`:has-text()` 这类，procedural_selector.h），在网页里用脚本找，只能限定
    // 网站（同 uBlock Origin）。
    kProcedural,
    // HTML 过滤（`网站##^选择器`，html_filter.h）：浏览器从页面的 HTML 源码里删掉，只能限定
    // 网站。
    kHtml,
    // 去掉页面的响应头（`网站##^responseheader(名字)`），style 是小写的名字。
    kResponseHeader,
  };

  CosmeticFilter();
  CosmeticFilter(CosmeticFilter&&);
  CosmeticFilter& operator=(CosmeticFilter&&);
  ~CosmeticFilter();

  Kind kind = Kind::kHide;
  bool exception = false;
  // kHide、kStyle 是 CSS 的选择器；kProcedural 是整条的写法（例外按它对）；kHtml、
  // kResponseHeader 是 `^` 加上 `##^` 后面的写法（例外按它对，不和元素隐藏的混）。
  std::string selector;
  // kStyle 和 `:style()` 动作的样式（大括号里面的）；`:remove-attr()`、`:remove-class()`
  // 动作是编好的名字（CompileNameMatcher）。
  std::string style;
  // 编好的扩展选择器（CompileProceduralSelector）。kHide 也可能有：`选择器:remove()` 除了
  // 用样式表藏起来，还要在网页里删掉。kHtml 的也是这样编的（HtmlSelector::Compile 用）。
  std::string procedural;
  ProceduralAction action = ProceduralAction::kHide;
  // 空的是不限网站。
  DomainSet domains;
};

// 一条脚本片段规则：uBlock Origin 的 `网站##+js(名字, 参数…)`、`网站#@#+js(…)`，AdGuard 的
// `网站#%#//scriptlet('名字', '参数')`，Adblock Plus 的 `网站#$#名字 参数; …`（一行可能有几条）。
struct ScriptletFilter {
  ScriptletFilter();
  ScriptletFilter(ScriptletFilter&&);
  ScriptletFilter& operator=(ScriptletFilter&&);
  ~ScriptletFilter();

  bool exception = false;
  // FindScriptlet 的编号。例外里没写名字（`网站#@#+js()`）时是 kNoScriptlet：这些网站上一个
  // 也不跑。
  uint16_t scriptlet;
  // 去掉了引号、转义的参数，末尾空的去掉。
  std::vector<std::string> args;
  // 空的是不限网站。
  DomainSet domains;
};

// 解析一行的结果。一行可能变成几条网络规则：hosts 文件一行写几个网站，`$redirect=`
// 是一条拦截加一条换资源。
struct ParsedLine {
  enum class Type : uint8_t {
    // 空行、注释、列表头。
    kNone,
    kNetwork,
    kCosmetic,
    kScriptlet,
    kSkipped,
  };

  ParsedLine();
  ParsedLine(ParsedLine&&);
  ParsedLine& operator=(ParsedLine&&);
  ~ParsedLine();

  Type type = Type::kNone;
  SkipReason reason = SkipReason::kOption;
  std::vector<NetworkFilter> network;
  CosmeticFilter cosmetic;
  std::vector<ScriptletFilter> scriptlets;
  // 带 $badfilter 的规则：去掉 badfilter 之后的写法（要去掉的那条规则，和列表里
  // 那一行的文字一样）。
  bool badfilter = false;
  std::string badfilter_target;
};

// hosts 文件、单独一个网站名的一行当作 `||网站^`，$badfilter 按这个写法对。
std::string HostFilterText(std::string_view host);

// `网站#@#选择器:style(样式)` 这种例外的键（CosmeticFilter::selector），只对同样的选择器加同样的
// 样式。
std::string StyleExceptionKey(std::string_view selector,
                              std::string_view style);

ParsedLine ParseFilterLine(std::string_view line);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_FILTER_PARSER_H_
