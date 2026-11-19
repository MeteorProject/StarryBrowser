// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/css_selector.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"

namespace starry::content_filter {

namespace {

using NodeId = HtmlDocument::NodeId;
constexpr NodeId kNone = HtmlDocument::kNone;
constexpr NodeId kDocument = HtmlDocument::kDocument;
// 括号最多套几层（`:not(:has(:is(…)))`）。
constexpr int kMaxDepth = 8;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool IsCombinator(char c) {
  return c == '>' || c == '+' || c == '~';
}

bool IsNameChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

bool IsIdentifierStart(char c) {
  return base::IsAsciiAlpha(c) || c == '-' || c == '_' || c == '\\' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// HTML 里这些属性的值比较时不分大小写（HTML 规范「Selectors」一节）。
bool IsCaseInsensitiveAttribute(std::string_view name) {
  static constexpr std::string_view kNames[] = {
      "accept",    "accept-charset", "align",      "alink",    "axis",
      "bgcolor",   "charset",        "checked",    "clear",    "codetype",
      "color",     "compact",        "declare",    "defer",    "dir",
      "direction", "disabled",       "enctype",    "face",     "frame",
      "hreflang",  "http-equiv",     "lang",       "language", "link",
      "media",     "method",         "multiple",   "nohref",   "noresize",
      "noshade",   "nowrap",         "readonly",   "rel",      "rev",
      "rules",     "scope",          "scrolling",  "selected", "shape",
      "target",    "text",           "type",       "valign",   "valuetype",
      "vlink"};
  return std::find(std::begin(kNames), std::end(kNames), name) !=
         std::end(kNames);
}

bool Equals(std::string_view a, std::string_view b, bool ignore_case) {
  return ignore_case ? base::EqualsCaseInsensitiveASCII(a, b) : a == b;
}

// 空白分开的一列里有没有 word。
bool ContainsWord(std::string_view list,
                  std::string_view word,
                  bool ignore_case) {
  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && IsSpace(list[i])) {
      ++i;
    }
    const size_t start = i;
    while (i < list.size() && !IsSpace(list[i])) {
      ++i;
    }
    if (i > start && Equals(list.substr(start, i - start), word, ignore_case)) {
      return true;
    }
  }
  return false;
}

}  // namespace

struct CssSelector::Attribute {
  enum class Op : uint8_t {
    kExists,
    kEquals,
    kIncludes,
    kDashMatch,
    kPrefix,
    kSuffix,
    kSubstring,
  };

  // 小写。
  std::string name;
  Op op = Op::kExists;
  std::string value;
  bool ignore_case = false;
};

struct CssSelector::Pseudo {
  enum class Kind : uint8_t { kNot, kIs, kHas, kNth, kEmpty, kRoot, kScope };

  Kind kind = Kind::kIs;
  std::unique_ptr<CssSelector> selector;
  // kNth：兄弟里的第 a*n+b 个（n >= 0，从 1 数），of_type 只数同样标签的，from_end 从后面数。
  int a = 0;
  int b = 0;
  bool of_type = false;
  bool from_end = false;
};

struct CssSelector::Compound {
  // 小写；空的是任意。
  std::string tag;
  std::vector<std::string> ids;
  std::vector<std::string> classes;
  std::vector<Attribute> attributes;
  std::vector<Pseudo> pseudos;
};

struct CssSelector::Complex {
  // 从左到右，combinators[i] 在 compounds[i] 和 compounds[i + 1] 之间（' '、'>'、'+'、'~'）。
  std::vector<Compound> compounds;
  std::vector<char> combinators;
  // 相对的选择器开头的组合符（省略的是 ' '）；不是相对的是 0。
  char leading = 0;
};

class CssSelector::Parser {
 public:
  Parser(std::string_view text, int depth) : text_(text), depth_(depth) {}

