// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/js_regex.h"

#include <algorithm>

#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace starry::content_filter {

namespace {

constexpr size_t kNpos = std::string_view::npos;

bool AllHex(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), base::IsHexDigit<char>);
}

// `{n}`、`{n,}`、`{n,m}`：pos 是 `{`，返回 `}` 的位置；不成重复（JS 里是字面的 `{`）时
// 返回 npos。
size_t QuantifierEnd(std::string_view source, size_t pos) {
  size_t i = pos + 1;
  const size_t digits = i;
  while (i < source.size() && base::IsAsciiDigit(source[i])) {
    ++i;
  }
  if (i == digits) {
    return kNpos;
  }
  if (i < source.size() && source[i] == ',') {
    ++i;
    while (i < source.size() && base::IsAsciiDigit(source[i])) {
      ++i;
    }
  }
  return i < source.size() && source[i] == '}' ? i : kNpos;
}

// 字符类的转义（`\d`、`\w`、`\s` 和大写的）。
bool IsClassEscape(char c) {
  return std::string_view("dDwWsS").find(c) != kNpos;
}

}  // namespace

bool TranslateJsRegexToRe2(std::string_view source, std::string* out) {
  bool in_class = false;
  for (size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '\\') {
      if (i + 1 >= source.size()) {
        return false;
      }
      const char d = source[++i];
      if (d == 'u') {
        // \uXXXX、\u{X…}
        if (i + 1 < source.size() && source[i + 1] == '{') {
          const size_t close = source.find('}', i + 2);
          if (close == kNpos) {
            return false;
          }
          *out += "\\x{";
          out->append(source.substr(i + 2, close - i - 2));
          *out += '}';
          i = close;
          continue;
        }
        if (i + 4 >= source.size() || !AllHex(source.substr(i + 1, 4))) {
          return false;
        }
        *out += "\\x{";
        out->append(source.substr(i + 1, 4));
        *out += '}';
        i += 4;
        continue;
      }
      if ((d >= '1' && d <= '9') || d == 'c' || d == 'k') {
        return false;
      }
      if (d == '0') {
        *out += "\\x00";
      } else if (d == 'b' && in_class) {
        *out += "\\x08";
      } else if (base::IsAsciiAlpha(d) &&
                 std::string_view("dDwWsSbBnrtfvx").find(d) == kNpos) {
        // JS 里认不得的转义是字母本身。
        *out += d;
      } else if (base::IsAsciiAlphaNumeric(d) ||
                 std::string_view("\\^$.|?*+()[]{}-").find(d) != kNpos) {
        *out += '\\';
        *out += d;
      } else {
        // 别的标点（`\/`、`\_`、`\:`）就是它本身。
        *out += d;
      }
      continue;
    }
    if (in_class) {
      if (c == ']') {
        in_class = false;
      }
      *out += c;
      continue;
    }
    if (c == '[') {
      if (source.substr(i, 3) == "[^]") {
        *out += "[\\s\\S]";
        i += 2;
        continue;
      }
      in_class = true;
      *out += c;
      continue;
    }
    if (c == '(' && source.substr(i, 2) == "(?") {
      const std::string_view rest = source.substr(i + 2);
      if (rest.starts_with("=") || rest.starts_with("!") ||
          rest.starts_with("<=") || rest.starts_with("<!")) {
        return false;
      }
      if (rest.starts_with("<")) {
        *out += "(?P<";
        i += 2;
        continue;
      }
    }
    *out += c;
  }
  return true;
}

