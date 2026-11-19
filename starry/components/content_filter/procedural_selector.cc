// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/procedural_selector.h"

#include <algorithm>
#include <vector>

#include "base/json/string_escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace starry::content_filter {

namespace {

constexpr size_t kNpos = std::string_view::npos;
// 选择器里的括号最多套几层（`:has(:not(:has(…)))`）。
constexpr int kMaxDepth = 8;
constexpr size_t kMaxSelectorLength = 4096;

std::string_view Trim(std::string_view text) {
  return base::TrimWhitespaceASCII(text, base::TRIM_ALL);
}

std::string_view TrimLeading(std::string_view text) {
  return base::TrimWhitespaceASCII(text, base::TRIM_LEADING);
}

bool IsIdentChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_';
}

bool IsCombinator(char c) {
  return c == '>' || c == '+' || c == '~';
}

enum class Op : uint8_t {
  // 原生的伪类（`:not(.x)`、`:nth-child()`、`:has(> .x)`……），留在选择器里给浏览器。
  kNative,
  // 不支持的扩展写法，整条跳过。
  kUnsupported,
  // 动作（`:style()`、`:remove()`……），只能在最后，SplitSelectorAction 先分出去。
  kAction,
  kHasText,
  kHas,
  kNot,
  kUpward,
  kNthAncestor,
  kXpath,
  kMatchesCss,
  kMatchesCssBefore,
  kMatchesCssAfter,
  kMinTextLength,
  kWatchAttr,
  kMatchesAttr,
  kMatchesPath,
  kMatchesMedia,
  kOthers,
};

struct OpName {
  std::string_view name;
  Op op;
  // 参数是文字或正则：引号不算特殊的（`:has-text(Don't)`），只数括号。
  bool text;
};

// uBlock Origin、AdGuard、Adblock Plus 的写法（同一个意思的几个名字）。
constexpr OpName kOps[] = {
    {"-abp-contains", Op::kHasText, true},
    {"-abp-has", Op::kHas, false},
    {"-abp-properties", Op::kUnsupported, true},
    {"contains", Op::kHasText, true},
    {"has", Op::kHas, false},
    {"has-text", Op::kHasText, true},
    {"if", Op::kHas, false},
    {"if-not", Op::kNot, false},
    {"matches-attr", Op::kMatchesAttr, true},
    {"matches-css", Op::kMatchesCss, true},
    {"matches-css-after", Op::kMatchesCssAfter, true},
    {"matches-css-before", Op::kMatchesCssBefore, true},
    {"matches-media", Op::kMatchesMedia, true},
    {"matches-path", Op::kMatchesPath, true},
    {"matches-prop", Op::kUnsupported, true},
    {"matches-property", Op::kUnsupported, true},
    {"min-text-length", Op::kMinTextLength, true},
    {"not", Op::kNot, false},
    {"nth-ancestor", Op::kNthAncestor, true},
    {"others", Op::kOthers, true},
    {"remove", Op::kAction, true},
    {"remove-attr", Op::kAction, true},
    {"remove-class", Op::kAction, true},
    {"shadow", Op::kUnsupported, false},
    {"spath", Op::kUnsupported, false},
    {"style", Op::kAction, false},
    {"upward", Op::kUpward, false},
    {"watch-attr", Op::kWatchAttr, true},
    {"xpath", Op::kXpath, false},
};

const OpName* FindOp(std::string_view name) {
  for (const OpName& op : kOps) {
    if (base::EqualsCaseInsensitiveASCII(name, op.name)) {
      return &op;
    }
  }
  return nullptr;
}

// 选择器里的一个伪类函数 `:名字(参数)`。
struct Function {
  // ':' 的位置。
  size_t start = 0;
  // ')' 的后面。
  size_t end = 0;
  std::string_view name;
  std::string_view arg;
};

