// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_RESPONSE_FILTER_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_RESPONSE_FILTER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class GURL;

namespace re2 {
class RE2;
}

namespace starry::content_filter {

class HtmlSelector;

// `||网站/路径$replace=/正则/替换/标志`（uBlock Origin 的 replace=，
// 「改写响应」）：把响应体里匹配的文字换掉，同 JS 的 `文字.replace(new RegExp(正则, 标志),
// 替换)`（`g` 换全部，否则只换第一处；替换里的 `$1`、`$&`、`` $` ``、`$'`、`$<名字>`、`$$`）。
//
// 正则先用 RE2（线性时间）；RE2 做不到的（前瞻、后顾、反向引用、重复超过 1000 次）用 ICU
// 的回溯引擎，一次最多跑 kIcuTimeBudget，超时这一条不改。
class ReplaceFilter {
 public:
  // value 是 `replace=` 后面的写法，分开的规则同 uBlock Origin（parseReplaceValue）：
  // `/` 分开三段，`\/` 是 `/` 本身；一段整个用引号括起来时引号去掉；两头的空白去掉；`\$`、
  // `\,` 是 `$`、`,`。标志能用 g、i、m、s（u、d 不影响）。写错的、编不了的返回 nullptr。
  static std::unique_ptr<ReplaceFilter> Parse(std::string_view value);

  ReplaceFilter(const ReplaceFilter&) = delete;
  ReplaceFilter& operator=(const ReplaceFilter&) = delete;
  ~ReplaceFilter();

  // 改 text（UTF-8）。换了（至少匹配到一处）返回 true；没匹配到、ICU 超时返回 false，
  // text 不动。
  bool Apply(std::string* text) const;

  // 规则里的写法（`replace=` 后面的）。
  const std::string& source() const { return source_; }
  // 用的是 ICU（RE2 做不到）。
  bool uses_icu() const { return icu_ != nullptr; }

 private:
  struct Span {
    size_t begin;
    size_t end;
  };
  // ICU 的 icu::RegexPattern（不在头文件里引 ICU）。
  struct IcuPattern;

  ReplaceFilter();

  // 一处匹配：groups[0] 是整个，没参与匹配的分组 begin 是 npos。
  void AppendReplacement(std::string_view text,
                         const std::vector<Span>& groups,
                         std::string* out) const;
  // 命名分组的编号，没有时 0。
  int GroupNumber(std::string_view name) const;
  bool ApplyLiteral(std::string* text) const;
  bool ApplyRe2(std::string* text) const;
  bool ApplyIcu(std::string* text) const;

  std::string source_;
  std::string replacement_;
  bool global_ = false;
  // 替换里有 `$数字`、`$<`：要取分组。
  bool needs_groups_ = false;
  bool named_groups_ = false;
  int group_count_ = 0;
  // 三种只有一种：字面的文字（没有正则写法、没有 i 标志）、RE2、ICU。
  std::string literal_;
  std::unique_ptr<re2::RE2> re2_;
  std::unique_ptr<IcuPattern> icu_;
};

// 一个响应要做的改写：先按顺序做 `$replace`，页面再做 HTML 过滤（同 uBlock Origin）。
// 指着引擎里的规则：用完之前要拿着引擎。
struct ResponseFilters {
  ResponseFilters();
  ResponseFilters(ResponseFilters&&);
  ResponseFilters& operator=(ResponseFilters&&);
  ~ResponseFilters();

  bool empty() const { return replace.empty() && html.empty(); }

  std::vector<const ReplaceFilter*> replace;
  std::vector<const HtmlSelector*> html;
};

struct ResponseFilterStats {
  // 换了的 `$replace` 条数、HTML 过滤删掉的元素。
  size_t replaced = 0;
  size_t removed = 0;
};

// 能改的响应（同 uBlock Origin）：`text/` 开头的（`text/event-stream` 这种一直不结束的
// 除外），JS、JSON、XML、XHTML、m3u8 播放列表、DASH 清单；没有类型的当作文字。
bool IsFilterableResponseMime(std::string_view mime);

// 改写一个响应体。html 是页面（按 HTML 的规则定编码：BOM、响应头、`<meta>`）；别的按 BOM、
// 响应头，都没有时是合法的 UTF-8 就当 UTF-8，否则 windows-1252。不是 UTF-8 的解码成 UTF-8
// 再换，换完编回原来的编码（编不回去时不改）。没改、编码不支持（UTF-16 这类）时返回
// nullopt。
std::optional<std::string> FilterResponseBody(std::string_view body,
                                              std::string_view header_charset,
                                              bool html,
                                              const GURL& url,
                                              const ResponseFilters& filters,
                                              ResponseFilterStats* stats);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_RESPONSE_FILTER_H_
