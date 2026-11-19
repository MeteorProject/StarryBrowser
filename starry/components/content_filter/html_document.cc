// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/html_document.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "base/i18n/icu_string_conversions.h"
#include "base/memory/raw_ref.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"

namespace starry::content_filter {

namespace {

using NodeId = HtmlDocument::NodeId;
using Attribute = HtmlDocument::Attribute;
constexpr NodeId kNone = HtmlDocument::kNone;
constexpr size_t kNpos = std::string_view::npos;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool OneOf(std::string_view name,
           std::initializer_list<std::string_view> names) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

bool IsVoid(std::string_view tag) {
  return OneOf(tag, {"area", "base", "basefont", "bgsound", "br", "col",
                     "embed", "frame", "hr", "img", "input", "keygen", "link",
                     "meta", "param", "source", "track", "wbr"});
}

// 写在 head 里的元素（在 head 外面、body 前面出现时也放进 head）。
bool IsHeadContent(std::string_view tag) {
  return OneOf(tag, {"base", "basefont", "bgsound", "link", "meta",
                     "noframes", "noscript", "script", "style", "template",
                     "title"});
}

bool IsHeading(std::string_view tag) {
  return tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6';
}

// 开始标签会先结束没结束的 <p>。
bool ClosesParagraph(std::string_view tag) {
  return IsHeading(tag) ||
         OneOf(tag, {"address", "article", "aside", "blockquote", "center",
                     "dd", "details", "dialog", "dir", "div", "dl", "dt",
                     "fieldset", "figcaption", "figure", "footer", "form",
                     "header", "hgroup", "hr", "li", "listing", "main", "menu",
                     "nav", "ol", "p", "plaintext", "pre", "search", "section",
                     "summary", "table", "ul", "xmp"});
}

// 规范里的 special 元素：一般的结束标签找不过它们。
bool IsSpecial(std::string_view tag) {
  return IsHeading(tag) ||
         OneOf(tag,
               {"address", "applet",   "area",       "article",  "aside",
                "base",    "basefont", "bgsound",    "blockquote", "body",
                "br",      "button",   "caption",    "center",   "col",
                "colgroup", "dd",      "details",    "dir",      "div",
                "dl",      "dt",       "embed",      "fieldset", "figcaption",
                "figure",  "footer",   "form",       "frame",    "frameset",
                "head",    "header",   "hgroup",     "hr",       "html",
                "iframe",  "img",      "input",      "keygen",   "li",
                "link",    "listing",  "main",       "marquee",  "menu",
                "meta",    "nav",      "noembed",    "noframes", "noscript",
                "object",  "ol",       "p",          "param",    "plaintext",
                "pre",     "script",   "search",     "section",  "select",
                "source",  "style",    "summary",    "table",    "tbody",
                "td",      "template", "textarea",   "tfoot",    "th",
                "thead",   "title",    "tr",         "track",    "ul",
                "wbr",     "xmp"});
}

bool IsTablePart(std::string_view tag) {
  return OneOf(tag, {"caption", "colgroup", "table", "tbody", "td", "tfoot",
                     "th", "thead", "tr"});
}

// 在 svg、math 里遇到这些开始标签时回到 HTML。
bool BreaksOutOfForeign(std::string_view tag) {
  return IsHeading(tag) ||
         OneOf(tag, {"b",    "big",   "blockquote", "body",   "br",
                     "center", "code", "dd",        "div",    "dl",
                     "dt",   "em",    "embed",      "head",   "hr",
                     "i",    "img",   "li",         "listing", "menu",
                     "meta", "nobr",  "ol",         "p",      "pre",
                     "ruby", "s",     "small",      "span",   "strong",
                     "strike", "sub", "sup",        "table",  "tt",
                     "u",    "ul",    "var"});
}

enum class Scope { kDefault, kButton, kListItem, kTable };

bool IsScopeBoundary(std::string_view tag, Scope scope) {
  if (scope == Scope::kTable) {
    return OneOf(tag, {"html", "table", "template"});
  }
  if (OneOf(tag, {"applet", "caption", "html", "table", "td", "th", "marquee",
                  "object", "template"})) {
    return true;
  }
  return (scope == Scope::kButton && tag == "button") ||
         (scope == Scope::kListItem && (tag == "ol" || tag == "ul"));
}

enum class Content {
  kNormal,
  kScript,
  kRawText,
  kEscapableRawText,
  kPlaintext
};

Content ContentOf(std::string_view tag) {
  if (tag == "script") {
    return Content::kScript;
  }
  if (OneOf(tag,
            {"style", "xmp", "iframe", "noembed", "noframes", "noscript"})) {
    return Content::kRawText;
  }
  if (tag == "textarea" || tag == "title") {
    return Content::kEscapableRawText;
  }
  return tag == "plaintext" ? Content::kPlaintext : Content::kNormal;
}

struct NamedEntity {
  std::string_view name;
  uint32_t code;
};

// 常见的几个（完整的表有两千多个，选择器、文字里用得到的就这些）。
constexpr NamedEntity kEntities[] = {
    {"amp", '&'},        {"lt", '<'},          {"gt", '>'},
    {"quot", '"'},       {"apos", '\''},       {"nbsp", 0xA0},
    {"copy", 0xA9},      {"reg", 0xAE},        {"trade", 0x2122},
    {"hellip", 0x2026},  {"mdash", 0x2014},    {"ndash", 0x2013},
    {"lsquo", 0x2018},   {"rsquo", 0x2019},    {"ldquo", 0x201C},
    {"rdquo", 0x201D},   {"laquo", 0xAB},      {"raquo", 0xBB},
    {"middot", 0xB7},    {"bull", 0x2022},     {"times", 0xD7},
    {"divide", 0xF7},    {"deg", 0xB0},        {"euro", 0x20AC},
    {"pound", 0xA3},     {"yen", 0xA5},        {"cent", 0xA2},
    {"sect", 0xA7},      {"para", 0xB6},       {"plusmn", 0xB1},
    {"ensp", 0x2002},    {"emsp", 0x2003},     {"thinsp", 0x2009},
    {"zwnj", 0x200C},    {"zwj", 0x200D},      {"shy", 0xAD},
};

// 解开字符引用。utf8 是 false 时（源码不是 UTF-8）只解出 ASCII 的，别的照留。
void DecodeEntities(std::string_view in, bool utf8, std::string* out) {
  size_t i = 0;
  while (i < in.size()) {
    const size_t amp = in.find('&', i);
    if (amp == kNpos) {
      out->append(in.substr(i));
      return;
    }
    out->append(in.substr(i, amp - i));
    i = amp + 1;
    uint32_t code = 0;
    size_t end = i;
    if (end < in.size() && in[end] == '#') {
      ++end;
      const bool hex = end < in.size() && (in[end] == 'x' || in[end] == 'X');
      if (hex) {
        ++end;
      }
      const size_t digits = end;
      uint32_t value = 0;
      while (end < in.size() && (hex ? base::IsHexDigit(in[end])
                                     : base::IsAsciiDigit(in[end]))) {
        value = std::min<uint32_t>(
            value * (hex ? 16 : 10) + base::HexDigitToInt(in[end]), 0x110000);
        ++end;
      }
      if (end == digits) {
        out->push_back('&');
        continue;
      }
      if (end < in.size() && in[end] == ';') {
        ++end;
      }
      code = value == 0 || value > 0x10FFFF ||
                     (value >= 0xD800 && value <= 0xDFFF)
                 ? 0xFFFD
                 : value;
    } else {
      size_t name_end = end;
      while (name_end < in.size() && base::IsAsciiAlphaNumeric(in[name_end])) {
        ++name_end;
      }
      if (name_end == end || name_end >= in.size() || in[name_end] != ';') {
        out->push_back('&');
        continue;
      }
      const std::string_view name = in.substr(end, name_end - end);
      for (const NamedEntity& entity : kEntities) {
        if (entity.name == name) {
          code = entity.code;
          break;
        }
      }
      if (code == 0) {
        out->push_back('&');
        continue;
      }
      end = name_end + 1;
    }
    if (code >= 0x80 && !utf8) {
      out->append(in.substr(amp, end - amp));
    } else {
      base::WriteUnicodeCharacter(static_cast<base_icu::UChar32>(code), out);
    }
    i = end;
  }
}

std::string ToUtf8(std::string_view text, const std::string& charset) {
  std::u16string utf16;
  if (!base::CodepageToUTF16(text, charset.c_str(),
                             base::OnStringConversionError::SUBSTITUTE,
                             &utf16)) {
    return std::string(text);
  }
  return base::UTF16ToUTF8(utf16);
}

}  // namespace

// 分词加建树，一遍做完（HTML 规范的 tokenization、tree construction 的主要部分）。
class HtmlTreeBuilder {
 public:
  explicit HtmlTreeBuilder(HtmlDocument* doc)
      : doc_(*doc), source_(doc->source_) {}

