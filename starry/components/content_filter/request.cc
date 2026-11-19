// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/request.h"

#include <algorithm>

#include "base/strings/string_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace starry::content_filter {

uint16_t MethodFromName(std::string_view name) {
  static constexpr struct {
    std::string_view name;
    uint16_t bit;
  } kMethods[] = {
      {"get", kMethodGet},         {"post", kMethodPost},
      {"head", kMethodHead},       {"put", kMethodPut},
      {"delete", kMethodDelete},   {"patch", kMethodPatch},
      {"options", kMethodOptions}, {"connect", kMethodConnect},
      {"trace", kMethodTrace},
  };
  for (const auto& method : kMethods) {
    if (base::EqualsCaseInsensitiveASCII(name, method.name)) {
      return method.bit;
    }
  }
  return 0;
}

void Tokenize(std::string_view lower, std::vector<uint32_t>* out) {
  out->clear();
  const size_t length = std::min(lower.size(), kMaxTokenizedUrlLength);
  size_t i = 0;
  while (i < length) {
    if (!IsTokenChar(lower[i])) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < length && IsTokenChar(lower[i])) {
      ++i;
    }
    if (i - start >= kMinTokenLength) {
      out->push_back(HashToken(lower.substr(start, i - start)));
    }
  }
  // 同一个分词只查一次。
  std::sort(out->begin(), out->end());
  out->erase(std::unique(out->begin(), out->end()), out->end());
}

Request::Request(const GURL& url, const GURL& source, RequestType type)
    : url_(url), lower_(base::ToLowerASCII(url.spec())), type_(type) {
  const url::Component host = url.parsed_for_possibly_invalid_spec().host;
  if (host.is_valid()) {
    host_begin_ = static_cast<size_t>(host.begin);
    host_end_ = static_cast<size_t>(host.end());
  }
  if (source.is_valid() && source.has_host()) {
    source_host_ = base::ToLowerASCII(source.host());
    third_party_ = !net::registry_controlled_domains::SameDomainOrHost(
        url, source,
        net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
    strict_third_party_ = url.host() != source.host();
  }
}

Request::~Request() = default;

std::string_view Request::host() const {
  return std::string_view(lower_).substr(host_begin_, host_end_ - host_begin_);
}

const std::vector<uint32_t>& Request::tokens() const {
  if (!tokenized_) {
    tokenized_ = true;
    Tokenize(lower_, &tokens_);
  }
  return tokens_;
}

const HostSuffixes& Request::host_suffixes() const {
  if (!host_suffixes_ready_) {
    host_suffixes_ready_ = true;
    host_suffixes_.Compute(host());
  }
  return host_suffixes_;
}

const HostSuffixes& Request::source_suffixes() const {
  if (!source_suffixes_ready_) {
    source_suffixes_ready_ = true;
    source_suffixes_.Compute(source_host_);
  }
  return source_suffixes_;
}

}  // namespace starry::content_filter
