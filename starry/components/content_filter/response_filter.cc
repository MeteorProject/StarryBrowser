// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/response_filter.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/i18n/icu_string_conversions.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "starry/components/content_filter/html_filter.h"
#include "starry/components/content_filter/js_regex.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/common/unicode/utext.h"
#include "third_party/icu/source/i18n/unicode/regex.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"

namespace starry::content_filter {

namespace {

constexpr size_t kNpos = std::string_view::npos;

// ICU 的回溯引擎一条规则一次最多跑这么久（写得不好的正则在大的响应上可能要很久）。
constexpr base::TimeDelta kIcuTimeBudget = base::Milliseconds(100);

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

// ---- `replace=` 的值（uBlock Origin 的 ArglistParser，分隔符 `/`） ----

struct Arg {
  size_t begin = 0;
  size_t end = 0;
  size_t separator_begin = 0;
  size_t separator_end = 0;
  // 里面有转义了的分隔符（取出来以后去掉一层转义）。
  bool escaped = false;
  // 实际找的分隔符：整段用引号括起来时是引号。
  char separator = '/';
};

// text[0, pos) 以奇数个反斜杠结尾（pos 处的字符被转义了）。
bool IsEscaped(std::string_view text, size_t pos) {
  size_t count = 0;
  while (count < pos && text[pos - 1 - count] == '\\') {
    ++count;
  }
  return count % 2 == 1;
}

// 从 begin 开始找没转义的 separator，位置记到 arg->end（找不到时是结尾）。
void FindSeparator(std::string_view text,
                   size_t begin,
                   char separator,
                   Arg* arg) {
  arg->escaped = false;
  arg->separator = separator;
  for (size_t pos = begin;; ++pos) {
    pos = text.find(separator, pos);
    if (pos == kNpos) {
      arg->end = text.size();
      return;
    }
    if (!IsEscaped(text, pos)) {
      arg->end = pos;
      return;
    }
    arg->escaped = true;
  }
}

Arg NextArg(std::string_view text, size_t begin, char separator) {
  Arg arg;
  size_t start = begin;
  while (start < text.size() && IsSpace(text[start])) {
    ++start;
  }
  const char quote = start < text.size() ? text[start] : '\0';
  if (quote == '"' || quote == '\'' || quote == '`') {
    // 整段用引号括起来（后面紧跟着分隔符或者结尾）：引号去掉。
    arg.begin = start + 1;
    FindSeparator(text, arg.begin, quote, &arg);
    if (arg.end != text.size()) {
      arg.separator_begin = arg.separator_end = arg.end + 1;
      while (arg.separator_end < text.size() &&
             IsSpace(text[arg.separator_end])) {
        ++arg.separator_end;
      }
      if (arg.separator_end == text.size()) {
        return arg;
      }
      if (text[arg.separator_end] == separator) {
        ++arg.separator_end;
        return arg;
      }
    }
  }
  arg.begin = start;
  FindSeparator(text, start, separator, &arg);
  arg.separator_begin = arg.separator_end = arg.end;
  if (arg.separator_begin < text.size()) {
    ++arg.separator_end;
  }
  while (arg.end > arg.begin && IsSpace(text[arg.end - 1])) {
    --arg.end;
  }
  return arg;
}

// 去掉 c 前面的一层转义（`\c` → `c`，`\\c` 不动）：uBlock Origin 的 normalizeArg。
std::string Unescape(std::string_view text, char c) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    if (ch == c && IsEscaped(out, out.size())) {
      out.pop_back();
    }
    out += ch;
  }
  return out;
}

std::string TakeArg(std::string_view text, const Arg& arg) {
  std::string value(text.substr(arg.begin, arg.end - arg.begin));
  if (arg.escaped) {
    value = Unescape(value, arg.separator);
  }
  return Unescape(Unescape(value, '$'), ',');
}