  void Run();

 private:
  struct Tag {
    std::string name;
    std::vector<Attribute> attributes;
    bool self_closing = false;
    // `>` 的后面。
    size_t end = 0;
  };

  enum class Mode { kBeforeHtml, kBeforeHead, kInHead, kAfterHead, kInBody };

  NodeId current() const { return stack_.back(); }
  const std::string& current_tag() const { return nodes_[stack_.back()].tag; }

  // 下面的函数都从 source_[lt] == '<' 开始，返回接着读的位置。
  size_t Markup(size_t lt);
  size_t SkipComment(size_t lt) const;
  size_t SkipBogusComment(size_t from) const;
  size_t StartTag(size_t lt);
  size_t EndTag(size_t lt);
  // i 是标签名的开头。文件在标签中间结束时返回 false。
  bool ParseTag(size_t i, bool want_attributes, Tag* tag);
  std::string DecodeAttribute(std::string_view raw) const;
  bool IsEndTagAt(size_t i, std::string_view tag) const;
  size_t FindRawTextEnd(size_t from, std::string_view tag) const;
  size_t FindScriptEnd(size_t from) const;

  size_t HandleStart(Tag tag, size_t begin);
  void HandleEnd(std::string_view name, size_t begin, size_t end);
  size_t Insert(Tag tag, size_t begin);
  void Text(size_t begin, size_t end);
  void AppendText(size_t begin, size_t end, bool decode);