// text[open] 是 '('，返回配对的 ')'。quotes：引号里的括号不算。
size_t FindClose(std::string_view text, size_t open, bool quotes) {
  int depth = 0;
  char quote = 0;
  for (size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
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
    if (quotes && (c == '"' || c == '\'')) {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && --depth == 0) {
      return i;
    }
  }
  return kNpos;
}

// 最外层（不在括号、方括号、引号里）的伪类函数。括号、引号不配对时返回 false。
bool TopLevelFunctions(std::string_view text, std::vector<Function>* out) {
  int brackets = 0;
  char quote = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
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
    switch (c) {
      case '"':
      case '\'':
        quote = c;
        break;
      case '[':
        ++brackets;
        break;
      case ']':
        if (--brackets < 0) {
          return false;
        }
        break;
      case ')':
        return false;
      case '(': {
        size_t name_start = i;
        while (name_start > 0 && IsIdentChar(text[name_start - 1])) {
          --name_start;
        }
        if (brackets > 0 || name_start == i || name_start == 0 ||
            text[name_start - 1] != ':') {
          return false;
        }
        const std::string_view name = text.substr(name_start, i - name_start);
        const OpName* op = FindOp(name);
        size_t close = FindClose(text, i, !(op && op->text));
        if (op && op->text && i + 1 < text.size() &&
            (text[i + 1] == '\'' || text[i + 1] == '"')) {
          // 整个用引号括起来的文字参数（`:has-text(';confirm(')`）：引号里的括号不算，
          // 引号留着（是要找的文字的一部分）。
          const size_t quoted = FindClose(text, i, /*quotes=*/true);
          if (quoted != kNpos && text[quoted - 1] == text[i + 1]) {
            close = quoted;
          }
        }
        if (close == kNpos) {
          return false;
        }
        out->push_back({name_start - 1, close + 1, name,
                        text.substr(i + 1, close - i - 1)});
        i = close;
        break;
      }
      default:
        break;
    }
  }
  return quote == 0 && brackets == 0;
}

bool IsProcedural(std::string_view text, int depth);

// 一个最外层的伪类函数要不要脚本做。
Op Classify(const Function& f, int depth) {
  const OpName* op = FindOp(f.name);
  if (!op) {
    // 原生的 :is()、:where() 这类：里面有扩展写法的不支持。
    return IsProcedural(f.arg, depth + 1) ? Op::kUnsupported : Op::kNative;
  }
  if (op->op == Op::kHas && op->name == "has") {
    // 原生的 :has() 不能再套 :has()（规范不许，浏览器会丢掉整条），这种也交给脚本。
    return IsProcedural(f.arg, depth + 1) || f.arg.find(":has(") != kNpos
               ? Op::kHas
               : Op::kNative;
  }
  if (op->op == Op::kNot && op->name == "not") {
    return IsProcedural(f.arg, depth + 1) ? Op::kNot : Op::kNative;
  }
  return op->op;
}

bool IsProcedural(std::string_view text, int depth) {
  if (depth > kMaxDepth) {
    // 套得太深的交给编译（会失败，整条跳过）。
    return true;
  }
  std::vector<Function> functions;
  if (!TopLevelFunctions(text, &functions)) {
    // 写错了：当原生的，后面检查选择器时跳过。
    return false;
  }
  for (const Function& f : functions) {
    if (Classify(f, depth) != Op::kNative) {
      return true;
    }
  }
  return false;
}

// ---- JSON ----

void AppendString(std::string* out, std::string_view text) {
  base::EscapeJSONString(text, /*put_in_quotes=*/true, out);
}

void BeginTask(std::string* tasks, std::string_view name) {
  if (!tasks->empty()) {
    *tasks += ',';
  }
  *tasks += '[';
  AppendString(tasks, name);
}