// 只有转义了的标点、没有别的正则写法的：直接找字符串。
bool TakeLiteral(std::string_view source, std::string* literal) {
  for (size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '\\') {
      if (i + 1 >= source.size() || base::IsAsciiAlphaNumeric(source[i + 1])) {
        return false;
      }
      *literal += source[++i];
    } else if (std::string_view("^$.*+?()[]{}|").find(c) != kNpos) {
      return false;
    } else {
      *literal += c;
    }
  }
  return !literal->empty();
}

// UTF-8 的一个字有几个字节（空的匹配往后挪一个字，不切开多字节的字）。
size_t Utf8CharLength(std::string_view text, size_t pos) {
  const unsigned char lead = static_cast<unsigned char>(text[pos]);
  const size_t length = lead >= 0xF0   ? 4
                        : lead >= 0xE0 ? 3
                        : lead >= 0xC0 ? 2
                                       : 1;
  return std::min(length, text.size() - pos);
}

UBool U_CALLCONV BeforeDeadline(const void* context, int64_t) {
  return base::TimeTicks::Now() < *static_cast<const base::TimeTicks*>(context);
}

UBool U_CALLCONV BeforeDeadlineSteps(const void* context, int32_t steps) {
  return BeforeDeadline(context, steps);
}

struct UTextCloser {
  void operator()(UText* text) const { utext_close(text); }
};

// 不是页面的响应的编码：BOM、响应头；都没有时是合法的 UTF-8 就是 UTF-8，否则
// windows-1252（每个字节都能解码，原样编回去）。
bool DetectTextCharset(std::string_view body,
                       std::string_view header_charset,
                       std::string* charset) {
  if (body.starts_with("\xEF\xBB\xBF")) {
    *charset = "utf-8";
    return true;
  }
  if (body.starts_with("\xFE\xFF") || body.starts_with("\xFF\xFE")) {
    return false;
  }
  if (!NormalizeCharset(header_charset, charset)) {
    return false;
  }
  if (charset->empty()) {
    *charset = base::IsStringUTF8(body) ? "utf-8" : "windows-1252";
  }
  return true;
}

}  // namespace

struct ReplaceFilter::IcuPattern {
  std::unique_ptr<icu::RegexPattern> pattern;
};

ReplaceFilter::ReplaceFilter() = default;
ReplaceFilter::~ReplaceFilter() = default;

