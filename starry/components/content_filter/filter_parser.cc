// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/filter_parser.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/http/http_util.h"
#include "starry/components/content_filter/html_filter.h"
#include "starry/components/content_filter/redirect_resources.h"
#include "starry/components/content_filter/request.h"
#include "starry/components/content_filter/response_filter.h"
#include "starry/components/content_filter/scriptlets.h"
#include "third_party/re2/src/re2/re2.h"

namespace starry::content_filter {

namespace {

constexpr size_t kNpos = std::string_view::npos;

std::string_view Trim(std::string_view text) {
  return base::TrimWhitespaceASCII(text, base::TRIM_ALL);
}

ParsedLine Skipped(SkipReason reason) {
  ParsedLine result;
  result.type = ParsedLine::Type::kSkipped;
  result.reason = reason;
  return result;
}

// ---- 元素隐藏 ----

enum class CosmeticSeparator {
  kHide,
  kHideException,
  kExtended,
  kExtendedException,
  kInjection,
  kInjectionException,
  kExtendedInjection,
  kScriptlet,
  kHtml,
  kAdguardScript,
};

struct CosmeticSplit {
  size_t domains_end = 0;
  size_t body_start = 0;
  CosmeticSeparator separator = CosmeticSeparator::kHide;
  bool exception = false;
};

bool IsDomainListChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
         c == '~' || c == ',' || c == '*' || c == ':' || c == '[' ||
         c == ']' || u >= 0x80;
}

bool IsDomainList(std::string_view text) {
  return std::all_of(text.begin(), text.end(), IsDomainListChar);
}

bool FindCosmetic(std::string_view line, CosmeticSplit* out) {
  struct Separator {
    std::string_view text;
    CosmeticSeparator separator;
    bool exception;
    // 正文从分隔符的哪里开始（脚本片段、HTML 过滤留着 `+js(`、`^` 给后面认）。
    size_t body_offset;
  };
  static constexpr Separator kSeparators[] = {
      {"#@#+js(", CosmeticSeparator::kScriptlet, true, 3},
      {"##+js(", CosmeticSeparator::kScriptlet, false, 2},
      {"#@#^", CosmeticSeparator::kHtml, true, 3},
      {"##^", CosmeticSeparator::kHtml, false, 2},
      {"#@$?#", CosmeticSeparator::kExtendedInjection, true, 5},
      {"#@?#", CosmeticSeparator::kExtendedException, true, 4},
      {"#@$#", CosmeticSeparator::kInjectionException, true, 4},
      {"#@%#", CosmeticSeparator::kAdguardScript, true, 4},
      {"#@#", CosmeticSeparator::kHideException, true, 3},
      {"#$?#", CosmeticSeparator::kExtendedInjection, false, 4},
      {"#?#", CosmeticSeparator::kExtended, false, 3},
      {"#$#", CosmeticSeparator::kInjection, false, 3},
      {"#%#", CosmeticSeparator::kAdguardScript, false, 3},
      {"##", CosmeticSeparator::kHide, false, 2},
  };
  for (size_t i = line.find('#'); i != kNpos; i = line.find('#', i + 1)) {
    const std::string_view rest = line.substr(i);
    for (const Separator& separator : kSeparators) {
      if (!rest.starts_with(separator.text)) {
        continue;
      }
      if (!IsDomainList(line.substr(0, i))) {
        return false;
      }
      out->domains_end = i;
      out->body_start = i + separator.body_offset;
      out->separator = separator.separator;
      out->exception = separator.exception;
      return true;
    }
  }
  // AdGuard 的 HTML 过滤：`网站$$script[…]`。
  for (std::string_view separator : {std::string_view("$@$"), std::string_view("$$")}) {
    const size_t i = line.find(separator);
    if (i != kNpos && i > 0 && IsDomainList(line.substr(0, i))) {
      out->domains_end = i;
      out->body_start = i + separator.size();
      out->separator = CosmeticSeparator::kHtml;
      return true;
    }
  }
  return false;
}

// 选择器要能安全地放进 `选择器{…}`：括号、引号配对，没有大括号和注释（不然会吞掉
// 后面的规则）。
bool IsSafeSelector(std::string_view selector) {
  if (selector.empty() || selector.size() > 4096 || selector.front() == '@') {
    return false;
  }
  int parens = 0;
  int brackets = 0;
  char quote = 0;
  for (size_t i = 0; i < selector.size(); ++i) {
    const char c = selector[i];
    if (c == '\\') {
      ++i;
      continue;
    }
    if (quote) {
      if (c == quote) {
        quote = 0;
      } else if (c == '\n' || c == '\r') {
        return false;
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
        if (--parens < 0) {
          return false;
        }
        break;
      case '[':
        ++brackets;
        break;
      case ']':
        if (--brackets < 0) {
          return false;
        }
        break;
      case '{':
      case '}':
      case ';':
        return false;
      case '/':
        if (i + 1 < selector.size() && selector[i + 1] == '*') {
          return false;
        }
        break;
      default:
        break;
    }
  }
  return parens == 0 && brackets == 0 && quote == 0;
}

// 样式声明（`:style()`、`#$#`）：不能带大括号、注释，也不许取外部资源。
bool IsSafeStyle(std::string_view style) {
  if (style.empty() || style.size() > 4096) {
    return false;
  }
  const std::string lower = base::ToLowerASCII(style);
  for (std::string_view banned :
       {std::string_view("{"), std::string_view("}"), std::string_view("/*"),
        std::string_view("url("), std::string_view("image-set("),
        std::string_view("@import"), std::string_view("expression(")}) {
    if (lower.find(banned) != kNpos) {
      return false;
    }
  }
  return true;
}

// ---- 脚本片段 ----

// uBlock Origin 的参数：逗号分开（`\,` 是逗号本身），两头的空白去掉；整个用 ' " ` 括起来
// 的参数里逗号不分开，两头的引号去掉（里面 `\引号` 是引号本身）。别的反斜杠照留
// （正则要用）。AdGuard 的 `//scriptlet('名字', '参数')` 也是这种写法。
std::vector<std::string> SplitScriptletArgs(std::string_view text) {
  std::vector<std::string> args;
  const size_t n = text.size();
  size_t i = 0;
  for (;;) {
    while (i < n && base::IsAsciiWhitespace(text[i])) {
      ++i;
    }
    std::string arg;
    bool quoted = false;
    if (i < n && (text[i] == '\'' || text[i] == '"' || text[i] == '`')) {
      const char quote = text[i];
      std::string value;
      size_t j = i + 1;
      bool closed = false;
      while (j < n) {
        if (text[j] == '\\' && j + 1 < n && text[j + 1] == quote) {
          value += quote;
          j += 2;
        } else if (text[j] == quote) {
          closed = true;
          ++j;
          break;
        } else {
          value += text[j++];
        }
      }
      size_t k = j;
      while (k < n && base::IsAsciiWhitespace(text[k])) {
        ++k;
      }
      // 引号后面还有别的字（`'a'b`）就不当引号用。
      if (closed && (k == n || text[k] == ',')) {
        arg = std::move(value);
        quoted = true;
        i = k;
      }
    }
    if (!quoted) {
      while (i < n && text[i] != ',') {
        if (text[i] == '\\' && i + 1 < n && text[i + 1] == ',') {
          arg += ',';
          i += 2;
        } else {
          arg += text[i++];
        }
      }
      arg = std::string(Trim(arg));
    }
    args.push_back(std::move(arg));
    if (i >= n) {
      break;
    }
    ++i;
  }
  return args;
}

// Adblock Plus 的脚本片段：`名字 参数 参数; 名字 …`。参数用空白分开，可以用 ' 括起来，
// `\` 转义后面一个字（`\n`、`\t` 是换行、tab）。引号没配对时返回 false。
bool SplitAbpSnippets(std::string_view text,
                      std::vector<std::vector<std::string>>* snippets) {
  std::vector<std::string> current;
  std::string token;
  bool has_token = false;
  bool in_quote = false;
  auto end_token = [&] {
    if (has_token) {
      current.push_back(std::move(token));
      token.clear();
      has_token = false;
    }
  };
  auto end_snippet = [&] {
    end_token();
    if (!current.empty()) {
      snippets->push_back(std::move(current));
      current.clear();
    }
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size()) {
      const char next = text[++i];
      token += next == 'n'   ? '\n'
               : next == 't' ? '\t'
               : next == 'r' ? '\r'
                             : next;
      has_token = true;
    } else if (in_quote) {
      if (c == '\'') {
        in_quote = false;
      } else {
        token += c;
      }
    } else if (c == '\'') {
      in_quote = true;
      has_token = true;
    } else if (c == ';') {
      end_snippet();
    } else if (base::IsAsciiWhitespace(c)) {
      end_token();
    } else {
      token += c;
      has_token = true;
    }
  }
  if (in_quote) {
    return false;
  }
  end_snippet();
  return true;
}

// 一行里的脚本片段（每条是名字加参数）。有一条认不得就整行跳过。
ParsedLine ScriptletLine(std::string_view domains,
                         bool exception,
                         std::vector<std::vector<std::string>> calls) {
  DomainSet domain_set;
  if (!domains.empty() && !domain_set.Parse(domains, ',')) {
    return Skipped(SkipReason::kDomain);
  }
  ParsedLine result;
  for (std::vector<std::string>& call : calls) {
    ScriptletFilter filter;
    filter.exception = exception;
    const std::string_view name =
        call.empty() ? std::string_view() : Trim(call.front());
    if (name.empty()) {
      // `网站#@#+js()`：这些网站上一个也不跑。
      if (!exception || calls.size() != 1 || call.size() > 1) {
        return Skipped(SkipReason::kScriptlet);
      }
      filter.scriptlet = kNoScriptlet;
    } else {
      filter.scriptlet = FindScriptlet(name);
      if (filter.scriptlet == kNoScriptlet) {
        return Skipped(SkipReason::kScriptlet);
      }
      filter.args.assign(std::make_move_iterator(call.begin() + 1),
                         std::make_move_iterator(call.end()));
      while (!filter.args.empty() && filter.args.back().empty()) {
        filter.args.pop_back();
      }
    }
    filter.domains = domain_set;
    result.scriptlets.push_back(std::move(filter));
  }
  if (result.scriptlets.empty()) {
    return Skipped(SkipReason::kScriptlet);
  }
  result.type = ParsedLine::Type::kScriptlet;
  return result;
}

// `+js(…)`、`//scriptlet(…)` 括号里面的部分。
ParsedLine ParseScriptletCall(std::string_view domains,
                              bool exception,
                              std::string_view body,
                              std::string_view prefix) {
  if (!body.starts_with(prefix) || !body.ends_with(")")) {
    return Skipped(SkipReason::kScriptlet);
  }
  body = body.substr(prefix.size(), body.size() - prefix.size() - 1);
  std::vector<std::vector<std::string>> calls;
  calls.push_back(SplitScriptletArgs(body));
  return ScriptletLine(domains, exception, std::move(calls));
}

void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// AdGuard 的 `{ remove: true; }`。
bool IsRemoveDeclaration(std::string_view style) {
  std::string compact;
  for (char c : style) {
    if (!base::IsAsciiWhitespace(c)) {
      compact += base::ToLowerASCII(c);
    }
  }
  return compact == "remove:true" || compact == "remove:true;";
}

// `网站##选择器`（`#?#` 一样），末尾可能有动作（`:style()`、`:remove()`、`:remove-attr()`、
// `:remove-class()`）。原生的隐藏、样式写成 CSS；用了扩展操作符的（和原生选择器的
// `:remove-attr()` 这类动作）编成 JSON，在网页里用脚本做。
ParsedLine ParseHiding(std::string_view domains,
                       bool exception,
                       std::string_view body) {
  std::string text(Trim(body));
  ReplaceAll(text, ":-abp-has(", ":has(");
  std::string_view selector_text;
  ProceduralAction action;
  std::string_view arg;
  if (!SplitSelectorAction(text, &selector_text, &action, &arg)) {
    return Skipped(SkipReason::kCosmetic);
  }
  const std::string_view selector = Trim(selector_text);
  ParsedLine result;
  CosmeticFilter& filter = result.cosmetic;
  filter.exception = exception;
  if (!domains.empty() && !filter.domains.Parse(domains, ',')) {
    return Skipped(SkipReason::kDomain);
  }
  const bool specific = filter.domains.has_includes();
  result.type = ParsedLine::Type::kCosmetic;

  if (!IsProceduralSelector(selector) &&
      action != ProceduralAction::kRemoveAttr &&
      action != ProceduralAction::kRemoveClass) {
    if (!IsSafeSelector(selector)) {
      return Skipped(SkipReason::kCosmetic);
    }
    filter.selector = std::string(selector);
    if (action == ProceduralAction::kStyle && exception) {
      // 样式的例外按 `选择器:style(样式)` 对（StyleExceptionKey）。
      filter.selector = StyleExceptionKey(selector, arg);
      return result;
    }
    if (action == ProceduralAction::kStyle) {
      if (!specific || !IsSafeStyle(arg)) {
        return Skipped(SkipReason::kCosmetic);
      }
      filter.kind = CosmeticFilter::Kind::kStyle;
      filter.style = std::string(arg);
    } else if (action == ProceduralAction::kRemove && !exception && specific) {
      // 样式表马上藏起来，网页里再删掉（例外按选择器对，同没有 `:remove()` 的）。
      std::optional<std::string> json = CompileProceduralSelector(selector);
      if (json) {
        filter.procedural = std::move(*json);
        filter.action = action;
      }
    }
    return result;
  }

  // 要脚本的：例外按整条的写法对。
  filter.kind = CosmeticFilter::Kind::kProcedural;
  if (exception) {
    filter.selector = std::move(text);
    return result;
  }
  // 不限网站的不要（同 uBlock Origin：每个网页都要跑脚本找，太贵）。
  if (!specific) {
    return Skipped(SkipReason::kExtendedCss);
  }
  std::optional<std::string> json = CompileProceduralSelector(selector);
  if (!json) {
    return Skipped(SkipReason::kExtendedCss);
  }
  filter.procedural = std::move(*json);
  filter.action = action;
  // selector、arg 指着 text 里面，先拷出去再把 text 移走。
  const std::string action_arg(arg);
  filter.selector = std::move(text);
  arg = action_arg;
  switch (action) {
    case ProceduralAction::kStyle:
      if (!IsSafeStyle(arg)) {
        return Skipped(SkipReason::kCosmetic);
      }
      filter.style = std::string(arg);
      break;
    case ProceduralAction::kRemoveAttr:
    case ProceduralAction::kRemoveClass: {
      std::optional<std::string> matcher = CompileNameMatcher(arg);
      if (!matcher) {
        return Skipped(SkipReason::kExtendedCss);
      }
      filter.style = std::move(*matcher);
      break;
    }
    case ProceduralAction::kHide:
    case ProceduralAction::kRemove:
      break;
  }
  return result;
}

// `##^responseheader()` 能去掉的响应头。uBlock Origin 还允许 location、set-cookie、report-to：
// 它们在网络服务里就处理了（跟跳转、存 Cookie），代理看到响应时已经来不及，跳过。
bool IsRemovableResponseHeader(std::string_view name) {
  return name == "refresh";
}

// uBlock Origin 的 HTML 过滤：`网站##^选择器`（浏览器从页面的源码里删掉找到的元素，
// html_filter.h）、`网站##^responseheader(名字)`（去掉页面的响应头）。都只收限定网站的。
// AdGuard 的 `网站$$…` 是另一套写法（`[tag-content="…"]` 这些），跳过。
ParsedLine ParseHtmlFilter(std::string_view domains,
                           bool exception,
                           std::string_view body) {
  if (!body.starts_with('^')) {
    return Skipped(SkipReason::kHtmlFilter);
  }
  std::string text(Trim(body.substr(1)));
  ReplaceAll(text, ":-abp-has(", ":has(");
  if (text.empty()) {
    return Skipped(SkipReason::kHtmlFilter);
  }
  ParsedLine result;
  CosmeticFilter& filter = result.cosmetic;
  filter.exception = exception;
  if (!domains.empty() && !filter.domains.Parse(domains, ',')) {
    return Skipped(SkipReason::kDomain);
  }
  const bool specific = filter.domains.has_includes();
  result.type = ParsedLine::Type::kCosmetic;
  constexpr std::string_view kResponseHeader = "responseheader(";
  if (base::StartsWith(text, kResponseHeader,
                       base::CompareCase::INSENSITIVE_ASCII) &&
      text.ends_with(')')) {
    const std::string name =
        base::ToLowerASCII(Trim(std::string_view(text).substr(
            kResponseHeader.size(), text.size() - kResponseHeader.size() - 1)));
    if (!net::HttpUtil::IsValidHeaderName(name) ||
        (!exception && (!specific || !IsRemovableResponseHeader(name)))) {
      return Skipped(SkipReason::kHtmlFilter);
    }
    filter.kind = CosmeticFilter::Kind::kResponseHeader;
    filter.selector = "^responseheader(" + name + ")";
    filter.style = name;
    return result;
  }
  filter.kind = CosmeticFilter::Kind::kHtml;
  filter.selector = "^" + text;
  if (exception) {
    return result;
  }
  // 不限网站的不要（同 uBlock Origin）。
  std::string_view selector;
  ProceduralAction action;
  std::string_view arg;
  if (!specific || !SplitSelectorAction(text, &selector, &action, &arg) ||
      action != ProceduralAction::kHide) {
    return Skipped(SkipReason::kHtmlFilter);
  }
  std::optional<std::string> spec = CompileProceduralSelector(selector);
  if (!spec || !HtmlSelector::Compile(*spec)) {
    return Skipped(SkipReason::kHtmlFilter);
  }
  filter.procedural = std::move(*spec);
  return result;
}

ParsedLine ParseCosmetic(std::string_view line, const CosmeticSplit& split) {
  const std::string_view domains = line.substr(0, split.domains_end);
  std::string_view body = Trim(line.substr(split.body_start));
  switch (split.separator) {
    case CosmeticSeparator::kScriptlet:
      return ParseScriptletCall(domains, split.exception, body, "+js(");
    case CosmeticSeparator::kAdguardScript:
      // 别的 `#%#`（任意的脚本）不跑。
      return ParseScriptletCall(domains, split.exception, body, "//scriptlet(");
    case CosmeticSeparator::kHtml:
      return ParseHtmlFilter(domains, split.exception, body);
    case CosmeticSeparator::kInjection:
    case CosmeticSeparator::kInjectionException:
    case CosmeticSeparator::kExtendedInjection: {
      // AdGuard 的 `#$#选择器 { 样式 }`、`#$?#`（扩展选择器），`{ remove: true; }` 是删掉：
      // 同 `##选择器:style(样式)`、`##选择器:remove()`。Adblock Plus 的 `#$#` 是脚本片段
      // （没有大括号）。
      const size_t brace = body.rfind('{');
      if (brace == kNpos || !body.ends_with("}")) {
        if (split.separator == CosmeticSeparator::kExtendedInjection) {
          return Skipped(SkipReason::kCosmetic);
        }
        std::vector<std::vector<std::string>> snippets;
        if (!SplitAbpSnippets(body, &snippets)) {
          return Skipped(SkipReason::kScriptlet);
        }
        return ScriptletLine(domains, split.exception, std::move(snippets));
      }
      const std::string_view style =
          Trim(body.substr(brace + 1, body.size() - brace - 2));
      std::string text(Trim(body.substr(0, brace)));
      if (IsRemoveDeclaration(style)) {
        text += ":remove()";
      } else {
        text += ":style(";
        text.append(style);
        text += ')';
      }
      return ParseHiding(domains, split.exception, text);
    }
    case CosmeticSeparator::kHide:
    case CosmeticSeparator::kHideException:
    case CosmeticSeparator::kExtended:
    case CosmeticSeparator::kExtendedException:
      return ParseHiding(domains, split.exception, body);
  }
  return Skipped(SkipReason::kCosmetic);
}

// ---- hosts 文件 ----

bool IsHostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '-' || c == '_';
}