  bool ParseList(bool relative, std::vector<Complex>* out) {
    if (depth_ > kMaxDepth) {
      return false;
    }
    for (;;) {
      SkipSpace();
      Complex complex;
      if (!ParseComplex(relative, &complex)) {
        return false;
      }
      out->push_back(std::move(complex));
      SkipSpace();
      if (AtEnd()) {
        return true;
      }
      if (Peek() != ',') {
        return false;
      }
      ++pos_;
    }
  }

 private:
  bool AtEnd() const { return pos_ >= text_.size(); }
  char Peek(size_t offset = 0) const {
    return pos_ + offset < text_.size() ? text_[pos_ + offset] : '\0';
  }

  bool SkipSpace() {
    const size_t start = pos_;
    while (!AtEnd() && IsSpace(Peek())) {
      ++pos_;
    }
    return pos_ > start;
  }

  bool ParseComplex(bool relative, Complex* out) {
    if (relative) {
      out->leading = ' ';
      if (IsCombinator(Peek())) {
        out->leading = Peek();
        ++pos_;
        SkipSpace();
      }
    }
    for (;;) {
      Compound compound;
      if (!ParseCompound(&compound)) {
        return false;
      }
      out->compounds.push_back(std::move(compound));
      const bool space = SkipSpace();
      if (AtEnd() || Peek() == ',') {
        return true;
      }
      char combinator = ' ';
      if (IsCombinator(Peek())) {
        combinator = Peek();
        ++pos_;
        SkipSpace();
      } else if (!space) {
        return false;
      }
      out->combinators.push_back(combinator);
    }
  }

  bool ParseCompound(Compound* out) {
    const size_t start = pos_;
    if (Peek() == '*') {
      ++pos_;
    } else if (IsIdentifierStart(Peek())) {
      std::string tag;
      if (!ParseName(&tag)) {
        return false;
      }
      out->tag = base::ToLowerASCII(tag);
    }
    if (Peek() == '|') {
      // 命名空间。
      return false;
    }
    for (;;) {
      const char c = Peek();
      if (c == '#' || c == '.') {
        ++pos_;
        std::string name;
        if (!ParseName(&name)) {
          return false;
        }
        (c == '#' ? out->ids : out->classes).push_back(std::move(name));
      } else if (c == '[') {
        if (!ParseAttribute(out)) {
          return false;
        }
      } else if (c == ':') {
        if (!ParsePseudo(out)) {
          return false;
        }
      } else {
        break;
      }
    }
    return pos_ > start;
  }

  // CSS 的名字（标识符、`#` `.` 后面的），转义解开。
  bool ParseName(std::string* out) {
    const size_t start = pos_;
    while (!AtEnd()) {
      const char c = Peek();
      if (c == '\\') {
        if (!ParseEscape(out)) {
          return false;
        }
      } else if (IsNameChar(c)) {
        out->push_back(c);
        ++pos_;
      } else {
        break;
      }
    }
    return pos_ > start;
  }