// static
std::unique_ptr<ReplaceFilter> ReplaceFilter::Parse(std::string_view value) {
  if (!value.starts_with('/')) {
    return nullptr;
  }
  const Arg pattern_arg = NextArg(value, 1, '/');
  const std::string pattern = TakeArg(value, pattern_arg);
  if (pattern.empty()) {
    return nullptr;
  }
  const Arg replacement_arg = NextArg(value, pattern_arg.separator_end, '/');
  if (replacement_arg.separator_end == replacement_arg.separator_begin) {
    return nullptr;
  }
  auto filter = base::WrapUnique(new ReplaceFilter());
  filter->source_ = std::string(value);
  filter->replacement_ = TakeArg(value, replacement_arg);

  bool case_insensitive = false;
  bool multiline = false;
  bool dot_all = false;
  std::string seen;
  for (char flag : value.substr(replacement_arg.separator_end)) {
    // y（粘着）、v 不支持；同一个标志写两次是错的（同 JS）。
    if (std::string_view("dgimsu").find(flag) == kNpos ||
        seen.find(flag) != std::string::npos) {
      return nullptr;
    }
    seen += flag;
    filter->global_ |= flag == 'g';
    case_insensitive |= flag == 'i';
    multiline |= flag == 'm';
    dot_all |= flag == 's';
  }

  const std::string& replacement = filter->replacement_;
  for (size_t i = 0; i + 1 < replacement.size(); ++i) {
    if (replacement[i] == '$' &&
        (base::IsAsciiDigit(replacement[i + 1]) || replacement[i + 1] == '<')) {
      filter->needs_groups_ = true;
      break;
    }
  }
  filter->named_groups_ = JsRegexHasNamedGroups(pattern);

  if (!case_insensitive && TakeLiteral(pattern, &filter->literal_)) {
    return filter;
  }
  filter->literal_.clear();

  std::string translated;
  if (TranslateJsRegexToRe2(pattern, &translated)) {
    re2::RE2::Options options;
    options.set_log_errors(false);
    options.set_case_sensitive(!case_insensitive);
    options.set_dot_nl(dot_all);
    options.set_max_mem(8 << 20);
    auto regex = std::make_unique<re2::RE2>(
        (multiline ? "(?m)" : "") + translated, options);
    if (regex->ok()) {
      filter->group_count_ = regex->NumberOfCapturingGroups();
      filter->re2_ = std::move(regex);
      return filter;
    }
  }

  translated.clear();
  if (!TranslateJsRegexToIcu(pattern, multiline, &translated)) {
    return nullptr;
  }
  uint32_t flags = 0;
  if (case_insensitive) {
    flags |= UREGEX_CASE_INSENSITIVE;
  }
  if (multiline) {
    flags |= UREGEX_MULTILINE;
  }
  if (dot_all) {
    flags |= UREGEX_DOTALL;
  }
  UErrorCode status = U_ZERO_ERROR;
  UParseError parse_error;
  std::unique_ptr<icu::RegexPattern> regex(icu::RegexPattern::compile(
      icu::UnicodeString::fromUTF8(translated), flags, parse_error, status));
  if (U_FAILURE(status) || !regex) {
    return nullptr;
  }
  std::unique_ptr<icu::RegexMatcher> matcher(regex->matcher(status));
  if (U_FAILURE(status) || !matcher) {
    return nullptr;
  }
  filter->group_count_ = matcher->groupCount();
  filter->icu_ = std::make_unique<IcuPattern>(std::move(regex));
  return filter;
}

bool ReplaceFilter::Apply(std::string* text) const {
  if (text->empty()) {
    return false;
  }
  if (!literal_.empty()) {
    return ApplyLiteral(text);
  }
  if (re2_) {
    return ApplyRe2(text);
  }
  return ApplyIcu(text);
}

int ReplaceFilter::GroupNumber(std::string_view name) const {
  if (re2_) {
    const std::map<std::string, int>& groups = re2_->NamedCapturingGroups();
    auto it = groups.find(std::string(name));
    return it == groups.end() ? 0 : it->second;
  }
  if (icu_) {
    UErrorCode status = U_ZERO_ERROR;
    const int32_t number = icu_->pattern->groupNumberFromName(
        name.data(), static_cast<int32_t>(name.size()), status);
    return U_SUCCESS(status) ? number : 0;
  }
  return 0;
}

// JS 的 GetSubstitution。
void ReplaceFilter::AppendReplacement(std::string_view text,
                                      const std::vector<Span>& groups,
                                      std::string* out) const {
  const std::string& r = replacement_;
  auto append_group = [&](int index) {
    if (index < static_cast<int>(groups.size()) &&
        groups[index].begin != kNpos) {
      out->append(text.substr(groups[index].begin,
                              groups[index].end - groups[index].begin));
    }
  };
  for (size_t i = 0; i < r.size(); ++i) {
    const char c = r[i];
    if (c != '$' || i + 1 >= r.size()) {
      *out += c;
      continue;
    }
    const char d = r[i + 1];
    if (d == '$') {
      *out += '$';
      ++i;
    } else if (d == '&') {
      append_group(0);
      ++i;
    } else if (d == '`') {
      out->append(text.substr(0, groups[0].begin));
      ++i;
    } else if (d == '\'') {
      out->append(text.substr(groups[0].end));
      ++i;
    } else if (base::IsAsciiDigit(d)) {
      // `$n`、`$nn`：两位数的分组没有时按一位数算；没有这个分组时是字面的。
      size_t digits = 1;
      int index = d - '0';
      if (i + 2 < r.size() && base::IsAsciiDigit(r[i + 2]) &&
          index * 10 + (r[i + 2] - '0') <= group_count_) {
        digits = 2;
        index = index * 10 + (r[i + 2] - '0');
      }
      if (index >= 1 && index <= group_count_) {
        append_group(index);
        i += digits;
      } else {
        *out += '$';
      }
    } else if (d == '<' && named_groups_ && r.find('>', i + 2) != kNpos) {
      const size_t close = r.find('>', i + 2);
      if (const int index =
              GroupNumber(std::string_view(r).substr(i + 2, close - i - 2))) {
        append_group(index);
      }
      i = close;
    } else {
      *out += '$';
    }
  }
}

