// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SWIPE_OVERSCROLL_NAVIGATION_H_
#define STARRY_BROWSER_UI_SWIPE_OVERSCROLL_NAVIGATION_H_

namespace starry {

// 触控板、触摸屏左右滑动翻页在 aura（Windows、Linux）上的那一半：content 的
// OverscrollController 认出翻页、找到手指离开（overscroll_delegate.h，编进
// //content/browser），这里把它交给 SwipeNavigation（同 macOS 的指示器、上下选翻几页）。
// 启动时登记一次。所有平台都登记：macOS 上没人用它，只有调试步骤 overscroll: 走这条路。
void RegisterOverscrollNavigation();

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SWIPE_OVERSCROLL_NAVIGATION_H_
