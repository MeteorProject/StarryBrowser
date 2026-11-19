// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/network_filter.h"

#include <utility>

#include "starry/components/content_filter/request.h"
#include "starry/components/content_filter/response_filter.h"
#include "third_party/re2/src/re2/re2.h"

namespace starry::content_filter {

namespace {

constexpr size_t kNoMatch = std::string_view::npos;

// 一段（不含 `*`）从 i 处匹配，返回结束的位置。`^` 匹配一个分隔符，或者网址的结尾
// （这时这一段剩下的也只能是 `^`）。
size_t MatchSegment(std::string_view segment, std::string_view text, size_t i) {
  for (size_t k = 0; k < segment.size(); ++k) {
    const char p = segment[k];
    if (p == '^') {
      if (i == text.size()) {
        for (size_t rest = k + 1; rest < segment.size(); ++rest) {
          if (segment[rest] != '^') {
            return kNoMatch;
          }
        }
        return text.size();
      }
      if (!IsSeparatorChar(text[i])) {
        return kNoMatch;
      }
      ++i;
      continue;
    }
    if (i >= text.size() || text[i] != p) {
      return kNoMatch;
    }
    ++i;
  }
  return i;
}

// 从 from 往后找这一段最靠左的匹配，返回结束的位置。
size_t FindSegment(std::string_view segment,
                   std::string_view text,
                   size_t from) {
  if (segment.find('^') == std::string_view::npos) {
    const size_t found = text.find(segment, from);
    return found == std::string_view::npos ? kNoMatch : found + segment.size();
  }
  // 先找开头那段字面的字符，再核对整段。
  const size_t literal_end = segment.find('^');
  const std::string_view literal = segment.substr(0, literal_end);
  if (literal.empty()) {
    for (size_t i = from; i <= text.size(); ++i) {
      const size_t end = MatchSegment(segment, text, i);
      if (end != kNoMatch) {
        return end;
      }
    }
    return kNoMatch;
  }
  size_t i = from;
  while ((i = text.find(literal, i)) != std::string_view::npos) {
    const size_t end = MatchSegment(segment, text, i);
    if (end != kNoMatch) {
      return end;
    }
    ++i;
  }
  return kNoMatch;
}

// 最后一段要匹配到结尾。
bool MatchLastSegmentAtEnd(std::string_view segment,
                           std::string_view text,
                           size_t from) {
  if (segment.find('^') == std::string_view::npos) {
    return text.size() >= from + segment.size() && text.ends_with(segment);
  }
  for (size_t i = from; i <= text.size(); ++i) {
    if (MatchSegment(segment, text, i) == text.size()) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool MatchPatternAt(std::string_view pattern,
                    std::string_view text,
                    size_t pos,
                    bool right_anchor) {
  size_t star = pattern.find('*');
  const std::string_view first = pattern.substr(0, star);
  size_t i = MatchSegment(first, text, pos);
  if (i == kNoMatch) {
    return false;
  }
  if (star == std::string_view::npos) {
    return !right_anchor || i == text.size();
  }
  std::string_view rest = pattern.substr(star + 1);
  while (true) {
    star = rest.find('*');
    const std::string_view segment = rest.substr(0, star);
    if (star == std::string_view::npos) {
      // 最后一段。
      if (segment.empty()) {
        return true;
      }
      if (right_anchor) {
        return MatchLastSegmentAtEnd(segment, text, i);
      }
      return FindSegment(segment, text, i) != kNoMatch;
    }
    if (!segment.empty()) {
      i = FindSegment(segment, text, i);
      if (i == kNoMatch) {
        return false;
      }
    }
    rest = rest.substr(star + 1);
  }
}

NetworkFilter::NetworkFilter() = default;
NetworkFilter::NetworkFilter(NetworkFilter&&) = default;
NetworkFilter& NetworkFilter::operator=(NetworkFilter&&) = default;
NetworkFilter::~NetworkFilter() = default;

bool NetworkFilter::IsPlainHost() const {
  constexpr uint32_t kAllowed = kHostAnchor | kWildcard;
  return kind == NetworkFilterKind::kBlock && host_length > 0 &&
         pattern.size() == static_cast<size_t>(host_length) + 1 &&
         pattern.back() == '^' && (flags & ~(kAllowed | kException)) == 0 &&
         types == kNetworkTypes && doc_flags == 0 && methods == 0 &&
         !domains && !to_domains;
}

bool NetworkFilter::MatchesOptions(const Request& request) const {
  if (!(types & request.type())) {
    return false;
  }
  if (flags & (kFirstParty | kThirdParty)) {
    const bool third = (flags & kStrictParty) ? request.strict_third_party()
                                              : request.third_party();
    if (third ? !(flags & kThirdParty) : !(flags & kFirstParty)) {
      return false;
    }
  }
  if (methods) {
    const bool listed = (methods & request.method()) != 0;
    if (listed == methods_negated) {
      return false;
    }
  }
  if (domains && !domains->Matches(request.source_suffixes())) {
    return false;
  }
  if (to_domains && !to_domains->Matches(request.host_suffixes())) {
    return false;
  }
  return true;
}

bool NetworkFilter::MatchesPattern(const Request& request) const {
  if (regex) {
    return re2::RE2::PartialMatch(request.spec(), *regex);
  }
  if (pattern.empty()) {
    return true;
  }
  const std::string_view text =
      (flags & kMatchCase) ? std::string_view(request.spec())
                           : std::string_view(request.lower());
  const bool right = flags & kRightAnchor;
  if (flags & kHostAnchor) {
    const std::string_view host = request.host();
    if (host_length > 0) {
      // 按网站找到的候选：网站正好是请求的网站或者它的上级。
      const std::string_view part = host_part();
      if (host.size() < part.size() || !host.ends_with(part) ||
          (host.size() > part.size() &&
           host[host.size() - part.size() - 1] != '.')) {
        return false;
      }
      return MatchPatternAt(pattern, text,
                            request.host_end() - part.size(), right);
    }
    // 从主机名的开头和每个点后面试。
    size_t start = request.host_begin();
    const size_t end = request.host_end();
    while (start < end) {
      if (MatchPatternAt(pattern, text, start, right)) {
        return true;
      }
      const size_t dot = text.find('.', start);
      if (dot == std::string_view::npos || dot >= end) {
        return false;
      }
      start = dot + 1;
    }
    return false;
  }
  if (flags & kLeftAnchor) {
    return MatchPatternAt(pattern, text, 0, right);
  }
  if (!(flags & kWildcard)) {
    return right ? text.ends_with(pattern)
                 : text.find(pattern) != std::string_view::npos;
  }
  // 不锚定的：第一段放在最靠左的位置（留给后面几段的地方最多）。
  const size_t star = pattern.find('*');
  const std::string_view first = std::string_view(pattern).substr(0, star);
  if (first.empty()) {
    return MatchPatternAt(pattern, text, 0, right);
  }
  if (first.find('^') == std::string_view::npos) {
    const size_t found = text.find(first);
    return found != std::string_view::npos &&
           MatchPatternAt(pattern, text, found, right);
  }
  for (size_t i = 0; i <= text.size(); ++i) {
    if (MatchPatternAt(pattern, text, i, right)) {
      return true;
    }
  }
  return false;
}

namespace {

// 正则里能确定一定出现的分词：只认被字面的分隔符（`\/`、`\.`、`-`……）或者开头
// 结尾的锚夹着的一段字母数字。有「或」时放弃（任何一边都可能不出现）。
std::vector<uint32_t> RegexTokenCandidates(std::string_view source) {
  std::vector<uint32_t> result;
  if (source.find('|') != std::string_view::npos) {
    return result;
  }
  std::string run;
  bool left_ok = false;
  auto finish = [&](bool right_ok) {
    if (!run.empty() && left_ok && right_ok && run.size() >= kMinTokenLength) {
      result.push_back(HashToken(run));
    }
    run.clear();
  };
  size_t i = 0;
  // 开头的 ^ 是网址的开头。
  if (!source.empty() && source[0] == '^') {
    left_ok = true;
    i = 1;
  } else {
    left_ok = false;
  }
  while (i < source.size()) {
    const char c = source[i];
    if (c == '\\' && i + 1 < source.size()) {
      const char next = source[i + 1];
      const bool literal_separator =
          next == '/' || next == '.' || next == '-' || next == '_' ||
          next == ':' || next == '?' || next == '=' || next == '&' ||
          next == '#' || next == '+' || next == ',' || next == ';';
      finish(literal_separator);
      left_ok = literal_separator;
      i += 2;
      continue;
    }
    if (c == '[') {
      finish(false);
      left_ok = false;
      const size_t close = source.find(']', i + 1);
      i = close == std::string_view::npos ? source.size() : close + 1;
      continue;
    }
    if (c == '*' || c == '?' || c == '+' || c == '{') {
      // 前一个字符可有可无或者重复：这一段作废。
      run.clear();
      left_ok = false;
      if (c == '{') {
        const size_t close = source.find('}', i + 1);
        i = close == std::string_view::npos ? source.size() : close + 1;
      } else {
        ++i;
      }
      continue;
    }
    if (c == '(' || c == ')' || c == '.') {
      finish(false);
      left_ok = false;
      ++i;
      continue;
    }
    if (c == '$' && i + 1 == source.size()) {
      finish(true);
      ++i;
      continue;
    }
    const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    if (IsTokenChar(lower)) {
      run.push_back(lower);
      ++i;
      continue;
    }
    // 其他字面的分隔符（/、-、= 这些）。
    finish(true);
    left_ok = true;
    ++i;
  }
  // 结尾没有锚：最后一段后面可能还连着字母，不能用。
  run.clear();
  return result;
}

}  // namespace

std::vector<uint32_t> NetworkFilter::TokenCandidates() const {
  if (flags & kRegex) {
    if (!regex) {
      return {};
    }
    return RegexTokenCandidates(regex->pattern());
  }
  std::vector<uint32_t> result;
  const std::string lower = (flags & kMatchCase) ? [this] {
    std::string copy = pattern;
    for (char& c : copy) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c + 32);
      }
    }
    return copy;
  }()
                                                 : pattern;
  const bool left_anchored = flags & (kLeftAnchor | kHostAnchor);
  const bool right_anchored = flags & kRightAnchor;
  size_t i = 0;
  while (i < lower.size()) {
    if (!IsTokenChar(lower[i])) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < lower.size() && IsTokenChar(lower[i])) {
      ++i;
    }
    if (i - start < kMinTokenLength) {
      continue;
    }
    const bool left_ok = start > 0 ? lower[start - 1] != '*' : left_anchored;
    const bool right_ok = i < lower.size() ? lower[i] != '*' : right_anchored;
    if (left_ok && right_ok) {
      result.push_back(HashToken(std::string_view(lower).substr(start, i - start)));
    }
  }
  return result;
}

}  // namespace starry::content_filter
