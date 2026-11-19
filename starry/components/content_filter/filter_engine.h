// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_FILTER_ENGINE_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_FILTER_ENGINE_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "starry/components/content_filter/content_filter_types.h"
#include "url/gurl.h"

namespace starry::content_filter {

class HtmlSelector;
class ReplaceFilter;
class Request;

// 一份过滤列表（订阅的或者自定义规则）的文字。
struct FilterListSource {
  std::string id;
  std::string text;
  // 信得过的列表（uBlock Origin 自己的、自定义规则）：能用 trusted- 开头的脚本片段
  // （IsTrustedScriptlet）和 `$replace`，预处理的 cap_html_filtering 成立。
  bool trusted = false;
};

// 一份列表合并进来多少条、跳过多少条（重复的不算）。
struct FilterListStats {
  std::string id;
  size_t network = 0;
  size_t cosmetic = 0;
  size_t scriptlet = 0;
  size_t skipped = 0;
  std::array<size_t, kSkipReasonCount> skipped_by_reason{};

  size_t rules() const { return network + cosmetic + scriptlet; }
};

// 一个请求怎么处理。
struct NetworkMatch {
  enum class Action : uint8_t {
    // 没有规则管它。
    kNone,
    kBlock,
    // 拦下，换成替身资源 redirect。
    kRedirect,
    // 有拦截规则，但被例外放行了。
    kAllow,
  };

  NetworkMatch();
  NetworkMatch(const NetworkMatch&);
  NetworkMatch& operator=(const NetworkMatch&);
  ~NetworkMatch();

  bool blocked() const {
    return action == Action::kBlock || action == Action::kRedirect;
  }

  Action action = Action::kNone;
  bool important = false;
  // 替身资源的规范名字（FindRedirectResource）。
  std::string redirect;
  // 没拦下、但要去掉网址参数（$removeparam）：去掉之后的网址。不用去时为空。
  GURL rewritten;
};

// 一个页面的元素隐藏。
struct CosmeticResult {
  CosmeticResult();
  CosmeticResult(CosmeticResult&&);
  CosmeticResult& operator=(CosmeticResult&&);
  ~CosmeticResult();

  // 用不限网站的那份（generic_selectors()）。
  bool generic = false;
  // 这个网站上不用的通用选择器（generic_selectors() 的下标，从小到大）。
  std::vector<uint32_t> generic_exclusions;
  // 限定这个网站的：隐藏和 `:style()`，已经写成 CSS。
  std::string css;
  // 限定这个网站的条数（统计用）。
  size_t specific_count = 0;
  // 扩展选择器：JSON 的 `[[选择器, 动作, 参数…], …]`（procedural_selector.h，网页进程里
  // procedural.js 做）。`:style()` 的参数是 procedural_styles 的下标。没有时为空。
  std::string procedural;
  std::vector<std::string> procedural_styles;
  size_t procedural_count = 0;
  // 脚本片段：拼好的一段脚本（ScriptletScript），在网页自己的脚本之前跑。没有时为空。
  std::string scripts;
  size_t scriptlet_count = 0;
};

// 一个页面的 HTML 过滤（`网站##^…`）。
struct HtmlFilterResult {
  HtmlFilterResult();
  HtmlFilterResult(HtmlFilterResult&&);
  HtmlFilterResult& operator=(HtmlFilterResult&&);
  ~HtmlFilterResult();

  bool empty() const { return selectors.empty() && removed_headers.empty(); }

  // 指着引擎里的规则：用完之前要拿着引擎（FilterConfig::engine_ref()）。
  std::vector<const HtmlSelector*> selectors;
  // 要去掉的响应头（小写）。
  std::vector<std::string> removed_headers;
};

// 过滤引擎：所有启用的列表合在一起建好的索引。
// 建好以后只读，几个线程同时查没有问题（网络请求在代理的线程上查，元素隐藏在界面
// 线程上查）。换规则时整个换掉。
//
// 网络规则分成几类，各有一份索引：拦截、`$important` 拦截、例外、整页例外、替身资源、
// CSP、去参数、改响应（后四种各有自己的例外）。每份索引里：
// - 只写了网站的 `||网站^`（列表里九成以上）直接进网站散列的集合，查的时候拿请求网站的
//   各级后缀去碰，不再逐条核对；
// - 其他 `||网站^…` 按网站分桶；
// - 其余的按模式里最少见的一个分词分桶（同 uBlock Origin、adblock-rust），请求网址里
//   的每个分词查一次桶；
// - 没有分词、限定了网页的（`*$script,domain=…`）按网页的网站分桶，剩下的每次都查。
class FilterEngine : public base::RefCountedThreadSafe<FilterEngine> {
 public:
  static scoped_refptr<FilterEngine> Build(
      const std::vector<FilterListSource>& sources,
      std::vector<FilterListStats>* stats);
  static scoped_refptr<FilterEngine> CreateEmpty();

  FilterEngine(const FilterEngine&) = delete;
  FilterEngine& operator=(const FilterEngine&) = delete;

  // doc_flags 是发出请求的页面的整页例外（DocumentFlags）。
  NetworkMatch MatchRequest(const Request& request, uint32_t doc_flags) const;
  // 页面（顶层或 iframe 的文档）要加的内容安全策略。
  std::vector<std::string> CspDirectives(const Request& request,
                                         uint32_t doc_flags) const;
  // 要改响应体的 `$replace`（按列表里的顺序，去掉例外的）。指着引擎里的规则：用完之前要
  // 拿着引擎。图片、媒体、字体这些不是文字的请求没有。
  std::vector<const ReplaceFilter*> ReplaceFilters(const Request& request,
                                                   uint32_t doc_flags) const;
  // 页面的整页例外（`@@…$document`、`$elemhide`……）。parent 是 iframe 的上一层页面，
  // 顶层页面传空的。
  uint32_t DocumentFlags(const GURL& url, const GURL& parent) const;
  // 页面的元素隐藏和脚本片段。
  CosmeticResult Cosmetics(const GURL& url, uint32_t doc_flags) const;
  // 页面（顶层或 iframe 的文档）的 HTML 过滤和要去掉的响应头。`$document` 关掉。
  HtmlFilterResult HtmlFilters(const GURL& url, uint32_t doc_flags) const;

  // 不限网站的元素隐藏（每个网页进程送一次，见 content_filter.mojom）。
  const std::vector<std::string>& generic_selectors() const;
  // generic_selectors() 的散列：网页进程按它认出是不是同一份。
  uint64_t generic_version() const;

  size_t network_rule_count() const;
  size_t cosmetic_rule_count() const;
  size_t scriptlet_rule_count() const;
  size_t rule_count() const {
    return network_rule_count() + cosmetic_rule_count() +
           scriptlet_rule_count();
  }
  bool empty() const { return rule_count() == 0; }
  // 进程里每建一个引擎加一（缓存用：换了引擎就重新算）。
  uint64_t id() const { return id_; }

 private:
  friend class base::RefCountedThreadSafe<FilterEngine>;
  struct Data;

  explicit FilterEngine(std::unique_ptr<Data> data);
  ~FilterEngine();

  std::unique_ptr<Data> data_;
  const uint64_t id_;
};

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_FILTER_ENGINE_H_
