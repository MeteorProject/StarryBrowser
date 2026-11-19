// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_HTML_FILTER_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_HTML_FILTER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

class GURL;

namespace starry::content_filter {

class HtmlDocument;

namespace internal {
struct HtmlProgram;
}  // namespace internal

// HTML 过滤（uBlock Origin 的 `网站##^选择器`）：浏览器
// 在页面的 HTML 交给网页进程之前找出元素，从源码里删掉。网页的脚本、解析器都看不到它们
// （`<script>` 不会跑，图片、iframe 不会加载）。
//
// 一条规则编好的样子。选择器的写法同扩展选择器（procedural_selector.h），只是在源码上找：
// 原生的 CSS（css_selector.h）、`:has-text()`、`:has()`、`:not()`、`:upward()`、
// `:min-text-length()`、`:matches-attr()`、`:matches-path()`。
class HtmlSelector {
 public:
  // spec 是 CompileProceduralSelector 编出的 JSON。源码上做不到的（`:matches-css()` 这类要
  // 排版的、`:matches-media()`、`:xpath()`、`:others()`、浏览器才认的伪类）返回 nullptr。
  static std::unique_ptr<HtmlSelector> Compile(std::string_view spec);

  HtmlSelector(const HtmlSelector&) = delete;
  HtmlSelector& operator=(const HtmlSelector&) = delete;
  ~HtmlSelector();

  // 找到的元素（HtmlDocument 的编号，文档顺序）。url 是页面的网址（`:matches-path()` 用）。
  std::vector<uint32_t> Select(const HtmlDocument& doc, const GURL& url) const;

 private:
  explicit HtmlSelector(std::unique_ptr<internal::HtmlProgram> program);

  std::unique_ptr<internal::HtmlProgram> program_;
};

// 编码的名字（响应头的 charset、`<meta>` 里的）改成 ICU 认的（WHATWG 的别名：GBK 一类是
// gb18030，Latin-1、ASCII 是 windows-1252）。空的得到空的。不兼容 ASCII 的（UTF-16、
// ISO-2022-JP）返回 false。
bool NormalizeCharset(std::string_view label, std::string* charset);

// 页面的编码（BOM、响应头的 charset、开头的 `<meta>`，同浏览器的顺序），写成 ICU 认的名字。
// 不兼容 ASCII 的（UTF-16、ISO-2022-JP）返回 false：标签不能按字节认，不过滤。
bool DetectHtmlCharset(std::string_view html,
                       std::string_view header_charset,
                       std::string* charset);

// 用 selectors 改写页面的 HTML：找到的元素（连同里面的）从源码里删掉，别的地方一个字节
// 都不动（根元素 html 不删）。header_charset 是响应头里的（可以是空的）。没有要删的、编码
// 不支持时返回 nullopt。removed 是删掉了几段。
std::optional<std::string> FilterHtml(
    std::string_view html,
    std::string_view header_charset,
    const GURL& url,
    base::span<const HtmlSelector* const> selectors,
    size_t* removed = nullptr);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_HTML_FILTER_H_
