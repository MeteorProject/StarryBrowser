// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/domain_set.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "starry/components/content_filter/content_filter_types.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace starry::content_filter {

namespace {

bool IsSimpleHostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '-' || c == '_';
}

bool IsIPAddress(std::string_view host) {
  if (host.empty()) {
    return false;
  }
  if (host.front() == '[') {
    return true;
  }
  return std::all_of(host.begin(), host.end(), [](char c) {
    return (c >= '0' && c <= '9') || c == '.';
  });
}

bool Contains(const std::vector<uint64_t>& sorted, uint64_t value) {
  if (sorted.size() <= 8) {
    return std::find(sorted.begin(), sorted.end(), value) != sorted.end();
  }
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

void InsertSorted(std::vector<uint64_t>& sorted, uint64_t value) {
  auto it = std::lower_bound(sorted.begin(), sorted.end(), value);
  if (it == sorted.end() || *it != value) {
    sorted.insert(it, value);
  }
}

}  // namespace

void HostSuffixes::Compute(std::string_view host) {
  plain.clear();
  entity.clear();
  if (host.empty()) {
    return;
  }
  if (IsIPAddress(host)) {
    plain.push_back(HashHost(host));
    return;
  }
  size_t start = 0;
  while (start < host.size()) {
    plain.push_back(HashHost(host.substr(start)));
    const size_t dot = host.find('.', start);
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  // 「名字.*」：去掉公共后缀（含私有的，比如 github.io）。
  const size_t registry =
      net::registry_controlled_domains::GetCanonicalHostRegistry(
          host, net::registry_controlled_domains::EXCLUDE_UNKNOWN_REGISTRIES,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)
          .value_or(std::string_view())
          .size();
  if (registry == 0 || registry + 1 >= host.size()) {
    return;
  }
  const std::string_view name = host.substr(0, host.size() - registry - 1);
  std::string key;
  start = 0;
  while (start < name.size()) {
    key.assign(name.substr(start));
    key += ".*";
    entity.push_back(HashHost(key));
    const size_t dot = name.find('.', start);
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
}

std::string NormalizeHost(std::string_view input, bool strip_www) {
  std::string_view host = base::TrimWhitespaceASCII(input, base::TRIM_ALL);
  while (!host.empty() && host.back() == '.') {
    host.remove_suffix(1);
  }
  if (host.empty() || host.front() == '.') {
    return std::string();
  }
  std::string result;
  if (std::all_of(host.begin(), host.end(), [](char c) {
        return IsSimpleHostChar(base::ToLowerASCII(c));
      })) {
    result = base::ToLowerASCII(host);
  } else {
    // 非 ASCII（转成 punycode）或者别的字符：交给 GURL 判断。
    GURL url(std::string("http://") + std::string(host) + "/");
    if (!url.is_valid() || !url.has_host()) {
      return std::string();
    }
    result = std::string(url.host());
    if (!std::all_of(result.begin(), result.end(), [](char c) {
          return IsSimpleHostChar(c) || c == '[' || c == ']' || c == ':';
        })) {
      return std::string();
    }
  }
  if (strip_www && result.starts_with("www.") && result.size() > 4) {
    result.erase(0, 4);
  }
  return result;
}

DomainSet::DomainSet() = default;
DomainSet::DomainSet(const DomainSet&) = default;
DomainSet& DomainSet::operator=(const DomainSet&) = default;
DomainSet::DomainSet(DomainSet&&) = default;
DomainSet& DomainSet::operator=(DomainSet&&) = default;
DomainSet::~DomainSet() = default;

bool DomainSet::Parse(std::string_view list, char separator) {
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(separator, start);
    if (end == std::string_view::npos) {
      end = list.size();
    }
    std::string_view entry =
        base::TrimWhitespaceASCII(list.substr(start, end - start),
                                  base::TRIM_ALL);
    start = end + 1;
    if (entry.empty()) {
      continue;
    }
    bool exclude = false;
    if (entry.front() == '~') {
      exclude = true;
      entry.remove_prefix(1);
    }
    if (!AddEntry(entry, exclude)) {
      return false;
    }
  }
  return true;
}

void DomainSet::AddInclude(std::string_view host) {
  AddEntry(host, /*exclude=*/false);
}

void DomainSet::AddExclude(std::string_view host) {
  AddEntry(host, /*exclude=*/true);
}

bool DomainSet::AddEntry(std::string_view entry, bool exclude) {
  if (entry.empty()) {
    return true;
  }
  // 正则写的网站（uBlock Origin 的 `domain=/…/`）不支持。
  if (entry.front() == '/') {
    return false;
  }
  if (entry.starts_with("*.")) {
    entry.remove_prefix(2);
  }
  std::vector<uint64_t>& target = exclude ? excludes_ : includes_;
  if (entry.ends_with(".*")) {
    const std::string name =
        NormalizeHost(entry.substr(0, entry.size() - 2), /*strip_www=*/false);
    if (name.empty()) {
      return false;
    }
    InsertSorted(target, HashHost(name + ".*"));
    has_entities_ = true;
    return true;
  }
  const std::string host = NormalizeHost(entry, /*strip_www=*/false);
  if (host.empty()) {
    return false;
  }
  InsertSorted(target, HashHost(host));
  return true;
}

bool DomainSet::Matches(const HostSuffixes& host) const {
  for (uint64_t suffix : host.plain) {
    if (!excludes_.empty() && Contains(excludes_, suffix)) {
      return false;
    }
    if (!includes_.empty() && Contains(includes_, suffix)) {
      return true;
    }
  }
  if (has_entities_) {
    for (uint64_t suffix : host.entity) {
      if (!excludes_.empty() && Contains(excludes_, suffix)) {
        return false;
      }
      if (!includes_.empty() && Contains(includes_, suffix)) {
        return true;
      }
    }
  }
  return includes_.empty();
}

}  // namespace starry::content_filter