bool ReplaceFilter::ApplyLiteral(std::string* text) const {
  const std::string_view source(*text);
  size_t pos = source.find(literal_);
  if (pos == kNpos) {
    return false;
  }
  std::string out;
  out.reserve(source.size());
  std::vector<Span> groups(1);
  size_t copied = 0;
  while (pos != kNpos) {
    groups[0] = {pos, pos + literal_.size()};
    out.append(source.substr(copied, pos - copied));
    AppendReplacement(source, groups, &out);
    copied = groups[0].end;
    if (!global_) {
      break;
    }
    pos = source.find(literal_, copied);
  }
  out.append(source.substr(copied));
  *text = std::move(out);
  return true;
}

bool ReplaceFilter::ApplyRe2(std::string* text) const {
  const std::string_view source(*text);
  const int count = needs_groups_ ? 1 + group_count_ : 1;
  std::vector<std::string_view> submatches(count);
  std::vector<Span> groups(count);
  std::string out;
  bool matched = false;
  size_t copied = 0;
  size_t pos = 0;
  while (pos <= source.size() &&
         re2_->Match(source, pos, source.size(), re2::RE2::UNANCHORED,
                     submatches.data(), count)) {
    for (int i = 0; i < count; ++i) {
      const std::string_view group = submatches[i];
      groups[i] = group.data()
                      ? Span{static_cast<size_t>(group.data() - source.data()),
                             static_cast<size_t>(group.data() - source.data()) +
                                 group.size()}
                      : Span{kNpos, kNpos};
    }
    if (!matched) {
      out.reserve(source.size());
      matched = true;
    }
    out.append(source.substr(copied, groups[0].begin - copied));
    AppendReplacement(source, groups, &out);
    copied = groups[0].end;
    if (!global_) {
      break;
    }
    if (groups[0].end > groups[0].begin) {
      pos = groups[0].end;
    } else if (groups[0].end < source.size()) {
      // 空的匹配：往后挪一个字（同 JS 的 lastIndex + 1）。
      pos = groups[0].end + Utf8CharLength(source, groups[0].end);
    } else {
      break;
    }
  }
  if (!matched) {
    return false;
  }
  out.append(source.substr(copied));
  *text = std::move(out);
  return true;
}

bool ReplaceFilter::ApplyIcu(std::string* text) const {
  const std::string_view source(*text);
  UErrorCode status = U_ZERO_ERROR;
  std::unique_ptr<UText, UTextCloser> input(utext_openUTF8(
      nullptr, source.data(), static_cast<int64_t>(source.size()), &status));
  std::unique_ptr<icu::RegexMatcher> matcher(icu_->pattern->matcher(status));
  if (U_FAILURE(status) || !input || !matcher) {
    return false;
  }
  // 位置都是 UTF-8 的字节（UText 的 native index）。
  matcher->reset(input.get());
  const base::TimeTicks deadline = base::TimeTicks::Now() + kIcuTimeBudget;
  matcher->setMatchCallback(&BeforeDeadlineSteps, &deadline, status);
  matcher->setFindProgressCallback(&BeforeDeadline, &deadline, status);
  if (U_FAILURE(status)) {
    return false;
  }
  const int count = needs_groups_ ? 1 + group_count_ : 1;
  std::vector<Span> groups(count);
  std::string out;
  bool matched = false;
  size_t copied = 0;
  while (matcher->find(status) && U_SUCCESS(status)) {
    for (int i = 0; i < count; ++i) {
      const int64_t begin = matcher->start64(i, status);
      const int64_t end = matcher->end64(i, status);
      groups[i] = begin < 0 ? Span{kNpos, kNpos}
                            : Span{static_cast<size_t>(begin),
                                   static_cast<size_t>(end)};
    }
    if (U_FAILURE(status) || groups[0].begin < copied) {
      return false;
    }
    if (!matched) {
      out.reserve(source.size());
      matched = true;
    }
    out.append(source.substr(copied, groups[0].begin - copied));
    AppendReplacement(source, groups, &out);
    copied = groups[0].end;
    if (!global_) {
      break;
    }
  }
  // 超时、回溯的栈不够：这一条不改。
  if (U_FAILURE(status) || !matched) {
    return false;
  }
  out.append(source.substr(copied));
  *text = std::move(out);
  return true;
}