bool IsSinkAddress(std::string_view text) {
  static constexpr std::string_view kSinks[] = {
      "0.0.0.0", "127.0.0.1", "::", "::1", "::0", "0", "255.255.255.255",
      "0:0:0:0:0:0:0:0", "fe80::1%lo0",
  };
  return std::find(std::begin(kSinks), std::end(kSinks), text) !=
         std::end(kSinks);
}

bool IsIgnoredHostsEntry(std::string_view host) {
  return host == "localhost" || host == "localhost.localdomain" ||
         host == "local" || host == "broadcasthost" || host == "0.0.0.0" ||
         host.starts_with("ip6-");
}

NetworkFilter HostFilter(std::string_view host) {
  NetworkFilter filter;
  filter.flags = NetworkFilter::kHostAnchor | NetworkFilter::kWildcard;
  filter.pattern = std::string(host) + "^";
  filter.host_length = static_cast<uint16_t>(host.size());
  return filter;
}

// 只有一个网站名的一行（uBlock Origin 当作 `||网站^`）：要有公认的顶级域名，免得把
// `banner.gif` 这种当成网站。
bool IsPlainHostname(std::string_view text) {
  if (text.size() < 4 || text.size() > 253 || text.front() == '.' ||
      text.front() == '-' || text.back() == '.' ||
      text.find('.') == kNpos || text.find("..") != kNpos) {
    return false;
  }
  if (!std::all_of(text.begin(), text.end(), IsHostChar)) {
    return false;
  }
  const size_t registry =
      net::registry_controlled_domains::GetCanonicalHostRegistry(
          text, net::registry_controlled_domains::EXCLUDE_UNKNOWN_REGISTRIES,
          net::registry_controlled_domains::EXCLUDE_PRIVATE_REGISTRIES)
          .value_or(std::string_view())
          .size();
  return registry > 0 && registry + 1 < text.size();
}

