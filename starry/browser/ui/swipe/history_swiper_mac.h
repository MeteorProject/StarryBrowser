// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SWIPE_HISTORY_SWIPER_MAC_H_
#define STARRY_BROWSER_UI_SWIPE_HISTORY_SWIPER_MAC_H_

#import "chrome/browser/renderer_host/chrome_render_widget_host_view_mac_history_swiper.h"

namespace content {
class RenderWidgetHost;
}

// 认出触控板两指左右滑动翻页（macOS）。chrome/browser/renderer_host/
// chrome_render_widget_host_view_mac_delegate.mm 的补丁用它换掉 Chrome 的 HistorySwiper：
// 网页是 Starry 窗口里的标签时，认出翻页以后交给 starry::SwipeNavigation（指示器、上下选
// 翻几页、振动）；不是时（开发者工具之类）滚动事件原样交给 Chrome 的，还是 Chrome 的箭头。
//
// 什么时候开始算翻页同 Chrome：系统设置开着「在页面之间轻扫」，网页没消费这一串滚动的
// 第一下、横向滚到了头、overscroll-behavior 允许。和 Chrome 不同的是：上膛以前上下移动多了
// 还是当成滚动、取消；上了膛以后上下移动用来选翻几页，不再取消。松手前的速度也算进去
// （快速一甩）。妙控鼠标还是系统的 trackSwipeEvent，只是指示器换成 Starry 的。
@interface StarryHistorySwiper : HistorySwiper

- (instancetype)initWithDelegate:(id<HistorySwiperDelegate>)delegate
                renderWidgetHost:(content::RenderWidgetHost*)renderWidgetHost;

@end

#endif  // STARRY_BROWSER_UI_SWIPE_HISTORY_SWIPER_MAC_H_
