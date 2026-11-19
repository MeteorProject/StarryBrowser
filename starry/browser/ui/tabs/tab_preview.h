// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TAB_PREVIEW_H_
#define STARRY_BROWSER_UI_TABS_TAB_PREVIEW_H_

#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

namespace base {
class ProcessMetrics;
}

namespace views {
class EventMonitor;
}

namespace starry {

class StarryBrowserView;
class TabPreviewCard;

// 悬停标签时的预览卡片：网页缩略图（选中的、显示着的标签、新标签页和内置页面不显示）、
// 标题、域名，下方为标签所在渲染进程的内存与 CPU 占用。每个窗口一份，铺满窗口且不接
// 鼠标事件（移到卡片上即视为离开标签）。
//
// - 鼠标在标签上停 0.5 秒后出现；卡片开着（或者刚收起不到 0.9 秒）时移到别的标签直接换
//   过去，卡片滑到新的位置、平滑调整高度；首次出现从标签那一侧轻微放大、移入并
//   淡入，缩放的中心是卡片上正对着标签的那一点。
// - 标签间隙里保留卡片，离开标签区域 0.12 秒后收起；按下鼠标、滚动、按键时马上收起，
//   鼠标离开那个标签之前不再为它出现。
// - 开着时每秒读一次内存和 CPU（内存是 Chrome 性能面板的那个数，CPU 是进程这一秒的
//   占用，100% 是一个核），收起就停。
// - 放在侧栏右边、和标签的顶边对齐，不超出窗口。
//
// 设置 › 外观 › 标签页预览可以关掉（关掉时标签的悬停提示是标题和网址），或者不显示占用。
class TabPreview : public views::View {
  METADATA_HEADER(TabPreview, views::View)

 public:
  explicit TabPreview(StarryBrowserView* browser_view);
  TabPreview(const TabPreview&) = delete;
  TabPreview& operator=(const TabPreview&) = delete;
  ~TabPreview() override;

  void SetTheme(const Theme& theme);

  // 标签进出。anchor 是标签在本视图里的位置，edge 是侧栏面板右边缘的
  // x（本视图的坐标）； 顶部标签行的标签没有 edge，卡片放在标签下面。
  void Hover(tabs::TabInterface* tab,
             const gfx::Rect& anchor,
             std::optional<int> edge,
             bool inside);
  // 标签之间的空隙也属于同一段悬停，经过它时保留当前卡片。
  void HoverGap(bool inside);
  // 马上收起。suppress 的标签在鼠标离开它之前不再显示；cooling 时不算「刚收起」（滚动、
  // 点击之后再停到标签上要重新等）。
  void Dismiss(std::optional<tabs::TabHandle> suppress = std::nullopt,
               bool cooling = false);

  // 设置里开着。
  bool Enabled() const;
  bool is_showing() const { return shown_tab_.has_value(); }
  // 调试：正在预览哪个标签、卡片在哪里、读到的占用。
  std::string DebugDescription() const;

 private:
  class EventWatcher;
  friend class EventWatcher;

  void Show(tabs::TabHandle handle);
  void Place(bool moving);
  void ScheduleHide();
  bool IsWarm() const;
  void InstallMonitor();
  void RemoveMonitor();
  void OnWindowEvent();
  void StartSampling();
  void Sample();

  const raw_ptr<StarryBrowserView> browser_view_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<TabPreviewCard> card_ = nullptr;

  // 正在预览的标签。
  std::optional<tabs::TabHandle> shown_tab_;
  // 鼠标在哪个标签上，和那个标签的位置、侧栏的右边缘。
  std::optional<tabs::TabHandle> hovered_;
  bool hovering_gap_ = false;
  gfx::Rect hovered_anchor_;
  std::optional<int> hovered_edge_;
  gfx::Rect shown_anchor_;
  std::optional<int> shown_edge_;
  // 点过（或者滚动、按键）的标签，鼠标离开它之前不再显示。
  std::optional<tabs::TabHandle> suppressed_;
  base::OneShotTimer show_timer_;
  base::OneShotTimer hide_timer_;
  base::TimeTicks hidden_at_;

  std::unique_ptr<EventWatcher> watcher_;
  std::unique_ptr<views::EventMonitor> monitor_;

  base::OneShotTimer sample_timer_;
  std::unique_ptr<base::ProcessMetrics> metrics_;
  base::ProcessId metrics_pid_ = base::kNullProcessId;

  base::WeakPtrFactory<TabPreview> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TAB_PREVIEW_H_
