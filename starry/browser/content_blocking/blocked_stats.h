// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_CONTENT_BLOCKING_BLOCKED_STATS_H_
#define STARRY_BROWSER_CONTENT_BLOCKING_BLOCKED_STATS_H_

#include <array>

class Profile;

namespace content {
class BrowserContext;
}

// 每天拦下了几个广告和跟踪器（新标签页的「本周拦截」）。只记个数，不记网站；无痕窗口
// 里的不记。拦下的时候先记在内存里，几秒写一次设置（prefs::kBlockedStats）。
namespace starry::content_blocking {

void RecordBlocked(content::BrowserContext* context);

struct WeekBlocked {
  int total = 0;
  // 最近 7 天，最后一个是今天。
  std::array<int, 7> days{};
};

WeekBlocked BlockedThisWeek(Profile* profile);

}  // namespace starry::content_blocking

#endif  // STARRY_BROWSER_CONTENT_BLOCKING_BLOCKED_STATS_H_