bool ParseHosts(std::string_view line, ParsedLine* out) {
  // 大多数行是一般的网络规则：不用切开就能知道不是 hosts。
  if (line.find_first_of(" \t") == kNpos) {
    if (!std::all_of(line.begin(), line.end(), [](char c) {
          return IsHostChar(base::ToLowerASCII(c));
        })) {
      return false;
    }
    const std::string lower = base::ToLowerASCII(line);
    if (!IsPlainHostname(lower)) {
      return false;
    }
    out->network.push_back(HostFilter(lower));
    out->type = ParsedLine::Type::kNetwork;
    return true;
  }
  if (const size_t comment = line.find('#'); comment != kNpos) {
    line = line.substr(0, comment);
  }
  std::vector<std::string_view> tokens = base::SplitStringPiece(
      line, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (tokens.empty()) {
    return false;
  }
  if (tokens.size() >= 2 && IsSinkAddress(tokens[0])) {
    for (size_t i = 1; i < tokens.size(); ++i) {
      const std::string host = NormalizeHost(tokens[i], /*strip_www=*/false);
      if (host.empty() || IsIgnoredHostsEntry(host) ||
          host.size() > 0xffff) {
        continue;
      }
      out->network.push_back(HostFilter(host));
    }
    out->type = out->network.empty() ? ParsedLine::Type::kNone
                                     : ParsedLine::Type::kNetwork;
    return true;
  }
  if (tokens.size() == 1) {
    const std::string lower = base::ToLowerASCII(tokens[0]);
    if (IsPlainHostname(lower)) {
      out->network.push_back(HostFilter(lower));
      out->type = ParsedLine::Type::kNetwork;
      return true;
    }
  }
  return false;
}

// ---- 网络规则 ----

struct TypeName {
  std::string_view name;
  uint32_t type;
};

constexpr TypeName kTypeNames[] = {
    {"script", kTypeScript},
    {"image", kTypeImage},
    {"stylesheet", kTypeStylesheet},
    {"css", kTypeStylesheet},
    {"object", kTypeObject},
    {"object-subrequest", kTypeObject},
    {"xmlhttprequest", kTypeXmlHttpRequest},
    {"xhr", kTypeXmlHttpRequest},
    {"subdocument", kTypeSubdocument},
    {"frame", kTypeSubdocument},
    {"ping", kTypePing},
    {"beacon", kTypePing},
    {"media", kTypeMedia},
    {"font", kTypeFont},
    {"websocket", kTypeWebSocket},
    {"other", kTypeOther},
    {"popup", kTypePopup},
};

struct Options {
  uint32_t include_types = 0;
  uint32_t exclude_types = 0;
  uint32_t party = 0;
  bool strict_party = false;
  bool match_case = false;
  bool important = false;
  bool badfilter = false;
  bool document = false;
  std::unique_ptr<DomainSet> domains;
  std::unique_ptr<DomainSet> to_domains;
  uint16_t methods = 0;
  bool methods_negated = false;
  std::optional<std::string> redirect;
  bool redirect_rule_only = false;
  std::optional<std::string> csp;
  std::optional<std::string> removeparam;
  std::optional<std::string> replace;
  uint32_t doc_flags = 0;
  // 原样的选项（$badfilter 拼出要去掉的那条）。
  std::vector<std::string_view> raw;
};

bool IsOptionNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// pos 处的逗号分不分开选项：后面像一个选项（`名字`、`~名字`、`名字=…`）或者是空的才分开，
// 同 uBlock Origin（`$replace=/a,b/[null,null]/`、`$removeparam=/x{1,3}/` 的值里有逗号）。
bool IsOptionSeparator(std::string_view text, size_t pos) {
  size_t i = pos + 1;
  if (i < text.size() && text[i] == '~') {
    ++i;
  }
  const size_t start = i;
  while (i < text.size() && IsOptionNameChar(text[i])) {
    ++i;
  }
  if (i == text.size() || text[i] == ',') {
    return true;
  }
  return i > start && text[i] == '=';
}

// 按逗号分开选项（`\,` 是值里的逗号）。
std::vector<std::string> SplitOptions(std::string_view text) {
  std::vector<std::string> parts(1);
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size() && text[i + 1] == ',') {
      parts.back().push_back(',');
      ++i;
    } else if (c == ',' && IsOptionSeparator(text, i)) {
      parts.emplace_back();
    } else {
      parts.back().push_back(c);
    }
  }
  return parts;
}

