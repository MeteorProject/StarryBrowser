// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/filter_list.h"

#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace starry::content_filter {

namespace {

constexpr std::string_view kInclude = "!#include";

std::string_view Trim(std::string_view text) {
  return base::TrimWhitespaceASCII(text, base::TRIM_ALL);
}

std::string_view StripUtf8Bom(std::string_view text) {
  return text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text;
}

// 一行一行（`\n`、`\r\n`、`\r` 都算换行）。
template <typename Visitor>
void ForEachLine(std::string_view text, Visitor&& visit) {
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find_first_of("\r\n", start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    if (!visit(text.substr(start, end - start))) {
      return;
    }
    start = end + 1;
    if (end < text.size() && text[end] == '\r' && start < text.size() &&
        text[start] == '\n') {
      ++start;
    }
  }
}

std::optional<base::TimeDelta> ParseExpires(std::string_view value) {
  const std::string lower = base::ToLowerASCII(value);
  size_t digits = 0;
  while (digits < lower.size() && base::IsAsciiDigit(lower[digits])) {
    ++digits;
  }
  int number = 0;
  if (!base::StringToInt(std::string_view(lower).substr(0, digits), &number) ||
      number <= 0) {
    return std::nullopt;
  }
  const std::string_view rest = Trim(std::string_view(lower).substr(digits));
  if (rest.starts_with("h")) {
    return base::Hours(number);
  }
  if (rest.starts_with("d") || rest.empty()) {
    return base::Days(number);
  }
  return std::nullopt;
}

// `!#if` 的条件：标识、!、&&、||、括号。
class ConditionParser {
 public:
  // html_filtering：cap_html_filtering 成不成立（ForEachActiveLine 里说为什么分开）。
  ConditionParser(std::string_view text, bool html_filtering)
      : text_(text), html_filtering_(html_filtering) {}

  bool Parse() { return ParseOr(); }

 private:
  void SkipSpaces() {
    while (pos_ < text_.size() && text_[pos_] == ' ') {
      ++pos_;
    }
  }

  bool Consume(std::string_view token) {
    SkipSpaces();
    if (text_.substr(pos_).starts_with(token)) {
      pos_ += token.size();
      return true;
    }
    return false;
  }

  bool ParseOr() {
    bool value = ParseAnd();
    while (Consume("||")) {
      value = ParseAnd() || value;
    }
    return value;
  }

  bool ParseAnd() {
    bool value = ParseUnary();
    while (Consume("&&")) {
      value = ParseUnary() && value;
    }
    return value;
  }

  bool ParseUnary() {
    if (Consume("!")) {
      return !ParseUnary();
    }
    if (Consume("(")) {
      const bool value = ParseOr();
      Consume(")");
      return value;
    }
    SkipSpaces();
    const size_t start = pos_;
    while (pos_ < text_.size() &&
           (base::IsAsciiAlphaNumeric(text_[pos_]) || text_[pos_] == '_')) {
      ++pos_;
    }
    return IsTrue(text_.substr(start, pos_ - start));
  }

  bool IsTrue(std::string_view name) const {
    return name == "env_chromium" || name == "ext_ublock" ||
           name == "cap_user_stylesheet" ||
           (html_filtering_ && name == "cap_html_filtering");
  }

  std::string_view text_;
  const bool html_filtering_;
  size_t pos_ = 0;
};

// HTML 过滤的一行（`网站##^…`、`网站#@#^…`）。
bool IsHtmlFilterLine(std::string_view line) {
  return line.find("##^") != std::string_view::npos ||
         line.find("#@#^") != std::string_view::npos;
}

}  // namespace

bool EvaluatePreprocessorCondition(std::string_view condition) {
  return ConditionParser(Trim(condition), /*html_filtering=*/false).Parse();
}

FilterListMetadata ParseMetadata(std::string_view text) {
  FilterListMetadata metadata;
  text = StripUtf8Bom(text);
  int lines = 0;
  ForEachLine(text, [&](std::string_view raw) {
    if (++lines > 200) {
      return false;
    }
    std::string_view line = Trim(raw);
    if (line.empty() || line.starts_with("[") || line.starts_with("!#")) {
      return true;
    }
    if (!line.starts_with("!")) {
      return false;
    }
    line = Trim(line.substr(1));
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      return true;
    }
    const std::string key = base::ToLowerASCII(Trim(line.substr(0, colon)));
    const std::string_view value = Trim(line.substr(colon + 1));
    if (value.empty()) {
      return true;
    }
    if (key == "title" && !metadata.title) {
      metadata.title = std::string(value);
    } else if (key == "homepage" && !metadata.homepage) {
      metadata.homepage = std::string(value);
    } else if (key == "version" && !metadata.version) {
      metadata.version = std::string(value);
    } else if (key == "expires" && !metadata.expires) {
      metadata.expires = ParseExpires(value);
    }
    return true;
  });
  return metadata;
}