  bool ParseEscape(std::string* out) {
    ++pos_;
    if (AtEnd() || Peek() == '\n') {
      return false;
    }
    if (!base::IsHexDigit(Peek())) {
      out->push_back(Peek());
      ++pos_;
      return true;
    }
    uint32_t code = 0;
    for (int digits = 0; digits < 6 && base::IsHexDigit(Peek()); ++digits) {
      code = code * 16 + base::HexDigitToInt(Peek());
      ++pos_;
    }
    if (IsSpace(Peek())) {
      ++pos_;
    }
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      code = 0xFFFD;
    }
    base::WriteUnicodeCharacter(static_cast<base_icu::UChar32>(code), out);
    return true;
  }

  bool ParseString(std::string* out) {
    const char quote = Peek();
    ++pos_;
    while (!AtEnd()) {
      const char c = Peek();
      if (c == quote) {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        if (Peek(1) == '\n') {
          pos_ += 2;
          continue;
        }
        if (!ParseEscape(out)) {
          return false;
        }
        continue;
      }
      if (c == '\n') {
        return false;
      }
      out->push_back(c);
      ++pos_;
    }
    return false;
  }

  bool ParseAttribute(Compound* out) {
    ++pos_;
    SkipSpace();
    Attribute attribute;
    std::string name;
    if (!ParseName(&name) || (Peek() == '|' && Peek(1) != '=')) {
      return false;
    }
    attribute.name = base::ToLowerASCII(name);
    SkipSpace();
    if (Peek() == ']') {
      ++pos_;
      out->attributes.push_back(std::move(attribute));
      return true;
    }
    using Op = Attribute::Op;
    if (Peek() == '=') {
      attribute.op = Op::kEquals;
      ++pos_;
    } else if (Peek(1) == '=') {
      switch (Peek()) {
        case '~':
          attribute.op = Op::kIncludes;
          break;
        case '|':
          attribute.op = Op::kDashMatch;
          break;
        case '^':
          attribute.op = Op::kPrefix;
          break;
        case '$':
          attribute.op = Op::kSuffix;
          break;
        case '*':
          attribute.op = Op::kSubstring;
          break;
        default:
          return false;
      }
      pos_ += 2;
    } else {
      return false;
    }
    SkipSpace();
    if (Peek() == '"' || Peek() == '\'') {
      if (!ParseString(&attribute.value)) {
        return false;
      }
    } else if (!ParseName(&attribute.value)) {
      return false;
    }
    SkipSpace();
    bool sensitive = false;
    if (Peek() == 'i' || Peek() == 'I') {
      attribute.ignore_case = true;
      ++pos_;
      SkipSpace();
    } else if (Peek() == 's' || Peek() == 'S') {
      sensitive = true;
      ++pos_;
      SkipSpace();
    }
    if (Peek() != ']') {
      return false;
    }
    ++pos_;
    if (!sensitive && IsCaseInsensitiveAttribute(attribute.name)) {
      attribute.ignore_case = true;
    }
    out->attributes.push_back(std::move(attribute));
    return true;
  }

  // pos_ 在 '('：取出配对的 ')' 之前的（引号、转义里的括号不算）。
  bool ParseArgument(std::string_view* out) {
    int depth = 0;
    char quote = 0;
    for (size_t i = pos_; i < text_.size(); ++i) {
      const char c = text_[i];
      if (c == '\\') {
        ++i;
        continue;
      }
      if (quote) {
        if (c == quote) {
          quote = 0;
        }
        continue;
      }
      if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')' && --depth == 0) {
        *out = text_.substr(pos_ + 1, i - pos_ - 1);
        pos_ = i + 1;
        return true;
      }
    }
    return false;
  }

  std::unique_ptr<CssSelector> ParseNested(std::string_view text,
                                           bool relative) {
    auto selector = base::WrapUnique(new CssSelector(relative));
    Parser parser(base::TrimWhitespaceASCII(text, base::TRIM_ALL),
                  depth_ + 1);
    if (!parser.ParseList(relative, &selector->selectors_)) {
      return nullptr;
    }
    return selector;
  }

  // `an+b`、`odd`、`even`（不支持 `of 选择器`）。
  static bool ParseNth(std::string_view text, int* a, int* b) {
    std::string s;
    for (char c : text) {
      if (!IsSpace(c)) {
        s += base::ToLowerASCII(c);
      }
    }
    if (s == "odd" || s == "even") {
      *a = 2;
      *b = s == "odd" ? 1 : 0;
      return true;
    }
    auto parse_int = [](std::string_view value, int* out) {
      if (value.starts_with('+')) {
        value.remove_prefix(1);
        if (value.starts_with('-')) {
          return false;
        }
      }
      return base::StringToInt(value, out);
    };
    const size_t n = s.find('n');
    if (n == std::string::npos) {
      *a = 0;
      return parse_int(s, b);
    }
    const std::string_view before = std::string_view(s).substr(0, n);
    const std::string_view after = std::string_view(s).substr(n + 1);
    if (before.empty() || before == "+") {
      *a = 1;
    } else if (before == "-") {
      *a = -1;
    } else if (!parse_int(before, a)) {
      return false;
    }
    *b = 0;
    if (after.empty()) {
      return true;
    }
    if (after.front() != '+' && after.front() != '-') {
      return false;
    }
    return parse_int(after, b);
  }

  bool ParsePseudo(Compound* out) {
    ++pos_;
    if (Peek() == ':') {
      // 伪元素。
      return false;
    }
    std::string name;
    if (!ParseName(&name)) {
      return false;
    }
    name = base::ToLowerASCII(name);
    std::string_view arg;
    const bool has_arg = Peek() == '(';
    if (has_arg && !ParseArgument(&arg)) {
      return false;
    }
    using Kind = Pseudo::Kind;
    Pseudo pseudo;
    if (name == "not" || name == "is" || name == "where" ||
        name == "matches" || name == "-webkit-any") {
      if (!has_arg) {
        return false;
      }
      pseudo.kind = name == "not" ? Kind::kNot : Kind::kIs;
      pseudo.selector = ParseNested(arg, /*relative=*/false);
      if (!pseudo.selector) {
        return false;
      }
    } else if (name == "has") {
      if (!has_arg) {
        return false;
      }
      pseudo.kind = Kind::kHas;
      pseudo.selector = ParseNested(arg, /*relative=*/true);
      if (!pseudo.selector) {
        return false;
      }
    } else if (name == "nth-child" || name == "nth-last-child" ||
               name == "nth-of-type" || name == "nth-last-of-type") {
      if (!has_arg || !ParseNth(arg, &pseudo.a, &pseudo.b)) {
        return false;
      }
      pseudo.kind = Kind::kNth;
      pseudo.of_type = name.ends_with("of-type");
      pseudo.from_end = name.starts_with("nth-last");
    } else if (has_arg) {
      return false;
    } else if (name == "first-child" || name == "last-child" ||
               name == "first-of-type" || name == "last-of-type") {
      pseudo.kind = Kind::kNth;
      pseudo.b = 1;
      pseudo.of_type = name.ends_with("of-type");
      pseudo.from_end = name.starts_with("last");
    } else if (name == "only-child" || name == "only-of-type") {
      // 既是第一个又是最后一个。
      for (bool from_end : {false, true}) {
        Pseudo nth;
        nth.kind = Kind::kNth;
        nth.b = 1;
        nth.of_type = name == "only-of-type";
        nth.from_end = from_end;
        out->pseudos.push_back(std::move(nth));
      }
      return true;
    } else if (name == "empty") {
      pseudo.kind = Kind::kEmpty;
    } else if (name == "root") {
      pseudo.kind = Kind::kRoot;
    } else if (name == "scope") {
      pseudo.kind = Kind::kScope;
    } else {
      return false;
    }
    out->pseudos.push_back(std::move(pseudo));
    return true;
  }

  const std::string_view text_;
  size_t pos_ = 0;
  const int depth_;
};

