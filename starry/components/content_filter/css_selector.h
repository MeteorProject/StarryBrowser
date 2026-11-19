// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_CSS_SELECTOR_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_CSS_SELECTOR_H_

#include <memory>
#include <string_view>
#include <vector>

#include "starry/components/content_filter/html_document.h"

namespace starry::content_filter {

// CSS 选择器，在 HtmlDocument 上找元素（HTML 过滤用，html_filter.h）。支持过滤列表里用的：
// 类型、`*`、`#id`、`.class`、属性（`=`、`~=`、`|=`、`^=`、`$=`、`*=`，`i`、`s`），组合符
// （空白、`>`、`+`、`~`），`:not()`、`:is()`、`:where()`、`:has()`（相对的选择器），
// `:nth-child()` 这一类、`:first-child`、`:only-of-type` 这一类，`:empty`、`:root`、`:scope`。
// 要排版、交互状态的伪类（`:hover`、`:checked`、`:visited`）、伪元素、命名空间不支持
// （Parse 失败）。
class CssSelector {
 public:
  using NodeId = HtmlDocument::NodeId;

  // relative：相对的选择器（`:has()` 里的、扩展选择器里接着的 `> span`、`+ span`），开头的
  // 组合符省略时是空白（里面的）。
  static std::unique_ptr<CssSelector> Parse(std::string_view text,
                                            bool relative);

  CssSelector(const CssSelector&) = delete;
  CssSelector& operator=(const CssSelector&) = delete;
  ~CssSelector();

  // element 符不符合。相对的选择器相对于 scope（`:scope` 也是它）。
  bool Matches(const HtmlDocument& doc, NodeId element, NodeId scope) const;
  // 找出符合的元素，按文档的顺序追加到 out。scope 是文档时找整个文档；相对的从 scope 找
  // （里面的、孩子、后面的兄弟），不是相对的找 scope 里面的。
  void Query(const HtmlDocument& doc,
             NodeId scope,
             std::vector<NodeId>* out) const;
  // `:has()`：有没有相对于 anchor 符合的元素。
  bool HasRelative(const HtmlDocument& doc, NodeId anchor) const;

  bool relative() const { return relative_; }

 private:
  struct Attribute;
  struct Pseudo;
  struct Compound;
  struct Complex;
  class Parser;

  explicit CssSelector(bool relative);

  bool MatchComplex(const HtmlDocument& doc,
                    const Complex& complex,
                    size_t index,
                    NodeId element,
                    NodeId scope) const;
  bool MatchCompound(const HtmlDocument& doc,
                     const Compound& compound,
                     NodeId element,
                     NodeId scope) const;
  bool MatchPseudo(const HtmlDocument& doc,
                   const Pseudo& pseudo,
                   NodeId element,
                   NodeId scope) const;

  const bool relative_;
  std::vector<Complex> selectors_;
};

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_CSS_SELECTOR_H_
