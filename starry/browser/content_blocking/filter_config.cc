// Copyright 2026 The Starry Browser Authors

#include "starry/browser/content_blocking/filter_config.h"

#include <utility>

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "url/gurl.h"

namespace starry::content_blocking {

namespace {

struct Current {
  base::Lock lock;
  scoped_refptr<const FilterConfig> config GUARDED_BY(lock);
};

Current& GetCurrent() {
  static base::NoDestructor<Current> current;
  return *current;
}

}  // namespace

FilterConfig::FilterConfig(
    scoped_refptr<const content_filter::FilterEngine> engine,
    bool enabled,
    bool ready,
    bool strip_tracking_parameters,
    const std::vector<std::string>& allowlist)
    : engine_(engine ? std::move(engine)
                     : scoped_refptr<const content_filter::FilterEngine>(
                           content_filter::FilterEngine::CreateEmpty())),
      enabled_(enabled),
      ready_(ready),
      strip_tracking_parameters_(strip_tracking_parameters),
      allowlist_(allowlist.begin(), allowlist.end()) {}

FilterConfig::~FilterConfig() = default;

bool FilterConfig::IsAllowlisted(std::string_view host) const {
  if (allowlist_.empty() || host.empty()) {
    return false;
  }
  // 网站本身和每一级上级（a.b.example.com → b.example.com → example.com → com）。
  while (!host.empty()) {
    if (allowlist_.contains(host)) {
      return true;
    }
    const size_t dot = host.find('.');
    if (dot == std::string_view::npos) {
      return false;
    }
    host.remove_prefix(dot + 1);
  }
  return false;
}

bool FilterConfig::IsAllowlistedUrl(const GURL& url) const {
  return url.has_host() && IsAllowlisted(url.host());
}

bool FilterConfig::ShouldFilter(const GURL& top_level_url) const {
  return enabled_ && !IsAllowlistedUrl(top_level_url);
}

scoped_refptr<const FilterConfig> CurrentFilterConfig() {
  Current& current = GetCurrent();
  base::AutoLock lock(current.lock);
  if (!current.config) {
    current.config = base::MakeRefCounted<FilterConfig>(
        nullptr, /*enabled=*/false, /*ready=*/true,
        /*strip_tracking_parameters=*/false, std::vector<std::string>());
  }
  return current.config;
}

void SetCurrentFilterConfig(scoped_refptr<const FilterConfig> config) {
  Current& current = GetCurrent();
  base::AutoLock lock(current.lock);
  current.config = std::move(config);
}

}  // namespace starry::content_blocking