CssSelector::CssSelector(bool relative) : relative_(relative) {}

CssSelector::~CssSelector() = default;

// static
std::unique_ptr<CssSelector> CssSelector::Parse(std::string_view text,
                                                bool relative) {
  auto selector = base::WrapUnique(new CssSelector(relative));
  Parser parser(base::TrimWhitespaceASCII(text, base::TRIM_ALL), 0);
  if (!parser.ParseList(relative, &selector->selectors_)) {
    return nullptr;
  }
  return selector;
}

bool CssSelector::Matches(const HtmlDocument& doc,
                          NodeId element,
                          NodeId scope) const {
  if (!doc.IsElement(element)) {
    return false;
  }
  for (const Complex& complex : selectors_) {
    if (MatchComplex(doc, complex, complex.compounds.size() - 1, element,
                     scope)) {
      return true;
    }
  }
  return false;
}

void CssSelector::Query(const HtmlDocument& doc,
                        NodeId scope,
                        std::vector<NodeId>* out) const {
  const HtmlDocument::Node& node = doc.node(scope);
  auto scan = [&](NodeId begin, NodeId end) {
    for (NodeId id = begin; id < end; ++id) {
      if (doc.IsElement(id) && Matches(doc, id, scope)) {
        out->push_back(id);
      }
    }
  };
  if (!relative_) {
    scan(scope + 1, node.subtree_end);
    return;
  }
  // 相对的：里面的（空白、`>` 开头）、后面的兄弟和它们里面的（`+`、`~` 开头）。
  bool inside = false;
  bool after = false;
  for (const Complex& complex : selectors_) {
    (complex.leading == '+' || complex.leading == '~' ? after : inside) = true;
  }
  if (inside) {
    scan(scope + 1, node.subtree_end);
  }
  if (after && node.parent != kNone) {
    scan(node.subtree_end, doc.node(node.parent).subtree_end);
  }
}