// `/正则/标志`：取出源和标志。
bool ParseRegex(std::string_view text,
                std::string* source,
                std::string* flags) {
  if (text.size() < 3 || text.front() != '/') {
    return false;
  }
  const size_t last = text.rfind('/');
  if (last == 0) {
    return false;
  }
  std::string kept;
  for (char c : text.substr(last + 1)) {
    if (std::string_view("dgimsuvy").find(c) == kNpos) {
      return false;
    }
    // g、y 会让 test() 从上次的位置接着找，d 没用：去掉。
    if (std::string_view("imsuv").find(c) != kNpos && kept.find(c) == kNpos) {
      kept += c;
    }
  }
  *source = std::string(text.substr(1, last - 1));
  *flags = std::move(kept);
  return true;
}

// 字面的文字写成正则。exact：整个对上；wildcard：`*` 是任意的文字（AdGuard 的写法）。
std::string EscapeRegex(std::string_view text, bool exact, bool wildcard) {
  std::string out;
  if (exact) {
    out += '^';
  }
  for (char c : text) {
    if (wildcard && c == '*') {
      out += ".*";
      continue;
    }
    if (std::string_view("\\^$.*+?()[]{}|/").find(c) != kNpos) {
      out += '\\';
    }
    out += c;
  }
  if (exact) {
    out += '$';
  }
  return out;
}

// 写成 JSON 的 `"正则","标志"`：`/正则/` 照用，别的是字面的文字。
void AppendMatcher(std::string* out,
                   std::string_view text,
                   bool exact,
                   bool wildcard) {
  std::string source;
  std::string flags;
  if (!ParseRegex(text, &source, &flags)) {
    source = EscapeRegex(text, exact, wildcard);
  }
  AppendString(out, source);
  *out += ',';
  AppendString(out, flags);
}

// ---- 选择器的各段 ----

// 原生选择器里第一个最外层的空白、组合符（> + ~）或逗号。
size_t FindTopLevelBreak(std::string_view text) {
  int parens = 0;
  int brackets = 0;
  char quote = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
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
    switch (c) {
      case '"':
      case '\'':
        quote = c;
        break;
      case '(':
        ++parens;
        break;
      case ')':
        --parens;
        break;
      case '[':
        ++brackets;
        break;
      case ']':
        --brackets;
        break;
      default:
        if (parens == 0 && brackets == 0 &&
            (base::IsAsciiWhitespace(c) || IsCombinator(c) || c == ',')) {
          return i;
        }
    }
  }
  return kNpos;
}

bool HasTopLevelComma(std::string_view text) {
  size_t offset = 0;
  for (;;) {
    const size_t i = FindTopLevelBreak(text.substr(offset));
    if (i == kNpos) {
      return false;
    }
    if (text[offset + i] == ',') {
      return true;
    }
    offset += i + 1;
  }
}

// 原生的一段。在操作符前面、以空白或组合符结尾时（`div :has-text(x)`、`div > :has-text(x)`）
// 补上 `*`。
std::string CompleteSegment(std::string_view segment, bool before_op) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(segment, base::TRIM_TRAILING);
  std::string result(trimmed);
  if (before_op && !result.empty() &&
      (trimmed.size() != segment.size() || IsCombinator(result.back()))) {
    result += " *";
  }
  return result;
}

// 开头的原生选择器（没有时从整个文档、或者 :has() 外面的元素开始）。
bool CompileBase(std::string_view segment,
                 bool nested,
                 bool before_op,
                 std::string* base) {
  std::string text = CompleteSegment(TrimLeading(segment), before_op);
  if (!text.empty() && IsCombinator(text.front())) {
    // `> .x`、`+ .x`：只在 :has()、:not() 里能用（相对于外面的元素）。
    if (!nested || HasTopLevelComma(text)) {
      return false;
    }
  }
  *base = std::move(text);
  return true;
}

