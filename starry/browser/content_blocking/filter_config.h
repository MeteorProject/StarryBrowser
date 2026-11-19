// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_CONTENT_BLOCKING_FILTER_CONFIG_H_
#define STARRY_BROWSER_CONTENT_BLOCKING_FILTER_CONFIG_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/memory/ref_counted.h"
#include "starry/components/content_filter/filter_engine.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"

class GURL;

namespace starry::content_blocking {

// 拦截现在的样子（引擎、开关、不拦截的网站）的一份快照，建好后不再改，哪个线程都能
// 读：网络请求在代理的线程上看它，元素隐藏在界面线程上看它。设置一变就整个换一份
// （ContentBlockingService 换）。
class FilterConfig : public base::RefCountedThreadSafe<FilterConfig> {
 public:
  FilterConfig(scoped_refptr<const content_filter::FilterEngine> engine,
               bool enabled,
               bool ready,
               bool strip_tracking_parameters,
               const std::vector<std::string>& allowlist);
  FilterConfig(const FilterConfig&) = delete;
  FilterConfig& operator=(const FilterConfig&) = delete;

  const content_filter::FilterEngine& engine() const { return *engine_; }
  const scoped_refptr<const content_filter::FilterEngine>& engine_ref() const {
    return engine_;
  }
  // 拦截开着。
  bool enabled() const { return enabled_; }
  // 引擎建好了。开着、还没建好时（刚启动）代理先等一会儿再放请求。
  bool ready() const { return ready_; }
  // 去掉链接里的跟踪参数（和拦截分开的开关）。
  bool strip_tracking_parameters() const { return strip_tracking_parameters_; }
  // 这个网站（或者它的上级）在「不拦截的网站」里。
  bool IsAllowlisted(std::string_view host) const;
  bool IsAllowlistedUrl(const GURL& url) const;
  // 拦截开着、并且这个网站没有关掉。
  bool ShouldFilter(const GURL& top_level_url) const;

 private:
  friend class base::RefCountedThreadSafe<FilterConfig>;
  ~FilterConfig();

  const scoped_refptr<const content_filter::FilterEngine> engine_;
  const bool enabled_;
  const bool ready_;
  const bool strip_tracking_parameters_;
  absl::flat_hash_set<std::string> allowlist_;
};

// 现在的快照（没设过时是关着的空引擎）。哪个线程都能调用。
scoped_refptr<const FilterConfig> CurrentFilterConfig();
// 只有 ContentBlockingService 调用（界面线程）。
void SetCurrentFilterConfig(scoped_refptr<const FilterConfig> config);

}  // namespace starry::content_blocking

#endif  // STARRY_BROWSER_CONTENT_BLOCKING_FILTER_CONFIG_H_