bool CssSelector::HasRelative(const HtmlDocument& doc, NodeId anchor) const {
  const HtmlDocument::Node& node = doc.node(anchor);
  for (const Complex& complex : selectors_) {
    const size_t last = complex.compounds.size() - 1;
    if (complex.leading == '+' || complex.leading == '~') {
      if (node.parent == kNone) {
        continue;
      }
      if (complex.leading == '+' && last == 0) {
        if (node.next_sibling != kNone && doc.IsElement(node.next_sibling) &&
            MatchComplex(doc, complex, 0, node.next_sibling, anchor)) {
          return true;
        }
        continue;
      }
      for (NodeId id = node.subtree_end;
           id < doc.node(node.parent).subtree_end; ++id) {
        if (doc.IsElement(id) && MatchComplex(doc, complex, last, id, anchor)) {
          return true;
        }
      }
      continue;
    }
    if (complex.leading == '>' && last == 0) {
      for (NodeId child = node.first_child; child != kNone;
           child = doc.node(child).next_sibling) {
        if (doc.IsElement(child) &&
            MatchComplex(doc, complex, 0, child, anchor)) {
          return true;
        }
      }
      continue;
    }
    for (NodeId id = anchor + 1; id < node.subtree_end; ++id) {
      if (doc.IsElement(id) && MatchComplex(doc, complex, last, id, anchor)) {
        return true;
      }
    }
  }
  return false;
}

bool CssSelector::MatchComplex(const HtmlDocument& doc,
                               const Complex& complex,
                               size_t index,
                               NodeId element,
                               NodeId scope) const {
  if (!MatchCompound(doc, complex.compounds[index], element, scope)) {
    return false;
  }
  const HtmlDocument::Node& node = doc.node(element);
  if (index == 0) {
    switch (complex.leading) {
      case 0:
        return true;
      case ' ':
        for (NodeId p = node.parent; p != kNone; p = doc.node(p).parent) {
          if (p == scope) {
            return true;
          }
        }
        return false;
      case '>':
        return node.parent == scope;
      case '+':
        return node.prev_sibling == scope;
      case '~':
        for (NodeId s = node.prev_sibling; s != kNone;
             s = doc.node(s).prev_sibling) {
          if (s == scope) {
            return true;
          }
        }
        return false;
    }
    return false;
  }
  switch (complex.combinators[index - 1]) {
    case ' ':
      for (NodeId p = doc.ParentElement(element); p != kNone;
           p = doc.ParentElement(p)) {
        if (MatchComplex(doc, complex, index - 1, p, scope)) {
          return true;
        }
      }
      return false;
    case '>': {
      const NodeId p = doc.ParentElement(element);
      return p != kNone && MatchComplex(doc, complex, index - 1, p, scope);
    }
    case '+':
      return node.prev_sibling != kNone &&
             MatchComplex(doc, complex, index - 1, node.prev_sibling, scope);
    case '~':
      for (NodeId s = node.prev_sibling; s != kNone;
           s = doc.node(s).prev_sibling) {
        if (MatchComplex(doc, complex, index - 1, s, scope)) {
          return true;
        }
      }
      return false;
  }
  return false;
}