// 操作符后面接着的原生部分：`.x`（找到的元素还要符合）、` span`、`> span`、`+ span`（从找到的
// 元素往里、往后找）。
bool CompileContinuation(std::string_view segment,
                         bool before_op,
                         std::string* tasks) {
  while (!segment.empty()) {
    const char c = segment.front();
    if (base::IsAsciiWhitespace(c) || IsCombinator(c)) {
      std::string_view rest = TrimLeading(segment);
      char combinator = ' ';
      if (!rest.empty() && IsCombinator(rest.front())) {
        combinator = rest.front();
        rest = TrimLeading(rest.substr(1));
      } else if (rest.empty() && !before_op) {
        // 末尾的空白。
        return true;
      }
      std::string selector = CompleteSegment(rest, before_op);
      if (selector.empty()) {
        if (!before_op) {
          return false;
        }
        selector = "*";
      }
      if (HasTopLevelComma(selector)) {
        return false;
      }
      std::string path(1, combinator);
      if (combinator != ' ') {
        path += ' ';
      }
      path += selector;
      BeginTask(tasks, "spath");
      *tasks += ',';
      AppendString(tasks, path);
      *tasks += ']';
      return true;
    }
    // 接着的复合选择器（`.x`、`[attr]`、`:not(.y)`）。
    const size_t end = FindTopLevelBreak(segment);
    if (end != kNpos && segment[end] == ',') {
      return false;
    }
    BeginTask(tasks, "match");
    *tasks += ',';
    AppendString(tasks, segment.substr(0, end));
    *tasks += ']';
    if (end == kNpos) {
      return true;
    }
    segment = segment.substr(end);
  }
  return true;
}

bool CompileSelector(std::string_view text,
                     bool nested,
                     int depth,
                     std::string* out);

bool ParseCount(std::string_view text,
                unsigned min,
                unsigned max,
                unsigned* n) {
  return base::StringToUint(text, n) && *n >= min && *n <= max;
}

// `:matches-css(属性: 值)`；uBlock Origin 也能写 `:matches-css(before, 属性: 值)`。
bool CompileMatchesCss(std::string_view arg, Op op, std::string* tasks) {
  std::string_view pseudo = op == Op::kMatchesCssBefore  ? "::before"
                            : op == Op::kMatchesCssAfter ? "::after"
                                                         : "";
  if (op == Op::kMatchesCss) {
    if (const size_t comma = arg.find(','); comma != kNpos) {
      std::string_view first = Trim(arg.substr(0, comma));
      while (!first.empty() && first.front() == ':') {
        first.remove_prefix(1);
      }
      if (base::EqualsCaseInsensitiveASCII(first, "before") ||
          base::EqualsCaseInsensitiveASCII(first, "after")) {
        pseudo = base::EqualsCaseInsensitiveASCII(first, "before") ? "::before"
                                                                   : "::after";
        arg = arg.substr(comma + 1);
      }
    }
  }
  const size_t colon = arg.find(':');
  if (colon == kNpos) {
    return false;
  }
  const std::string property = base::ToLowerASCII(Trim(arg.substr(0, colon)));
  const std::string_view value = Trim(arg.substr(colon + 1));
  if (property.empty() || value.empty()) {
    return false;
  }
  BeginTask(tasks, "css");
  *tasks += ',';
  AppendString(tasks, pseudo);
  *tasks += ',';
  AppendString(tasks, property);
  *tasks += ',';
  AppendMatcher(tasks, value, /*exact=*/true, /*wildcard=*/true);
  *tasks += ']';
  return true;
}

// 去掉两头成对的引号。
std::string_view Unquote(std::string_view text) {
  if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
      text.back() == text.front()) {
    return text.substr(1, text.size() - 2);
  }
  return text;
}