bool LooksLikeFilterList(std::string_view text) {
  text = StripUtf8Bom(text);
  if (base::TrimWhitespaceASCII(text, base::TRIM_LEADING).starts_with("<")) {
    return false;
  }
  bool found = false;
  int lines = 0;
  ForEachLine(text, [&](std::string_view raw) {
    if (++lines > 5000) {
      return false;
    }
    const std::string_view line = Trim(raw);
    if (line.starts_with(kInclude)) {
      found = true;
      return false;
    }
    if (line.empty() || line.starts_with("!") || line.starts_with("[") ||
        (line.starts_with("#") && !line.starts_with("##") &&
         !line.starts_with("#@#") && !line.starts_with("#?#"))) {
      return true;
    }
    found = true;
    return false;
  });
  return found;
}

std::vector<std::string> IncludeDirectives(std::string_view text) {
  std::vector<std::string> result;
  if (text.find(kInclude) == std::string_view::npos) {
    return result;
  }
  ForEachLine(text, [&](std::string_view raw) {
    const std::string_view line = Trim(raw);
    if (line.starts_with(kInclude)) {
      const std::string_view path = Trim(line.substr(kInclude.size()));
      if (!path.empty()) {
        result.emplace_back(path);
      }
    }
    return true;
  });
  return result;
}

std::string ExpandIncludes(
    std::string_view text,
    base::FunctionRef<std::optional<std::string>(std::string_view)> fetch) {
  if (text.find(kInclude) == std::string_view::npos) {
    return std::string(text);
  }
  std::string output;
  output.reserve(text.size());
  ForEachLine(text, [&](std::string_view raw) {
    const std::string_view line = Trim(raw);
    if (line.starts_with(kInclude)) {
      if (std::optional<std::string> included =
              fetch(Trim(line.substr(kInclude.size())))) {
        output += *included;
        output += '\n';
      }
      return true;
    }
    output.append(raw);
    output += '\n';
    return true;
  });
  return output;
}

void ForEachActiveLine(std::string_view text,
                       base::FunctionRef<void(std::string_view)> visit,
                       bool replace_allowed) {
  // 信不过的列表条件求两遍：uBlock Origin 的 cap_html_filtering 不只管 HTML 过滤，还管
  // `$replace`（这样的列表里跳过，`!#else` 里有替代的脚本片段）。HTML 过滤的行按成立求值
  // （引擎做得到），别的行按不成立。两边都有的话都用：`##^script:has-text(x)` 和替代的
  // `##+js(rmnt, script, x)` 是同一个意思。信得过的列表两遍都按成立（同 Firefox 上的
  // uBlock Origin）。
  struct Level {
    bool value;
    bool html_value;
  };
  std::vector<Level> levels;
  size_t false_depth = 0;
  size_t html_false_depth = 0;
  ForEachLine(text, [&](std::string_view raw) {
    const std::string_view line = Trim(raw);
    if (line.starts_with("!#")) {
      if (line.starts_with("!#if")) {
        const std::string_view condition = Trim(line.substr(4));
        const bool html_value =
            ConditionParser(condition, /*html_filtering=*/true).Parse();
        const Level level{
            replace_allowed
                ? html_value
                : ConditionParser(condition, /*html_filtering=*/false).Parse(),
            html_value};
        false_depth += !level.value;
        html_false_depth += !level.html_value;
        levels.push_back(level);
      } else if (line.starts_with("!#else")) {
        if (!levels.empty()) {
          Level& level = levels.back();
          level.value ? ++false_depth : --false_depth;
          level.html_value ? ++html_false_depth : --html_false_depth;
          level.value = !level.value;
          level.html_value = !level.html_value;
        }
      } else if (line.starts_with("!#endif")) {
        if (!levels.empty()) {
          false_depth -= !levels.back().value;
          html_false_depth -= !levels.back().html_value;
          levels.pop_back();
        }
      }
      return true;
    }
    if (!line.empty() &&
        (false_depth == 0 ||
         (html_false_depth == 0 && IsHtmlFilterLine(line)))) {
      visit(line);
    }
    return true;
  });
}

}  // namespace starry::content_filter