ResponseFilters::ResponseFilters() = default;
ResponseFilters::ResponseFilters(ResponseFilters&&) = default;
ResponseFilters& ResponseFilters::operator=(ResponseFilters&&) = default;
ResponseFilters::~ResponseFilters() = default;

bool IsFilterableResponseMime(std::string_view mime) {
  if (mime.empty()) {
    return true;
  }
  const std::string type = base::ToLowerASCII(mime);
  if (type.starts_with("text/")) {
    return type != "text/event-stream";
  }
  static constexpr std::string_view kTextTypes[] = {
      "application/dash+xml",
      "application/javascript",
      "application/json",
      "application/mpegurl",
      "application/vnd.api+json",
      "application/vnd.apple.mpegurl",
      "application/vnd.apple.mpegurl.audio",
      "application/x-javascript",
      "application/x-mpegurl",
      "application/xhtml+xml",
      "application/xml",
      "audio/mpegurl",
      "audio/x-mpegurl",
  };
  return std::find(std::begin(kTextTypes), std::end(kTextTypes), type) !=
         std::end(kTextTypes);
}

std::optional<std::string> FilterResponseBody(std::string_view body,
                                              std::string_view header_charset,
                                              bool html,
                                              const GURL& url,
                                              const ResponseFilters& filters,
                                              ResponseFilterStats* stats) {
  if (stats) {
    *stats = ResponseFilterStats();
  }
  std::string charset;
  if (body.empty() || filters.empty() ||
      !(html ? DetectHtmlCharset(body, header_charset, &charset)
             : DetectTextCharset(body, header_charset, &charset))) {
    return std::nullopt;
  }
  std::optional<std::string> result;
  if (!filters.replace.empty()) {
    const bool utf8 = charset == "utf-8";
    std::string text;
    bool decoded = true;
    if (utf8) {
      text = std::string(body);
    } else {
      std::u16string utf16;
      decoded = base::CodepageToUTF16(
          body, charset.c_str(), base::OnStringConversionError::FAIL, &utf16);
      text = base::UTF16ToUTF8(utf16);
    }
    size_t replaced = 0;
    for (const ReplaceFilter* filter : filters.replace) {
      if (decoded && filter->Apply(&text)) {
        ++replaced;
      }
    }
    if (replaced && utf8) {
      result = std::move(text);
    } else if (replaced) {
      std::string encoded;
      if (base::UTF16ToCodepage(base::UTF8ToUTF16(text), charset.c_str(),
                                base::OnStringConversionError::FAIL,
                                &encoded)) {
        result = std::move(encoded);
      } else {
        replaced = 0;
      }
    }
    if (stats) {
      stats->replaced = replaced;
    }
  }
  if (html && !filters.html.empty()) {
    size_t removed = 0;
    std::optional<std::string> filtered =
        FilterHtml(result ? std::string_view(*result) : body, charset, url,
                   filters.html, &removed);
    if (filtered) {
      result = std::move(filtered);
      if (stats) {
        stats->removed = removed;
      }
    }
  }
  return result;
}

}  // namespace starry::content_filter