// `$` 后面像不像选项：第一个选项名只有字母、数字、`-`、`_`（前面可以有 `~`）。
bool LooksLikeOptions(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  size_t i = text.front() == '~' ? 1 : 0;
  const size_t start = i;
  while (i < text.size() && IsOptionNameChar(text[i])) {
    ++i;
  }
  if (i == start) {
    return false;
  }
  return i == text.size() || text[i] == ',' || text[i] == '=';
}

size_t FindOptionsStart(std::string_view text) {
  for (size_t pos = text.rfind('$'); pos != kNpos;
       pos = pos == 0 ? kNpos : text.rfind('$', pos - 1)) {
    // `\$` 是值里的（`$replace=/…/\$1,…/`）。
    if (pos > 0 && text[pos - 1] == '\\') {
      continue;
    }
    if (LooksLikeOptions(text.substr(pos + 1))) {
      return pos;
    }
  }
  return kNpos;
}

bool ParseMethods(std::string_view value, Options* options) {
  for (std::string_view method : base::SplitStringPiece(
           value, "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    bool negated = false;
    if (method.starts_with("~")) {
      negated = true;
      method.remove_prefix(1);
    }
    const uint16_t bit = MethodFromName(method);
    if (!bit || (options->methods && negated != options->methods_negated)) {
      return false;
    }
    options->methods |= bit;
    options->methods_negated = negated;
  }
  return options->methods != 0;
}

bool ParseOptions(std::string_view text,
                  bool exception,
                  Options* options,
                  SkipReason* reason) {
  *reason = SkipReason::kOption;
  const std::vector<std::string> parts = SplitOptions(text);
  for (const std::string& part : parts) {
    std::string_view option = Trim(part);
    if (option.empty()) {
      continue;
    }
    bool negated = false;
    if (option.front() == '~') {
      negated = true;
      option.remove_prefix(1);
    }
    std::string_view value;
    if (const size_t equals = option.find('='); equals != kNpos) {
      value = option.substr(equals + 1);
      option = option.substr(0, equals);
    }
    const std::string name = base::ToLowerASCII(option);
    if (const auto* type =
            std::find_if(std::begin(kTypeNames), std::end(kTypeNames),
                         [&](const TypeName& t) { return t.name == name; });
        type != std::end(kTypeNames)) {
      (negated ? options->exclude_types : options->include_types) |=
          type->type;
      continue;
    }
    if (name == "third-party" || name == "3p") {
      options->party |=
          negated ? NetworkFilter::kFirstParty : NetworkFilter::kThirdParty;
    } else if (name == "first-party" || name == "1p") {
      options->party |=
          negated ? NetworkFilter::kThirdParty : NetworkFilter::kFirstParty;
    } else if (name == "strict3p") {
      options->party |= NetworkFilter::kThirdParty;
      options->strict_party = true;
    } else if (name == "strict1p") {
      options->party |= NetworkFilter::kFirstParty;
      options->strict_party = true;
    } else if (name == "match-case") {
      options->match_case = !negated;
    } else if (name == "important") {
      options->important = true;
    } else if (name == "badfilter") {
      options->badfilter = true;
    } else if (name == "domain" || name == "from") {
      options->domains = std::make_unique<DomainSet>();
      if (!options->domains->Parse(value, '|')) {
        *reason = SkipReason::kDomain;
        return false;
      }
    } else if (name == "to") {
      options->to_domains = std::make_unique<DomainSet>();
      if (!options->to_domains->Parse(value, '|')) {
        *reason = SkipReason::kDomain;
        return false;
      }
    } else if (name == "denyallow") {
      // 除了这些网站的请求。
      options->to_domains = std::make_unique<DomainSet>();
      for (std::string_view host : base::SplitStringPiece(
               value, "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
        if (host.starts_with("~") || host.starts_with("/")) {
          *reason = SkipReason::kDomain;
          return false;
        }
        options->to_domains->AddExclude(host);
      }
    } else if (name == "method") {
      if (!ParseMethods(value, options)) {
        return false;
      }
    } else if (name == "redirect" || name == "redirect-rule") {
      options->redirect = std::string(value);
      options->redirect_rule_only = name == "redirect-rule";
    } else if (name == "rewrite" && value.starts_with("abp-resource:")) {
      options->redirect = std::string(value);
    } else if (name == "empty") {
      options->redirect = "empty";
    } else if (name == "mp4") {
      options->redirect = "noopmp4-1s";
    } else if (name == "csp") {
      options->csp = std::string(value);
    } else if (name == "inline-script") {
      options->csp = "script-src 'unsafe-eval' * blob: data:";
    } else if (name == "inline-font") {
      options->csp = "font-src *";
    } else if (name == "removeparam" || name == "queryprune") {
      options->removeparam = std::string(value);
    } else if (name == "replace" && !negated) {
      options->replace = std::string(value);
    } else if (name == "document" || name == "doc") {
      if (exception) {
        options->doc_flags |= kDocDocument;
      } else {
        options->document = true;
      }
    } else if (name == "elemhide" || name == "ehide") {
      options->doc_flags |= kDocElemHide;
    } else if (name == "generichide" || name == "ghide") {
      options->doc_flags |= kDocGenericHide;
    } else if (name == "specifichide" || name == "shide") {
      options->doc_flags |= kDocSpecificHide;
    } else if (name == "genericblock") {
      options->doc_flags |= kDocGenericBlock;
    } else if (name == "urlblock") {
      options->doc_flags |= kDocUrlBlock;
    } else if (name == "jsinject") {
      options->doc_flags |= kDocJsInject;
    } else if (name == "all") {
      options->include_types |= kNetworkTypes | kTypePopup;
      options->document = !exception;
    } else if (name == "collapse" ||
               name.find_first_not_of('_') == std::string::npos) {
      // 不影响拦不拦。
    } else {
      // 不认识的、做不到的（$header、$permissions、$urlskip……）：整条跳过，不猜意思。
      return false;
    }
  }
  // 整页例外只能写在例外上。
  if (options->doc_flags && !exception) {
    return false;
  }
  if (options->badfilter) {
    // 原样的选项（拼出要去掉的那条）。
    options->raw = base::SplitStringPiece(text, ",", base::KEEP_WHITESPACE,
                                          base::SPLIT_WANT_ALL);
  }
  return true;
}

bool HasRegexMeta(std::string_view text) {
  return text.find_first_of("\\^$*+?.()[]{}|") != kNpos;
}

// 网址模式：去掉锚，规范成内部的写法（见 NetworkFilter）。
bool BuildPattern(std::string_view text,
                  bool match_case,
                  NetworkFilter* filter,
                  SkipReason* reason) {
  if (text.size() > 2 && text.front() == '/' && text.back() == '/') {
    const std::string_view body = text.substr(1, text.size() - 2);
    if (!HasRegexMeta(body)) {
      // `/ads/` 就是包含 ads（Adblock Plus 的写法：两头的斜杠是正则）。
      filter->pattern = match_case ? std::string(body) : base::ToLowerASCII(body);
      if (match_case) {
        filter->flags |= NetworkFilter::kMatchCase;
      }
      return true;
    }
    re2::RE2::Options options;
    options.set_case_sensitive(match_case);
    options.set_never_capture(true);
    options.set_log_errors(false);
    options.set_max_mem(1 << 20);
    auto regex = std::make_unique<re2::RE2>(body, options);
    if (!regex->ok()) {
      *reason = SkipReason::kRegex;
      return false;
    }
    filter->flags |= NetworkFilter::kRegex;
    if (match_case) {
      filter->flags |= NetworkFilter::kMatchCase;
    }
    filter->regex = std::move(regex);
    return true;
  }
  if (text.starts_with("||")) {
    filter->flags |= NetworkFilter::kHostAnchor;
    text.remove_prefix(2);
  } else if (text.starts_with("|")) {
    filter->flags |= NetworkFilter::kLeftAnchor;
    text.remove_prefix(1);
  }
  if (text.ends_with("|")) {
    filter->flags |= NetworkFilter::kRightAnchor;
    text.remove_suffix(1);
  }
  std::string pattern;
  pattern.reserve(text.size());
  for (char c : text) {
    if (c == '*' && !pattern.empty() && pattern.back() == '*') {
      continue;
    }
    pattern.push_back(c);
  }
  while (!pattern.empty() && pattern.front() == '*') {
    pattern.erase(0, 1);
    filter->flags &= ~(NetworkFilter::kLeftAnchor | NetworkFilter::kHostAnchor);
  }
  while (!pattern.empty() && pattern.back() == '*') {
    pattern.pop_back();
    filter->flags &= ~NetworkFilter::kRightAnchor;
  }
  if (match_case) {
    filter->flags |= NetworkFilter::kMatchCase;
  } else {
    pattern = base::ToLowerASCII(pattern);
  }
  if (pattern.find_first_of("*^") != std::string::npos) {
    filter->flags |= NetworkFilter::kWildcard;
  }
  if ((filter->flags & NetworkFilter::kHostAnchor) && !match_case) {
    size_t n = 0;
    while (n < pattern.size() && IsHostChar(pattern[n])) {
      ++n;
    }
    if (n > 0 && n < pattern.size() && n <= 0xffff &&
        (pattern[n] == '^' || pattern[n] == '/' || pattern[n] == ':') &&
        pattern[0] != '.' && pattern[n - 1] != '.') {
      filter->host_length = static_cast<uint16_t>(n);
    }
  }
  filter->pattern = std::move(pattern);
  return true;
}

std::unique_ptr<re2::RE2> CloneRegex(const std::unique_ptr<re2::RE2>& regex) {
  if (!regex) {
    return nullptr;
  }
  return std::make_unique<re2::RE2>(regex->pattern(), regex->options());
}

NetworkFilter Clone(const NetworkFilter& source) {
  NetworkFilter copy;
  copy.kind = source.kind;
  copy.flags = source.flags;
  copy.types = source.types;
  copy.doc_flags = source.doc_flags;
  copy.methods = source.methods;
  copy.methods_negated = source.methods_negated;
  copy.list = source.list;
  copy.host_length = source.host_length;
  copy.pattern = source.pattern;
  copy.value = source.value;
  copy.regex = CloneRegex(source.regex);
  copy.value_regex = CloneRegex(source.value_regex);
  if (source.replace) {
    copy.replace = ReplaceFilter::Parse(source.value);
  }
  if (source.domains) {
    copy.domains = std::make_unique<DomainSet>(*source.domains);
  }
  if (source.to_domains) {
    copy.to_domains = std::make_unique<DomainSet>(*source.to_domains);
  }
  return copy;
}

std::string BadfilterTarget(std::string_view line_without_options,
                            const std::vector<std::string_view>& raw) {
  std::string target(line_without_options);
  bool first = true;
  for (std::string_view option : raw) {
    if (base::EqualsCaseInsensitiveASCII(Trim(option), "badfilter")) {
      continue;
    }
    target += first ? "$" : ",";
    target += option;
    first = false;
  }
  return target;
}

ParsedLine ParseNetwork(std::string_view line) {
  std::string_view text = line;
  const bool exception = text.starts_with("@@");
  if (exception) {
    text.remove_prefix(2);
  }
  const size_t dollar = FindOptionsStart(text);
  const std::string_view pattern_text =
      dollar == kNpos ? text : text.substr(0, dollar);
  const std::string_view options_text =
      dollar == kNpos ? std::string_view() : text.substr(dollar + 1);

  Options options;
  SkipReason reason = SkipReason::kOption;
  if (!options_text.empty() &&
      !ParseOptions(options_text, exception, &options, &reason)) {
    return Skipped(reason);
  }

  NetworkFilter filter;
  if (exception) {
    filter.flags |= NetworkFilter::kException;
  }
  if (!BuildPattern(pattern_text, options.match_case, &filter, &reason)) {
    return Skipped(reason);
  }
  if (options.important) {
    filter.flags |= NetworkFilter::kImportant;
  }
  filter.flags |= options.party;
  if (options.strict_party) {
    filter.flags |= NetworkFilter::kStrictParty;
  }
  if (options.document) {
    options.include_types |= kTypeDocument;
  }
  uint32_t types = kNetworkTypes;
  if (options.include_types) {
    types = options.include_types & ~options.exclude_types;
    filter.flags |= NetworkFilter::kExplicitTypes;
  } else if (options.exclude_types) {
    types = kNetworkTypes & ~options.exclude_types;
    filter.flags |= NetworkFilter::kExplicitTypes;
  }
  filter.doc_flags = options.doc_flags;
  if (filter.doc_flags) {
    // 整页例外只拿页面（顶层和 iframe 的文档）来对。
    types = kTypeDocument | kTypeSubdocument;
  }
  filter.methods = options.methods;
  filter.methods_negated = options.methods_negated;
  filter.domains = std::move(options.domains);
  filter.to_domains = std::move(options.to_domains);

  ParsedLine result;
  if (options.badfilter) {
    result.badfilter = true;
    result.badfilter_target = BadfilterTarget(
        line.substr(0, (exception ? 2 : 0) + pattern_text.size()),
        options.raw);
    return result;
  }

  // 什么都不限制的规则（`*`、`@@|`）会拦下或者放行所有请求：不要。加策略、去参数
  // 的可以不限网址（`$removeparam=utm_source`）。
  const bool restricted =
      options.csp || options.removeparam || !filter.pattern.empty() ||
      filter.regex || filter.domains ||
      filter.to_domains || filter.doc_flags || filter.methods ||
      (filter.flags & (NetworkFilter::kFirstParty | NetworkFilter::kThirdParty)) ||
      types != kNetworkTypes;
  if (!restricted) {
    return Skipped(SkipReason::kPattern);
  }

  const int directives =
      options.redirect.has_value() + options.csp.has_value() +
      options.removeparam.has_value() + options.replace.has_value();
  if (directives > 1) {
    return Skipped(SkipReason::kOption);
  }
  const bool directive = directives > 0;
  // 拦整个页面（$document 拦截）：顶层页面不拦。
  if (!exception && !directive && (types & kTypeDocument)) {
    types &= ~kTypeDocument;
    if (!(types & kAllTypes)) {
      return Skipped(SkipReason::kDocumentBlock);
    }
  }
  filter.types = types;

  if (options.redirect) {
    std::string_view name;
    if (!(exception && options.redirect->empty())) {
      name = CanonicalRedirectName(*options.redirect);
      if (name.empty()) {
        return Skipped(SkipReason::kRedirect);
      }
    }
    if (!exception && !options.redirect_rule_only) {
      result.network.push_back(Clone(filter));
    }
    filter.kind = NetworkFilterKind::kRedirect;
    filter.value = std::string(name);
    filter.flags &= ~NetworkFilter::kImportant;
  } else if (options.csp) {
    if (!exception && options.csp->empty()) {
      return Skipped(SkipReason::kOption);
    }
    filter.kind = NetworkFilterKind::kCsp;
    filter.value = std::move(*options.csp);
    if (!(filter.flags & NetworkFilter::kExplicitTypes)) {
      filter.types = kTypeDocument | kTypeSubdocument;
    }
  } else if (options.removeparam) {
    filter.kind = NetworkFilterKind::kRemoveParam;
    filter.value = std::move(*options.removeparam);
    const std::string_view value = filter.value;
    if (value.size() > 2 && value.front() == '/' && value.back() == '/') {
      re2::RE2::Options regex_options;
      regex_options.set_never_capture(true);
      regex_options.set_log_errors(false);
      regex_options.set_max_mem(1 << 18);
      filter.value_regex = std::make_unique<re2::RE2>(
          value.substr(1, value.size() - 2), regex_options);
      if (!filter.value_regex->ok()) {
        return Skipped(SkipReason::kRegex);
      }
    }
    if (!(filter.flags & NetworkFilter::kExplicitTypes)) {
      filter.types = kTypeDocument | kTypeSubdocument;
    }
  } else if (options.replace) {
    // 能改的响应（ReplaceFilter）；信不过的列表里的由 FilterEngine::Build 跳过。
    filter.kind = NetworkFilterKind::kReplace;
    filter.value = std::move(*options.replace);
    if (!exception) {
      filter.replace = ReplaceFilter::Parse(filter.value);
      if (!filter.replace) {
        return Skipped(SkipReason::kRegex);
      }
    }
    filter.flags &= ~NetworkFilter::kImportant;
    if (!(filter.flags & NetworkFilter::kExplicitTypes)) {
      // 不写类型的也管页面（同 uBlock Origin）。
      filter.types = kNetworkTypes | kTypeDocument;
    }
  }
  result.network.push_back(std::move(filter));
  result.type = ParsedLine::Type::kNetwork;
  return result;
}

}  // namespace

CosmeticFilter::CosmeticFilter() = default;
CosmeticFilter::CosmeticFilter(CosmeticFilter&&) = default;
CosmeticFilter& CosmeticFilter::operator=(CosmeticFilter&&) = default;
CosmeticFilter::~CosmeticFilter() = default;

ScriptletFilter::ScriptletFilter() : scriptlet(kNoScriptlet) {}
ScriptletFilter::ScriptletFilter(ScriptletFilter&&) = default;
ScriptletFilter& ScriptletFilter::operator=(ScriptletFilter&&) = default;
ScriptletFilter::~ScriptletFilter() = default;

ParsedLine::ParsedLine() = default;
ParsedLine::ParsedLine(ParsedLine&&) = default;
ParsedLine& ParsedLine::operator=(ParsedLine&&) = default;
ParsedLine::~ParsedLine() = default;

std::string HostFilterText(std::string_view host) {
  return "||" + std::string(host) + "^";
}

std::string StyleExceptionKey(std::string_view selector,
                              std::string_view style) {
  std::string key(selector);
  key += ":style(";
  key.append(style);
  key += ')';
  return key;
}

ParsedLine ParseFilterLine(std::string_view raw) {
  const std::string_view line = Trim(raw);
  if (line.empty() || line.front() == '!' ||
      (line.front() == '[' && line.back() == ']')) {
    return ParsedLine();
  }
  CosmeticSplit split;
  if (FindCosmetic(line, &split)) {
    return ParseCosmetic(line, split);
  }
  if (line.front() == '#') {
    // hosts 文件的注释。
    return ParsedLine();
  }
  ParsedLine hosts;
  if (ParseHosts(line, &hosts)) {
    return hosts;
  }
  return ParseNetwork(line);
}

}  // namespace starry::content_filter