// `:matches-attr(名字="值")`：名字、值可以加引号，可以是 /正则/；没有值时只看名字。
bool CompileMatchesAttr(std::string_view arg, std::string* tasks) {
  arg = Trim(arg);
  size_t name_end = kNpos;
  if (!arg.empty() && (arg.front() == '"' || arg.front() == '\'')) {
    const size_t close = arg.find(arg.front(), 1);
    if (close == kNpos) {
      return false;
    }
    name_end = close + 1;
  } else if (!arg.empty() && arg.front() == '/') {
    size_t i = 1;
    while (i < arg.size() && arg[i] != '/') {
      i += arg[i] == '\\' ? 2 : 1;
    }
    if (i >= arg.size()) {
      return false;
    }
    ++i;
    while (i < arg.size() && base::IsAsciiAlpha(arg[i])) {
      ++i;
    }
    name_end = i;
  } else {
    name_end = std::min(arg.find('='), arg.size());
  }
  const std::string_view name = Unquote(Trim(arg.substr(0, name_end)));
  std::string_view rest = Trim(arg.substr(name_end));
  if (name.empty()) {
    return false;
  }
  BeginTask(tasks, "attr");
  *tasks += ',';
  AppendMatcher(tasks, name, /*exact=*/true, /*wildcard=*/false);
  if (!rest.empty()) {
    if (rest.front() != '=') {
      return false;
    }
    *tasks += ',';
    AppendMatcher(tasks, Unquote(Trim(rest.substr(1))), /*exact=*/true,
                  /*wildcard=*/false);
  }
  *tasks += ']';
  return true;
}

bool CompileTask(Op op, const Function& f, int depth, std::string* tasks) {
  const std::string_view arg = Trim(f.arg);
  switch (op) {
    case Op::kHasText:
    case Op::kMatchesPath:
      if (arg.empty()) {
        return false;
      }
      BeginTask(tasks, op == Op::kHasText ? "text" : "path");
      *tasks += ',';
      AppendMatcher(tasks, arg, /*exact=*/false, /*wildcard=*/false);
      *tasks += ']';
      return true;
    case Op::kHas:
    case Op::kNot: {
      std::string sub;
      if (!CompileSelector(arg, /*nested=*/true, depth + 1, &sub)) {
        return false;
      }
      BeginTask(tasks, op == Op::kHas ? "has" : "not");
      *tasks += ',';
      *tasks += sub;
      *tasks += ']';
      return true;
    }
    case Op::kUpward:
    case Op::kNthAncestor: {
      unsigned n = 0;
      if (!arg.empty() && base::IsAsciiDigit(arg.front())) {
        if (!ParseCount(arg, 1, 256, &n)) {
          return false;
        }
        BeginTask(tasks, "up");
        *tasks += ',';
        *tasks += base::NumberToString(n);
        *tasks += ']';
        return true;
      }
      // `:upward(选择器)`：最近的符合的上级。
      if (op == Op::kNthAncestor || arg.empty() ||
          IsProcedural(arg, depth + 1)) {
        return false;
      }
      BeginTask(tasks, "upsel");
      *tasks += ',';
      AppendString(tasks, arg);
      *tasks += ']';
      return true;
    }
    case Op::kXpath:
    case Op::kMatchesMedia:
      if (arg.empty()) {
        return false;
      }
      BeginTask(tasks, op == Op::kXpath ? "xpath" : "media");
      *tasks += ',';
      AppendString(tasks, arg);
      *tasks += ']';
      return true;
    case Op::kMatchesCss:
    case Op::kMatchesCssBefore:
    case Op::kMatchesCssAfter:
      return CompileMatchesCss(arg, op, tasks);
    case Op::kMatchesAttr:
      return CompileMatchesAttr(arg, tasks);
    case Op::kMinTextLength: {
      unsigned n = 0;
      if (!ParseCount(arg, 0, 1u << 20, &n)) {
        return false;
      }
      BeginTask(tasks, "minlen");
      *tasks += ',';
      *tasks += base::NumberToString(n);
      *tasks += ']';
      return true;
    }
    case Op::kWatchAttr: {
      // 这些属性变了也重新找（空的是任何属性）。
      BeginTask(tasks, "watch");
      *tasks += ",[";
      bool first = true;
      for (std::string_view name : base::SplitStringPiece(
               arg, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
        if (!first) {
          *tasks += ',';
        }
        first = false;
        AppendString(tasks, Unquote(name));
      }
      *tasks += "]]";
      return true;
    }
    case Op::kOthers:
      if (!arg.empty()) {
        return false;
      }
      BeginTask(tasks, "others");
      *tasks += ']';
      return true;
    case Op::kNative:
    case Op::kUnsupported:
    case Op::kAction:
      return false;
  }
  return false;
}

// nested：在 :has()、:not() 里面，可以用组合符开头、可以没有开头的选择器（外面的元素自己）。
bool CompileSelector(std::string_view text,
                     bool nested,
                     int depth,
                     std::string* out) {
  if (depth > kMaxDepth || text.size() > kMaxSelectorLength) {
    return false;
  }
  std::vector<Function> functions;
  if (!TopLevelFunctions(text, &functions)) {
    return false;
  }
  std::string base;
  std::string tasks;
  bool started = false;
  size_t pos = 0;
  for (const Function& f : functions) {
    const Op op = Classify(f, depth);
    if (op == Op::kNative) {
      continue;
    }
    if (op == Op::kUnsupported || op == Op::kAction) {
      return false;
    }
    const std::string_view segment = text.substr(pos, f.start - pos);
    if (!started) {
      if (!CompileBase(segment, nested, /*before_op=*/true, &base)) {
        return false;
      }
      started = true;
    } else if (!CompileContinuation(segment, /*before_op=*/true, &tasks)) {
      return false;
    }
    if (!CompileTask(op, f, depth, &tasks)) {
      return false;
    }
    pos = f.end;
  }
  const std::string_view rest = text.substr(pos);
  if (!started) {
    if (!CompileBase(rest, nested, /*before_op=*/false, &base)) {
      return false;
    }
  } else if (!CompileContinuation(rest, /*before_op=*/false, &tasks)) {
    return false;
  }
  if (base.empty() && tasks.empty()) {
    return false;
  }
  out->clear();
  *out += '[';
  AppendString(out, base);
  *out += ",[";
  *out += tasks;
  *out += "]]";
  return true;
}

}  // namespace