bool CssSelector::MatchCompound(const HtmlDocument& doc,
                                const Compound& compound,
                                NodeId element,
                                NodeId scope) const {
  if (!compound.tag.empty() && compound.tag != doc.node(element).tag) {
    return false;
  }
  for (const std::string& id : compound.ids) {
    const std::string* value = doc.AttributeValue(element, "id");
    if (!value || *value != id) {
      return false;
    }
  }
  if (!compound.classes.empty()) {
    const std::string* value = doc.AttributeValue(element, "class");
    if (!value) {
      return false;
    }
    for (const std::string& name : compound.classes) {
      if (!ContainsWord(*value, name, /*ignore_case=*/false)) {
        return false;
      }
    }
  }
  for (const Attribute& attribute : compound.attributes) {
    const std::string* found = doc.AttributeValue(element, attribute.name);
    if (!found) {
      return false;
    }
    const std::string_view value = *found;
    const std::string_view expected = attribute.value;
    const bool ignore_case = attribute.ignore_case;
    using Op = Attribute::Op;
    bool matched = false;
    switch (attribute.op) {
      case Op::kExists:
        matched = true;
        break;
      case Op::kEquals:
        matched = Equals(value, expected, ignore_case);
        break;
      case Op::kIncludes:
        matched = !expected.empty() &&
                  std::none_of(expected.begin(), expected.end(), IsSpace) &&
                  ContainsWord(value, expected, ignore_case);
        break;
      case Op::kDashMatch:
        matched = Equals(value, expected, ignore_case) ||
                  (value.size() > expected.size() &&
                   value[expected.size()] == '-' &&
                   Equals(value.substr(0, expected.size()), expected,
                          ignore_case));
        break;
      case Op::kPrefix:
        matched = !expected.empty() && value.size() >= expected.size() &&
                  Equals(value.substr(0, expected.size()), expected,
                         ignore_case);
        break;
      case Op::kSuffix:
        matched = !expected.empty() && value.size() >= expected.size() &&
                  Equals(value.substr(value.size() - expected.size()),
                         expected, ignore_case);
        break;
      case Op::kSubstring:
        matched = !expected.empty() &&
                  (ignore_case ? base::ToLowerASCII(value).find(
                                     base::ToLowerASCII(expected)) !=
                                     std::string::npos
                               : value.find(expected) != std::string::npos);
        break;
    }
    if (!matched) {
      return false;
    }
  }
  for (const Pseudo& pseudo : compound.pseudos) {
    if (!MatchPseudo(doc, pseudo, element, scope)) {
      return false;
    }
  }
  return true;
}

bool CssSelector::MatchPseudo(const HtmlDocument& doc,
                              const Pseudo& pseudo,
                              NodeId element,
                              NodeId scope) const {
  const HtmlDocument::Node& node = doc.node(element);
  switch (pseudo.kind) {
    case Pseudo::Kind::kNot:
      return !pseudo.selector->Matches(doc, element, scope);
    case Pseudo::Kind::kIs:
      return pseudo.selector->Matches(doc, element, scope);
    case Pseudo::Kind::kHas:
      return pseudo.selector->HasRelative(doc, element);
    case Pseudo::Kind::kEmpty:
      return node.first_child == kNone && node.text_begin == node.text_end;
    case Pseudo::Kind::kRoot:
      return element == doc.root();
    case Pseudo::Kind::kScope:
      return scope == kNone || scope == kDocument ? element == doc.root()
                                                  : element == scope;
    case Pseudo::Kind::kNth: {
      int index = 1;
      for (NodeId s = pseudo.from_end ? node.next_sibling : node.prev_sibling;
           s != kNone; s = pseudo.from_end ? doc.node(s).next_sibling
                                           : doc.node(s).prev_sibling) {
        if (!pseudo.of_type || doc.node(s).tag == node.tag) {
          ++index;
        }
      }
      if (pseudo.a == 0) {
        return index == pseudo.b;
      }
      const int diff = index - pseudo.b;
      return diff % pseudo.a == 0 && diff / pseudo.a >= 0;
    }
  }
  return false;
}

}  // namespace starry::content_filter
