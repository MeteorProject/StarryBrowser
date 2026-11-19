// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FRAME_STARRY_BROWSER_VIEW_LAYOUT_H_
#define STARRY_BROWSER_UI_FRAME_STARRY_BROWSER_VIEW_LAYOUT_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_impl.h"

namespace starry {

class StarryBrowserView;

// Starry 窗口的排版（上游的排版见 chrome/browser/ui/views/frame/layout/README.md，
// 普通窗口本来用 BrowserViewTabbedLayoutImpl）。
//
//   ┌──────────┬───────────────────────────┐
//   │          │ ╭───────────────────────╮ │
//   │   侧栏   │ │     内容卡片（网页）   │ │
//   │          │ ╰───────────────────────╯ │
//   └──────────┴───────────────────────────┘
//
// - 最下面是铺满窗口的底（WindowBackgroundView）。
// - 网页（MultiContentsView，开发者工具、分屏都在它里面）放在内容卡片的位置，信息栏
//   在卡片上面。
// - Chrome 自己的标签栏、工具栏、书签栏、侧边面板都藏起来。
// - 网页全屏时只剩网页。
// - 窗口边框占的角（macOS 左上的红绿灯，Windows 和 Linux 右上的按钮）从排版参数里
//   拿：红绿灯在侧栏顶栏里，右上的按钮在卡片上方留出的一条里。
// - 小窗口（StarryBrowserView::is_mini_window）：上方 44 高的工具栏，下方一张内容卡片
//   （左右下保留标准内边距），没有侧栏和标签行。
class StarryBrowserViewLayout : public BrowserViewLayoutImpl {
 public:
  StarryBrowserViewLayout(std::unique_ptr<BrowserViewLayoutDelegate> delegate,
                          BrowserViewLayoutViews views,
                          StarryBrowserView* browser_view);
  StarryBrowserViewLayout(const StarryBrowserViewLayout&) = delete;
  StarryBrowserViewLayout& operator=(const StarryBrowserViewLayout&) = delete;
  ~StarryBrowserViewLayout() override;

  // views::LayoutManager:
  gfx::Size GetMinimumSize(const views::View* host) const override;

 protected:
  // BrowserViewLayoutImpl:
  ProposedLayout CalculateProposedLayout(
      const BrowserLayoutParams& params) const override;
  gfx::Rect CalculateTopContainerLayout(ProposedLayout& layout,
                                        BrowserLayoutParams params,
                                        bool needs_exclusion) const override;
  void DoPreLayoutComputations(const BrowserLayoutParams& params) override;
  void DoPostLayoutVisualAdjustments(
      const BrowserLayoutParams& params) override;

 private:
  // 只显示网页（网页全屏）。
  bool IsContentOnly() const;
  ProposedLayout CalculateMiniWindowLayout(
      const BrowserLayoutParams& params) const;
  // 小窗口的内容卡片（这个视图的坐标）。
  gfx::Rect MiniWindowCard(const gfx::Rect& area) const;

  const raw_ptr<StarryBrowserView> browser_view_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FRAME_STARRY_BROWSER_VIEW_LAYOUT_H_
