// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_HTML_DOCUMENT_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_HTML_DOCUMENT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace starry::content_filter {

// HTML 过滤（html_filter.h）用的轻量的文档树。按 HTML 规范的分词（script、style 这些的原始
// 文本，script 里的 `<!--`，注释、字符引用）和建树的主要规则（没写的 html、head、body，自动
// 结束的 p、li、td、option，空元素，`</head>` 后面的 script 归 head，template 的内容不算 DOM
// 树里的）把源码解析成元素树，记下每个元素在源码里的范围：删元素就是删这段源码，别的地方
// 一个字节都不动。表格的 foster parenting、错误嵌套的格式元素（adoption agency）只近似。
//
// 源码当作兼容 ASCII 的编码（UTF-8、GBK、Shift_JIS、windows-1252……）处理，标签、属性名都是
// ASCII；不是 UTF-8 时属性值、文字按 charset 转成 UTF-8 再比较。
class HtmlDocument {
 public:
  using NodeId = uint32_t;
  static constexpr NodeId kNone = UINT32_MAX;
  // 第 0 个是文档自己，后面是元素，按文档顺序（先序）编号：一个元素里面的元素是编号在
  // (id, subtree_end) 的那些。
  static constexpr NodeId kDocument = 0;

  struct Attribute {
    // 小写。
    std::string name;
    // 解开了字符引用，UTF-8。
    std::string value;
  };

  struct Node {
    Node();
    Node(Node&&);
    Node& operator=(Node&&);
    ~Node();

    // 小写；文档是空的。
    std::string tag;
    std::vector<Attribute> attributes;
    NodeId parent = kNone;
    NodeId prev_sibling = kNone;
    NodeId next_sibling = kNone;
    NodeId first_child = kNone;
    NodeId subtree_end = 0;
    // 源码里的范围：开始标签的 `<` 到结束标签的 `>` 后面；没写结束标签的到被结束的地方
    // （下一个兄弟、上一级的结束标签、文件末尾）。没写标签的 html、head、body 从里面的
    // 第一个内容开始。
    size_t begin = 0;
    size_t end = 0;
    // textContent（原来的编码）在 text_ 里的范围。
    size_t text_begin = 0;
    size_t text_end = 0;
    // 在 <template> 的内容里：不算 DOM 树里的元素，选择器找不到。
    bool in_template = false;
  };

  // charset 是 ICU 认的编码名字（html_filter.cc 的 DetectCharset），空的或 "utf-8" 是 UTF-8。
  HtmlDocument(std::string_view html, std::string_view charset);
  HtmlDocument(const HtmlDocument&) = delete;
  HtmlDocument& operator=(const HtmlDocument&) = delete;
  ~HtmlDocument();

  size_t size() const { return nodes_.size(); }
  const Node& node(NodeId id) const { return nodes_[id]; }
  // DOM 树里的元素（不是文档自己、不在 template 的内容里）。
  bool IsElement(NodeId id) const {
    return id != kDocument && id < nodes_.size() && !nodes_[id].in_template;
  }
  // 上一级元素（同 parentElement：html 的是 kNone）。
  NodeId ParentElement(NodeId id) const;
  // 根元素（html），没有时是 kNone。
  NodeId root() const { return root_; }
  // 属性值（UTF-8），没有这个属性时是 nullptr。name 是小写。
  const std::string* AttributeValue(NodeId id, std::string_view name) const;
  // textContent，UTF-8（同 DOM：文档的是空的）。storage 在不是 UTF-8 时放转好的文字。
  std::string_view Text(NodeId id, std::string* storage) const;

  std::string_view source() const { return source_; }

 private:
  friend class HtmlTreeBuilder;

  const std::string_view source_;
  const std::string charset_;
  const bool utf8_;
  std::vector<Node> nodes_;
  std::string text_;
  NodeId root_ = kNone;
};

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_HTML_DOCUMENT_H_