bool TranslateJsRegexToIcu(std::string_view source,
                           bool multiline,
                           std::string* out) {
  bool in_class = false;
  // 字符类里上一项是 `\d` 这种：后面的 `-` 是字面的（JS 不拿它当范围）。
  bool after_class_escape = false;
  for (size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '\\') {
      if (i + 1 >= source.size()) {
        return false;
      }
      const char d = source[++i];
      after_class_escape = false;
      switch (d) {
        case 'u':
          if (i + 1 < source.size() && source[i + 1] == '{') {
            const size_t close = source.find('}', i + 2);
            if (close == kNpos ||
                !AllHex(source.substr(i + 2, close - i - 2))) {
              return false;
            }
            *out += "\\x{";
            out->append(source.substr(i + 2, close - i - 2));
            *out += '}';
            i = close;
          } else if (i + 4 < source.size() && AllHex(source.substr(i + 1, 4))) {
            *out += "\\x{";
            out->append(source.substr(i + 1, 4));
            *out += '}';
            i += 4;
          } else {
            *out += 'u';
          }
          continue;
        case 'x':
          if (i + 2 < source.size() && AllHex(source.substr(i + 1, 2))) {
            *out += "\\x{";
            out->append(source.substr(i + 1, 2));
            *out += '}';
            i += 2;
          } else {
            *out += 'x';
          }
          continue;
        case 'c':
          // \cX：控制字符。
          if (i + 1 >= source.size() || !base::IsAsciiAlpha(source[i + 1])) {
            return false;
          }
          *out += base::StringPrintf("\\x{%x}", source[++i] % 32);
          continue;
        case '0':
          // 后面跟着数字的是旧的八进制写法，不支持。
          if (i + 1 < source.size() && base::IsAsciiDigit(source[i + 1])) {
            return false;
          }
          *out += "\\x{0}";
          continue;
        case 'b':
          *out += in_class ? "\\x{8}" : "\\b";
          continue;
        case 'v':
          // ICU 的 `\v` 是所有竖向的空白。
          *out += "\\x{b}";
          continue;
        case 'k':
          // \k<名字>（ICU 同样的写法）。
          if (in_class) {
            return false;
          }
          *out += "\\k";
          continue;
        default:
          break;
      }
      if (base::IsAsciiDigit(d)) {
        // 反向引用（字符类里是八进制，不支持）。
        if (in_class) {
          return false;
        }
        *out += '\\';
        *out += d;
        continue;
      }
      if (IsClassEscape(d) || std::string_view("Bnrtf").find(d) != kNpos) {
        *out += '\\';
        *out += d;
        after_class_escape = in_class && IsClassEscape(d);
        continue;
      }
      if (base::IsAsciiAlpha(d)) {
        // JS 里认不得的转义是字母本身（ICU 的 `\e`、`\z`、`\p` 这些有别的意思）。
        *out += d;
        continue;
      }
      // 标点：ICU 里转义了就是它本身。
      *out += '\\';
      *out += d;
      continue;
    }
    if (in_class) {
      if (c == ']') {
        in_class = false;
        *out += c;
        continue;
      }
      const bool literal_dash =
          c == '-' && (after_class_escape ||
                       (i + 2 < source.size() && source[i + 1] == '\\' &&
                        IsClassEscape(source[i + 2])));
      if (literal_dash || std::string_view("[&{}$^").find(c) != kNpos) {
        *out += '\\';
      }
      *out += c;
      after_class_escape = false;
      continue;
    }
    switch (c) {
      case '[':
        if (source.substr(i, 3) == "[^]") {
          *out += "[\\s\\S]";
          i += 2;
          continue;
        }
        if (source.substr(i, 2) == "[]") {
          // 什么都不匹配。
          *out += "(?!)";
          i += 1;
          continue;
        }
        in_class = true;
        after_class_escape = false;
        *out += '[';
        if (i + 1 < source.size() && source[i + 1] == '^') {
          *out += '^';
          ++i;
        }
        continue;
      case '{': {
        const size_t end = QuantifierEnd(source, i);
        if (end == kNpos) {
          *out += "\\{";
        } else {
          out->append(source.substr(i, end - i + 1));
          i = end;
        }
        continue;
      }
      case '}':
      case ']':
        *out += '\\';
        *out += c;
        continue;
      case '$':
        if (!multiline) {
          *out += "\\z";
          continue;
        }
        break;
      default:
        break;
    }
    *out += c;
  }
  return !in_class;
}

bool JsRegexHasNamedGroups(std::string_view source) {
  bool in_class = false;
  for (size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '\\') {
      ++i;
    } else if (in_class) {
      in_class = c != ']';
    } else if (c == '[') {
      in_class = true;
    } else if (source.substr(i, 3) == "(?<" && i + 3 < source.size() &&
               source[i + 3] != '=' && source[i + 3] != '!') {
      return true;
    }
  }
  return false;
}

}  // namespace starry::content_filter