  NodeId Create(std::string_view tag,
                std::vector<Attribute> attributes,
                size_t begin);
  void Push(NodeId id);
  void Pop(size_t end);
  // 结束栈上第 index 个：上面的（没写结束标签）在 begin 结束，它自己在 end。
  void PopTo(size_t index, size_t begin, size_t end);
  size_t FindInScope(std::string_view tag, Scope scope) const;
  size_t FindHeadingInScope() const;
  void ClosePIfInButtonScope(size_t at);
  // 从上往下找 close 里的元素，在 stop 里的元素之前找到就结束到那里，再找。
  void CloseUntilContext(std::initializer_list<std::string_view> close,
                         std::initializer_list<std::string_view> stop,
                         size_t at);
  // 开始标签隐含的结束（<li> 结束上一个 <li>、<td> 结束上一个 <td>……）。
  void CloseImplied(std::string_view tag, size_t begin);
  void MergeAttributes(NodeId id, std::vector<Attribute> attributes);
  void EnsureHtml(size_t at);
  void EnsureHead(size_t at);
  void CloseHead(size_t begin, size_t end);
  void EnsureBody(size_t at);

  const raw_ref<HtmlDocument> doc_;
  const std::string_view source_;
  // 建好以后移给文档。
  std::vector<HtmlDocument::Node> nodes_;
  std::string text_;
  std::vector<NodeId> last_child_;
  std::vector<NodeId> stack_;
  Mode mode_ = Mode::kBeforeHtml;
  NodeId html_id_ = kNone;
  NodeId head_id_ = kNone;
  NodeId body_id_ = kNone;
  int template_depth_ = 0;
  int foreign_depth_ = 0;
  // 最后一个内容（不是空白、注释、`</body>`、`</html>`）的后面：到文件末尾还没结束的元素
  // 在这里结束，删掉时留着后面的 `</body></html>`。
  size_t content_end_ = 0;
};

void HtmlTreeBuilder::Run() {
  nodes_.reserve(source_.size() / 64 + 16);
  text_.reserve(source_.size() / 2);
  nodes_.emplace_back();
  last_child_.push_back(kNone);
  stack_.push_back(HtmlDocument::kDocument);
  const size_t n = source_.size();
  size_t pos = source_.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  while (pos < n) {
    const size_t lt = source_.find('<', pos);
    if (lt == kNpos) {
      Text(pos, n);
      break;
    }
    if (lt > pos) {
      Text(pos, lt);
    }
    pos = Markup(lt);
  }
  while (stack_.size() > 1) {
    Pop(content_end_);
  }
  nodes_[0].end = n;
  nodes_[0].text_end = text_.size();
  for (size_t id = nodes_.size(); id-- > 0;) {
    NodeId end = static_cast<NodeId>(id + 1);
    for (NodeId child = nodes_[id].first_child; child != kNone;
         child = nodes_[child].next_sibling) {
      end = std::max(end, nodes_[child].subtree_end);
    }
    nodes_[id].subtree_end = end;
  }
  doc_->nodes_ = std::move(nodes_);
  doc_->text_ = std::move(text_);
}

size_t HtmlTreeBuilder::Markup(size_t lt) {
  const size_t n = source_.size();
  if (lt + 1 >= n) {
    Text(lt, n);
    return n;
  }
  const char c = source_[lt + 1];
  if (c == '!') {
    if (source_.compare(lt, 4, "<!--") == 0) {
      return SkipComment(lt);
    }
    // <!DOCTYPE>、<![CDATA[…]]> 这类。
    return SkipBogusComment(lt + 2);
  }
  if (c == '?') {
    return SkipBogusComment(lt + 1);
  }
  if (c == '/') {
    if (lt + 2 >= n) {
      Text(lt, n);
      return n;
    }
    const char d = source_[lt + 2];
    if (base::IsAsciiAlpha(d)) {
      return EndTag(lt);
    }
    return d == '>' ? lt + 3 : SkipBogusComment(lt + 2);
  }
  if (base::IsAsciiAlpha(c)) {
    return StartTag(lt);
  }
  Text(lt, lt + 1);
  return lt + 1;
}

size_t HtmlTreeBuilder::SkipComment(size_t lt) const {
  const size_t start = lt + 4;
  // `<!-->`、`<!--->` 是空的注释。
  if (source_.compare(start, 1, ">") == 0) {
    return start + 1;
  }
  if (source_.compare(start, 2, "->") == 0) {
    return start + 2;
  }
  for (size_t i = source_.find("--", start); i != kNpos;
       i = source_.find("--", i + 1)) {
    if (source_.compare(i + 2, 1, ">") == 0) {
      return i + 3;
    }
    if (source_.compare(i + 2, 2, "!>") == 0) {
      return i + 4;
    }
  }
  return source_.size();
}

size_t HtmlTreeBuilder::SkipBogusComment(size_t from) const {
  const size_t gt = source_.find('>', from);
  return gt == kNpos ? source_.size() : gt + 1;
}

size_t HtmlTreeBuilder::StartTag(size_t lt) {
  Tag tag;
  if (!ParseTag(lt + 1, /*want_attributes=*/true, &tag)) {
    // 文件末尾的半个标签：丢掉（同规范）。
    return source_.size();
  }
  content_end_ = HandleStart(std::move(tag), lt);
  return content_end_;
}

size_t HtmlTreeBuilder::EndTag(size_t lt) {
  Tag tag;
  if (!ParseTag(lt + 2, /*want_attributes=*/false, &tag)) {
    return source_.size();
  }
  HandleEnd(tag.name, lt, tag.end);
  if (tag.name != "body" && tag.name != "html") {
    content_end_ = tag.end;
  }
  return tag.end;
}

bool HtmlTreeBuilder::ParseTag(size_t i, bool want_attributes, Tag* tag) {
  const std::string_view s = source_;
  const size_t n = s.size();
  const size_t name_start = i;
  while (i < n && !IsSpace(s[i]) && s[i] != '/' && s[i] != '>') {
    ++i;
  }
  tag->name = base::ToLowerASCII(s.substr(name_start, i - name_start));
  for (;;) {
    // 属性之间的空白；`/` 后面不是 `>` 时不算。
    while (i < n && (IsSpace(s[i]) ||
                     (s[i] == '/' && (i + 1 >= n || s[i + 1] != '>')))) {
      ++i;
    }
    if (i >= n) {
      return false;
    }
    if (s[i] == '>') {
      tag->end = i + 1;
      return true;
    }
    if (s[i] == '/') {
      tag->self_closing = true;
      tag->end = i + 2;
      return true;
    }
    // 属性名（第一个字可以是 `=`）。
    const size_t attr_start = i++;
    while (i < n && !IsSpace(s[i]) && s[i] != '/' && s[i] != '>' &&
           s[i] != '=') {
      ++i;
    }
    const std::string_view name = s.substr(attr_start, i - attr_start);
    while (i < n && IsSpace(s[i])) {
      ++i;
    }
    std::string_view raw;
    if (i < n && s[i] == '=') {
      ++i;
      while (i < n && IsSpace(s[i])) {
        ++i;
      }
      if (i >= n) {
        return false;
      }
      if (s[i] == '"' || s[i] == '\'') {
        const size_t close = s.find(s[i], i + 1);
        if (close == kNpos) {
          return false;
        }
        raw = s.substr(i + 1, close - i - 1);
        i = close + 1;
      } else {
        const size_t value_start = i;
        while (i < n && !IsSpace(s[i]) && s[i] != '>') {
          ++i;
        }
        raw = s.substr(value_start, i - value_start);
      }
    }
    if (!want_attributes) {
      continue;
    }
    std::string lower = base::ToLowerASCII(name);
    // 重复的属性只要第一个。
    if (std::none_of(tag->attributes.begin(), tag->attributes.end(),
                     [&](const Attribute& attribute) {
                       return attribute.name == lower;
                     })) {
      tag->attributes.push_back({std::move(lower), DecodeAttribute(raw)});
    }
  }
}

std::string HtmlTreeBuilder::DecodeAttribute(std::string_view raw) const {
  std::string value;
  DecodeEntities(raw, doc_->utf8_, &value);
  if (!doc_->utf8_ && !base::IsStringASCII(value)) {
    return ToUtf8(value, doc_->charset_);
  }
  return value;
}

// source_[i] 开始是 `</tag` 加上空白、`/` 或 `>`（不分大小写）。
bool HtmlTreeBuilder::IsEndTagAt(size_t i, std::string_view tag) const {
  const size_t after = i + 2 + tag.size();
  if (after >= source_.size() || source_.compare(i, 2, "</") != 0 ||
      !base::EqualsCaseInsensitiveASCII(source_.substr(i + 2, tag.size()),
                                        tag)) {
    return false;
  }
  const char c = source_[after];
  return IsSpace(c) || c == '/' || c == '>';
}

size_t HtmlTreeBuilder::FindRawTextEnd(size_t from,
                                       std::string_view tag) const {
  for (size_t i = source_.find("</", from); i != kNpos;
       i = source_.find("</", i + 2)) {
    if (IsEndTagAt(i, tag)) {
      return i;
    }
  }
  return kNpos;
}

// script 的内容在哪里结束：`<!--` 里面的 `<script>…</script>` 不算（规范的 script data
// escaped / double escaped 状态）。
size_t HtmlTreeBuilder::FindScriptEnd(size_t from) const {
  enum class State { kData, kEscaped, kDoubleEscaped };
  State state = State::kData;
  const size_t n = source_.size();
  size_t i = from;
  while (i < n) {
    i = source_.find_first_of("<-", i);
    if (i == kNpos) {
      break;
    }
    if (source_[i] == '-') {
      if (state != State::kData && source_.compare(i, 3, "-->") == 0) {
        state = State::kData;
        i += 3;
      } else {
        ++i;
      }
      continue;
    }
    switch (state) {
      case State::kData:
        if (source_.compare(i, 4, "<!--") == 0) {
          i += 4;
          while (i < n && source_[i] == '-') {
            ++i;
          }
          // `<!-->`、`<!--->`：马上结束了。
          if (i < n && source_[i] == '>') {
            ++i;
          } else {
            state = State::kEscaped;
          }
          continue;
        }
        if (IsEndTagAt(i, "script")) {
          return i;
        }
        break;
      case State::kEscaped:
        if (IsEndTagAt(i, "script")) {
          return i;
        }
        if (i + 7 < n &&
            base::EqualsCaseInsensitiveASCII(source_.substr(i, 7), "<script") &&
            (IsSpace(source_[i + 7]) || source_[i + 7] == '/' ||
             source_[i + 7] == '>')) {
          state = State::kDoubleEscaped;
          i += 7;
          continue;
        }
        break;
      case State::kDoubleEscaped:
        if (IsEndTagAt(i, "script")) {
          state = State::kEscaped;
          i += 8;
          continue;
        }
        break;
    }
    ++i;
  }
  return kNpos;
}

size_t HtmlTreeBuilder::HandleStart(Tag tag, size_t begin) {
  const std::string name = tag.name;
  if (template_depth_ > 0) {
    if (OneOf(name, {"html", "head", "body", "frameset"})) {
      return tag.end;
    }
    CloseImplied(name, begin);
    return Insert(std::move(tag), begin);
  }
  if (foreign_depth_ > 0) {
    if (!BreaksOutOfForeign(name)) {
      return Insert(std::move(tag), begin);
    }
    while (foreign_depth_ > 0 && stack_.size() > 1) {
      Pop(begin);
    }
  }
  if (name == "html") {
    if (html_id_ == kNone) {
      html_id_ = Create(name, std::move(tag.attributes), begin);
      Push(html_id_);
      doc_->root_ = html_id_;
      mode_ = Mode::kBeforeHead;
    } else {
      MergeAttributes(html_id_, std::move(tag.attributes));
    }
    return tag.end;
  }
  if (name == "head") {
    if (mode_ == Mode::kBeforeHtml || mode_ == Mode::kBeforeHead) {
      EnsureHtml(begin);
      head_id_ = Create(name, std::move(tag.attributes), begin);
      Push(head_id_);
      mode_ = Mode::kInHead;
    }
    return tag.end;
  }
  if (name == "body" || name == "frameset") {
    if (body_id_ == kNone) {
      EnsureHead(begin);
      CloseHead(begin, begin);
      body_id_ = Create("body", std::move(tag.attributes), begin);
      Push(body_id_);
      mode_ = Mode::kInBody;
    } else {
      MergeAttributes(body_id_, std::move(tag.attributes));
    }
    return tag.end;
  }
  if (mode_ != Mode::kInBody && IsHeadContent(name)) {
    if (mode_ != Mode::kAfterHead) {
      EnsureHead(begin);
      return Insert(std::move(tag), begin);
    }
    // `</head>` 后面、<body> 前面的：规范是放进 head。
    stack_.push_back(head_id_);
    const size_t next = Insert(std::move(tag), begin);
    stack_.erase(std::find(stack_.begin(), stack_.end(), head_id_));
    nodes_[head_id_].text_end = text_.size();
    return next;
  }
  if (mode_ != Mode::kInBody) {
    EnsureBody(begin);
  }
  CloseImplied(name, begin);
  // 表格里没写的 tbody、tr（同浏览器：`table > tbody > tr > td`）。
  if (name == "tr" || name == "td" || name == "th") {
    if (current_tag() == "table") {
      Push(Create("tbody", {}, begin));
    }
    if (name != "tr" &&
        OneOf(current_tag(), {"tbody", "thead", "tfoot"})) {
      Push(Create("tr", {}, begin));
    }
  }
  return Insert(std::move(tag), begin);
}

void HtmlTreeBuilder::HandleEnd(std::string_view name,
                                size_t begin,
                                size_t end) {
  if (name == "template") {
    if (template_depth_ == 0) {
      return;
    }
    for (size_t i = stack_.size(); i-- > 1;) {
      if (nodes_[stack_[i]].tag == "template") {
        PopTo(i, begin, end);
        return;
      }
    }
    return;
  }
  if (foreign_depth_ > 0) {
    // svg、math 里：从上往下找同名的，到 svg、math 为止。
    for (size_t i = stack_.size(); i-- > 1;) {
      const std::string& tag = nodes_[stack_[i]].tag;
      if (tag == name) {
        PopTo(i, begin, end);
        return;
      }
      if (tag == "svg" || tag == "math") {
        break;
      }
    }
  }
  if (OneOf(name, {"head", "body", "html"})) {
    if (template_depth_ > 0) {
      return;
    }
    if (name == "head") {
      CloseHead(begin, end);
    } else if (mode_ != Mode::kInBody) {
      // body、html 不结束（后面的内容规范上也放进 body）。
      EnsureBody(begin);
    }
    return;
  }
  if (name == "br") {
    return;
  }
  if (IsSpecial(name)) {
    size_t i = kNpos;
    if (IsHeading(name)) {
      i = FindHeadingInScope();
    } else {
      i = FindInScope(name, name == "p"           ? Scope::kButton
                            : name == "li"        ? Scope::kListItem
                            : IsTablePart(name)   ? Scope::kTable
                                                  : Scope::kDefault);
    }
    if (i != kNpos) {
      PopTo(i, begin, end);
    }
    return;
  }
  // 一般的结束标签：从上往下找同名的，遇到 special 的元素就算了。
  for (size_t i = stack_.size(); i-- > 1;) {
    const std::string& tag = nodes_[stack_[i]].tag;
    if (tag == name) {
      PopTo(i, begin, end);
      return;
    }
    if (IsSpecial(tag)) {
      return;
    }
  }
}

size_t HtmlTreeBuilder::Insert(Tag tag, size_t begin) {
  const bool foreign =
      foreign_depth_ > 0 || tag.name == "svg" || tag.name == "math";
  const NodeId id = Create(tag.name, std::move(tag.attributes), begin);
  if (foreign ? tag.self_closing : IsVoid(tag.name)) {
    nodes_[id].end = tag.end;
    return tag.end;
  }
  Push(id);
  if (foreign) {
    return tag.end;
  }
  const Content content = ContentOf(tag.name);
  if (content == Content::kNormal) {
    return tag.end;
  }
  const size_t n = source_.size();
  if (content == Content::kPlaintext) {
    AppendText(tag.end, n, /*decode=*/false);
    return n;
  }
  const size_t close = content == Content::kScript
                           ? FindScriptEnd(tag.end)
                           : FindRawTextEnd(tag.end, tag.name);
  AppendText(tag.end, close == kNpos ? n : close,
             content == Content::kEscapableRawText);
  Tag end_tag;
  if (close == kNpos ||
      !ParseTag(close + 2, /*want_attributes=*/false, &end_tag)) {
    return n;
  }
  Pop(end_tag.end);
  return end_tag.end;
}

void HtmlTreeBuilder::Text(size_t begin, size_t end) {
  if (template_depth_ == 0 && foreign_depth_ == 0 && mode_ != Mode::kInBody) {
    // body 之前的空白属于 head、html（html 之前的不要）；别的文字开始 body。
    size_t first = begin;
    while (first < end && IsSpace(source_[first])) {
      ++first;
    }
    if (mode_ == Mode::kInHead || mode_ == Mode::kAfterHead) {
      AppendText(begin, first, /*decode=*/false);
    }
    if (first == end) {
      return;
    }
    begin = first;
    EnsureBody(begin);
  }
  AppendText(begin, end, /*decode=*/true);
}

void HtmlTreeBuilder::AppendText(size_t begin, size_t end, bool decode) {
  // template 的内容不算 textContent。
  if (template_depth_ > 0 || begin >= end) {
    return;
  }
  const std::string_view raw = source_.substr(begin, end - begin);
  if (const size_t last = raw.find_last_not_of(" \t\n\f\r"); last != kNpos) {
    content_end_ = std::max(content_end_, begin + last + 1);
  }
  if (decode && raw.find('&') != kNpos) {
    DecodeEntities(raw, doc_->utf8_, &text_);
  } else {
    text_.append(raw);
  }
}

NodeId HtmlTreeBuilder::Create(std::string_view tag,
                               std::vector<Attribute> attributes,
                               size_t begin) {
  const NodeId id = static_cast<NodeId>(nodes_.size());
  const NodeId parent = current();
  HtmlDocument::Node node;
  node.tag = std::string(tag);
  node.attributes = std::move(attributes);
  node.parent = parent;
  node.begin = begin;
  node.end = begin;
  node.text_begin = text_.size();
  node.text_end = text_.size();
  node.in_template = template_depth_ > 0;
  if (const NodeId last = last_child_[parent]; last != kNone) {
    nodes_[last].next_sibling = id;
    node.prev_sibling = last;
  } else {
    nodes_[parent].first_child = id;
  }
  last_child_[parent] = id;
  nodes_.push_back(std::move(node));
  last_child_.push_back(kNone);
  return id;
}

void HtmlTreeBuilder::Push(NodeId id) {
  stack_.push_back(id);
  const std::string& tag = nodes_[id].tag;
  if (tag == "template") {
    ++template_depth_;
  } else if (tag == "svg" || tag == "math") {
    ++foreign_depth_;
  }
}

void HtmlTreeBuilder::Pop(size_t end) {
  const NodeId id = stack_.back();
  stack_.pop_back();
  HtmlDocument::Node& node = nodes_[id];
  node.end = std::max(end, node.begin);
  node.text_end = text_.size();
  if (node.tag == "template") {
    --template_depth_;
  } else if (node.tag == "svg" || node.tag == "math") {
    --foreign_depth_;
  }
}

void HtmlTreeBuilder::PopTo(size_t index, size_t begin, size_t end) {
  if (index == 0 || index >= stack_.size()) {
    return;
  }
  while (stack_.size() > index + 1) {
    Pop(begin);
  }
  Pop(end);
}

size_t HtmlTreeBuilder::FindInScope(std::string_view tag, Scope scope) const {
  for (size_t i = stack_.size(); i-- > 1;) {
    const std::string& t = nodes_[stack_[i]].tag;
    if (t == tag) {
      return i;
    }
    if (IsScopeBoundary(t, scope)) {
      return kNpos;
    }
  }
  return kNpos;
}

size_t HtmlTreeBuilder::FindHeadingInScope() const {
  for (size_t i = stack_.size(); i-- > 1;) {
    const std::string& t = nodes_[stack_[i]].tag;
    if (IsHeading(t)) {
      return i;
    }
    if (IsScopeBoundary(t, Scope::kDefault)) {
      return kNpos;
    }
  }
  return kNpos;
}

void HtmlTreeBuilder::ClosePIfInButtonScope(size_t at) {
  if (const size_t i = FindInScope("p", Scope::kButton); i != kNpos) {
    PopTo(i, at, at);
  }
}

void HtmlTreeBuilder::CloseUntilContext(
    std::initializer_list<std::string_view> close,
    std::initializer_list<std::string_view> stop,
    size_t at) {
  for (;;) {
    size_t found = kNpos;
    for (size_t i = stack_.size(); i-- > 1;) {
      const std::string& t = nodes_[stack_[i]].tag;
      if (OneOf(t, close)) {
        found = i;
        break;
      }
      if (OneOf(t, stop)) {
        break;
      }
    }
    if (found == kNpos) {
      return;
    }
    PopTo(found, at, at);
  }
}

void HtmlTreeBuilder::CloseImplied(std::string_view tag, size_t begin) {
  if (tag == "li" || tag == "dd" || tag == "dt") {
    for (size_t i = stack_.size(); i-- > 1;) {
      const std::string& t = nodes_[stack_[i]].tag;
      if (tag == "li" ? t == "li" : (t == "dd" || t == "dt")) {
        PopTo(i, begin, begin);
        break;
      }
      if (IsSpecial(t) && t != "address" && t != "div" && t != "p") {
        break;
      }
    }
  } else if (tag == "a") {
    // <a> 里不能套 <a>：前一个结束。
    for (size_t i = stack_.size(); i-- > 1;) {
      const std::string& t = nodes_[stack_[i]].tag;
      if (t == "a") {
        PopTo(i, begin, begin);
        break;
      }
      if (IsScopeBoundary(t, Scope::kDefault)) {
        break;
      }
    }
  } else if (tag == "option" || tag == "optgroup") {
    if (current_tag() == "option") {
      Pop(begin);
    }
    if (tag == "optgroup" && current_tag() == "optgroup") {
      Pop(begin);
    }
  } else if (tag == "button") {
    if (const size_t i = FindInScope("button", Scope::kDefault); i != kNpos) {
      PopTo(i, begin, begin);
    }
  } else if (tag == "tr") {
    CloseUntilContext({"td", "th", "tr"},
                      {"table", "tbody", "thead", "tfoot", "template", "html"},
                      begin);
  } else if (tag == "td" || tag == "th") {
    CloseUntilContext(
        {"td", "th"},
        {"tr", "table", "tbody", "thead", "tfoot", "template", "html"}, begin);
  } else if (OneOf(tag, {"tbody", "thead", "tfoot", "caption", "colgroup"})) {
    CloseUntilContext({"td", "th", "tr", "tbody", "thead", "tfoot", "caption",
                       "colgroup"},
                      {"table", "template", "html"}, begin);
  }
  if (ClosesParagraph(tag)) {
    ClosePIfInButtonScope(begin);
  }
  if (IsHeading(tag) && IsHeading(current_tag())) {
    Pop(begin);
  }
}

void HtmlTreeBuilder::MergeAttributes(NodeId id,
                                      std::vector<Attribute> attributes) {
  std::vector<Attribute>& existing = nodes_[id].attributes;
  for (Attribute& attribute : attributes) {
    if (std::none_of(existing.begin(), existing.end(), [&](const Attribute& a) {
          return a.name == attribute.name;
        })) {
      existing.push_back(std::move(attribute));
    }
  }
}

void HtmlTreeBuilder::EnsureHtml(size_t at) {
  if (html_id_ == kNone) {
    html_id_ = Create("html", {}, at);
    Push(html_id_);
    doc_->root_ = html_id_;
  }
  if (mode_ == Mode::kBeforeHtml) {
    mode_ = Mode::kBeforeHead;
  }
}

void HtmlTreeBuilder::EnsureHead(size_t at) {
  EnsureHtml(at);
  if (head_id_ == kNone) {
    head_id_ = Create("head", {}, at);
    Push(head_id_);
    mode_ = Mode::kInHead;
  }
}

void HtmlTreeBuilder::CloseHead(size_t begin, size_t end) {
  if (mode_ != Mode::kInHead) {
    return;
  }
  for (size_t i = stack_.size(); i-- > 1;) {
    if (stack_[i] == head_id_) {
      PopTo(i, begin, end);
      break;
    }
  }
  mode_ = Mode::kAfterHead;
}

void HtmlTreeBuilder::EnsureBody(size_t at) {
  EnsureHead(at);
  CloseHead(at, at);
  if (body_id_ == kNone) {
    body_id_ = Create("body", {}, at);
    Push(body_id_);
  }
  mode_ = Mode::kInBody;
}

HtmlDocument::Node::Node() = default;
HtmlDocument::Node::Node(Node&&) = default;
HtmlDocument::Node& HtmlDocument::Node::operator=(Node&&) = default;
HtmlDocument::Node::~Node() = default;

HtmlDocument::HtmlDocument(std::string_view html, std::string_view charset)
    : source_(html),
      charset_(charset),
      utf8_(charset.empty() ||
            base::EqualsCaseInsensitiveASCII(charset, "utf-8")) {
  HtmlTreeBuilder(this).Run();
}

HtmlDocument::~HtmlDocument() = default;

HtmlDocument::NodeId HtmlDocument::ParentElement(NodeId id) const {
  if (id >= nodes_.size()) {
    return kNone;
  }
  const NodeId parent = nodes_[id].parent;
  return parent == kNone || parent == kDocument ? kNone : parent;
}

const std::string* HtmlDocument::AttributeValue(NodeId id,
                                                std::string_view name) const {
  for (const Attribute& attribute : nodes_[id].attributes) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

std::string_view HtmlDocument::Text(NodeId id, std::string* storage) const {
  if (id == kDocument || id >= nodes_.size()) {
    return std::string_view();
  }
  const Node& node = nodes_[id];
  const std::string_view raw = std::string_view(text_).substr(
      node.text_begin, node.text_end - node.text_begin);
  if (utf8_ || base::IsStringASCII(raw)) {
    return raw;
  }
  *storage = ToUtf8(raw, charset_);
  return *storage;
}

}  // namespace starry::content_filter