bool SplitSelectorAction(std::string_view text,
                         std::string_view* selector,
                         ProceduralAction* action,
                         std::string_view* arg) {
  *selector = text;
  *action = ProceduralAction::kHide;
  *arg = std::string_view();
  if (!text.ends_with(')')) {
    return true;
  }
  std::vector<Function> functions;
  if (!TopLevelFunctions(text, &functions) || functions.empty() ||
      functions.back().end != text.size()) {
    // 写错了的交给后面（会跳过）。
    return true;
  }
  const Function& last = functions.back();
  const std::string_view value = Trim(last.arg);
  if (base::EqualsCaseInsensitiveASCII(last.name, "style")) {
    *action = ProceduralAction::kStyle;
  } else if (base::EqualsCaseInsensitiveASCII(last.name, "remove")) {
    *action = ProceduralAction::kRemove;
  } else if (base::EqualsCaseInsensitiveASCII(last.name, "remove-attr")) {
    *action = ProceduralAction::kRemoveAttr;
  } else if (base::EqualsCaseInsensitiveASCII(last.name, "remove-class")) {
    *action = ProceduralAction::kRemoveClass;
  } else {
    return true;
  }
  if ((*action == ProceduralAction::kRemove) != value.empty()) {
    return false;
  }
  *selector = text.substr(0, last.start);
  *arg = value;
  return !Trim(*selector).empty();
}

bool IsProceduralSelector(std::string_view selector) {
  return IsProcedural(selector, 0);
}

std::optional<std::string> CompileProceduralSelector(
    std::string_view selector) {
  std::string json;
  if (!CompileSelector(Trim(selector), /*nested=*/false, 0, &json)) {
    return std::nullopt;
  }
  return json;
}

std::optional<std::string> CompileNameMatcher(std::string_view arg) {
  const std::string_view name = Unquote(Trim(arg));
  if (name.empty()) {
    return std::nullopt;
  }
  std::string json;
  AppendMatcher(&json, name, /*exact=*/true, /*wildcard=*/false);
  return json;
}

}  // namespace starry::content_filter
