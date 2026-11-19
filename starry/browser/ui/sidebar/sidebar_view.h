// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SIDEBAR_SIDEBAR_VIEW_H_
#define STARRY_BROWSER_UI_SIDEBAR_SIDEBAR_VIEW_H_

#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/prefs/pref_change_registrar.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/frame/chrome_layout.h"
#include "starry/browser/ui/motion/frame_transition.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/view_tracker.h"

class BrowserWindowInterface;

namespace gfx {
class Canvas;
}

namespace starry {

class AddressPill;
class ArchiveButton;
class ArchivePage;
class OrganizePage;
class ExtensionBar;
class GlassView;
class MediaDock;
class NavigationButtons;
class SidebarPanelFill;
class SidebarResizeHandle;
class SidebarShadow;
class SidebarTabs;
class UtilityButtons;
class WindowBackgroundView;

// 侧栏：顶栏（红绿灯让出的位置、导航、归档）、地址栏、扩展按钮、标签（SidebarTabs：
// 固定标签、新标签页、标签列表）、底部的工具按钮（UtilityButtons）。
//
// 形态由窗口布局（ChromeLayout）决定，在布局菜单中切换并同步到所有窗口，切换时平滑
// 过渡：
// - 固定（默认）：贴着窗口左侧，无独立背景，直接显示窗口底色。
// - 浮动：带内边距和圆角的玻璃面板，底层仅窗口背景，使用静态底图渲染。
// - 自动隐藏：平时收起，内容卡片占满窗口；鼠标移到窗口左边缘时以浮动面板盖在网页上
//   滑出（实时玻璃、更实面板色和阴影），离开 0.15 秒后收回。命令栏、菜单、对话框
//   打开或拖标签、调宽时不收回。macOS 上收起时同步隐藏红绿灯。
// - 顶部标签布局：隐藏侧栏，向左侧滑出并淡出。
//
// 地址栏、扩展按钮可移至顶部（导航按钮在浮动、自动隐藏模式下随地址栏移动），下方元素
// 同步过渡；启用紧凑地址栏时顶栏高度与细栏对齐。
//
// 右边缘可拖动调宽（200…360，靠近默认宽度 248 时吸附，双击恢复默认），持久化到设置。
class SidebarView : public views::View, public views::ViewTargeterDelegate {
  METADATA_HEADER(SidebarView, views::View)

 public:
  // 侧栏在窗口里怎么放（StarryBrowserViewLayout 用）。坐标相对窗口的内容区。
  struct Placement {
    // 侧栏视图的位置（包括浮动面板的内边距、右边缘骑出去的半个拖动区）。
    gfx::Rect bounds;
    bool visible = true;
    // 内容卡片左边从哪里开始（卡片自己的内边距已经算上）。
    int content_left = 0;
    // 自动隐藏时窗口左边缘的感应条；不用时为空。
    gfx::Rect edge_trigger;
    // 浮动面板离窗口顶边多远（工具栏跟着下移，0…8，动画中是中间值）。
    int panel_inset = 0;
  };

  SidebarView(BrowserWindowInterface* browser, MatchedGeometry* matched);
  SidebarView(const SidebarView&) = delete;
  SidebarView& operator=(const SidebarView&) = delete;
  ~SidebarView() override;

  Placement ComputePlacement(const gfx::Rect& area) const;
  // 现在（动画的终点）是不是浮动的样子；自动隐藏也算。
  bool is_floating() const;
  bool is_auto_hide() const;
  bool is_peeking() const { return peeking_; }
  int sidebar_width() const;
  const gfx::Size& window_controls_inset() const {
    return window_controls_inset_;
  }

  void SetTheme(const Theme& theme);
  // 窗口布局变化。animated 时播放布局过渡动画（地址栏、导航按钮飞向工具栏由
  // MatchedGeometry 处理，调用前已完成 Begin）。
  void SetChromeLayout(const ChromeLayout& layout, bool animated);
  // 窗口边框在左上角占的地方（Windows、Linux 上一般没有）：顶栏的内容让开它。
  void SetWindowControlsInset(const gfx::Size& size);
  // 浮动侧栏的玻璃后面的底由它画。
  void SetUnderlaySource(WindowBackgroundView* background);
  // 排版要重新算（宽度、样子在动）：窗口那边调用 InvalidateLayout。
  void SetLayoutChangedCallback(base::RepeatingClosure callback);
  // 窗口里有没有带按钮的扩展变了（工具栏要不要有扩展那一组）。
  void SetExtensionsChangedCallback(base::RepeatingClosure callback);
  // 窗口进出全屏（只显示网页）。
  void SetContentOnly(bool content_only);

  // 自动隐藏时滑出来 / 收回去。
  void Peek();
  void Unpeek();
  // 调试用：滑出来以后不管鼠标在哪都不收回。
  void set_hold_peek_for_testing(bool hold) { hold_peek_for_testing_ = hold; }
  // 调试用：当作鼠标在这里（窗口坐标），不动真的鼠标。
  void set_pointer_for_testing(std::optional<gfx::Point> point) {
    pointer_for_testing_ = point;
  }

  // 按在地址栏里输入的规则打开：网址补全协议，其他的交给默认搜索引擎。
  void CommitAddress(const std::u16string& text);

  // views::View:
  void Layout(PassKey) override;
  void RemovedFromWidget() override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  SidebarTabs* tabs() { return tabs_; }
  // 侧栏翻到的页：标签（平时）、「已归档」（ArchivePage）、「整理标签页」（OrganizePage）。
  // 页盖住常用图块和标签列表的位置，媒体坞、工具按钮还在下面。翻过去时新的一页从右边
  // 推进来、原来的往左让开淡出，回到标签反过来。顶部标签布局下只有标签。
  enum class Page { kTabs, kArchive, kOrganize };
  void ShowPage(Page page);
  Page page() const { return page_; }
  // 翻到归档页；关（open 是 false）只在它开着时回到标签。整理页同。
  void ShowArchive(bool open);
  void ShowOrganize(bool open);
  bool archive_open() const { return page_ == Page::kArchive; }
  ArchiveButton* archive_button() { return archive_button_; }
  bool organize_open() const { return page_ == Page::kOrganize; }
  ArchivePage* archive_page() { return archive_page_; }
  OrganizePage* organize_page() { return organize_page_; }
  UtilityButtons* utility_buttons() { return utility_; }
  ExtensionBar* extension_bar() { return extension_bar_; }
  MediaDock* media_dock() { return media_; }
  // 媒体坞的卡片收着时在 target
  // 里的位置（位置还在让出来时也是最后的位置）：媒体岛飞进
  // 来的终点。侧栏不在屏幕上（自动隐藏收起、顶部标签布局）时是空的。
  gfx::Rect MediaDockCardBounds(const views::View* target) const;
  NavigationButtons* navigation() { return nav_; }
  // 地址栏（加书签的卡片挂在它下面）。
  views::View* address_pill_view();
  // 侧栏面板在本视图里的位置（浮动时比视图小一圈）。
  gfx::Rect panel_rect() const { return PanelRect(); }
  // 顶栏现在多高（动画中是中间值）。
  int header_height() const;

 private:
  // 宽度的设置变了（或者刚建好）。
  void ApplyWidth(bool animated);
  // 布局里侧栏的样子（浮动、自动隐藏、顶部标签）变了。
  void ApplyShape(const ChromeLayout& old_layout, bool animated);
  void UpdateGlassMode();
  void UpdateColors();
  void PaintGlassUnderlay(gfx::Canvas* canvas, const gfx::Rect& bounds);
  void FocusActiveTab();
  void NotifyLayoutChanged();
  views::View* PageView(Page page);
  LayerMotion& PageMotion(Page page);
  // 浮动面板（自动隐藏时）在侧栏视图里的位置。
  gfx::Rect PanelRect() const;
  int HiddenOffset() const;
  // 收起、滑出（以及固定 ↔ 自动隐藏的切换）时侧栏的变换。
  void SlideTo(bool shown, const MotionCurve& curve);
  // 侧栏滑出来时，每 33 毫秒看一次鼠标离开了没有。
  void CheckPointer();
  bool ShouldStayOpen() const;

  const raw_ptr<BrowserWindowInterface> browser_;
  base::WeakPtr<MatchedGeometry> matched_;
  Theme theme_ = Theme::DarkBase();
  gfx::Size window_controls_inset_;
  views::ViewTracker underlay_source_;
  base::RepeatingClosure layout_changed_;
  base::RepeatingClosure extensions_changed_;
  PrefChangeRegistrar prefs_;
  ChromeLayout layout_;
  bool has_layout_ = false;

  bool auto_hide_ = false;
  // 0 固定，1 浮动（自动隐藏也是 1）。
  ValueMotion floating_;
  // 1 侧栏占一列，0 自动隐藏或顶部标签布局（卡片占满）。
  ValueMotion column_;
  ValueMotion width_;
  // 顶栏的高度（紧凑地址栏时跟着细栏）。
  ValueMotion header_height_;
  // 地址栏、扩展按钮那一行在不在侧栏里（1 在，0
  // 移到顶部了）：下面的东西跟着挪。
  ValueMotion address_row_;
  ValueMotion extension_row_;
  // 媒体坞占的位置（1 占满，0 没有）：标签列表跟着让出、合拢。
  ValueMotion media_row_;
  // 正在拖着调宽度：这时设置变了不动画。
  bool resizing_ = false;
  bool peeking_ = false;
  // 收起（自动隐藏、顶部标签）的动画还在跑（视图还要显示）。
  bool sliding_out_ = false;
  base::RepeatingTimer pointer_timer_;
  std::optional<base::TimeTicks> away_since_;
  LayerMotion slide_motion_;
  LayerMotion shadow_motion_;
  bool content_only_ = false;
  bool hold_peek_for_testing_ = false;
  std::optional<gfx::Point> pointer_for_testing_;

  raw_ptr<SidebarShadow> shadow_ = nullptr;
  raw_ptr<GlassView> glass_ = nullptr;
  raw_ptr<SidebarPanelFill> fill_ = nullptr;
  // 侧栏里的所有内容；叠在玻璃上。
  raw_ptr<views::View> content_ = nullptr;
  raw_ptr<NavigationButtons> nav_ = nullptr;
  raw_ptr<ArchiveButton> archive_button_ = nullptr;
  base::CallbackListSubscription archive_visibility_subscription_;
  raw_ptr<AddressPill> address_pill_ = nullptr;
  raw_ptr<ExtensionBar> extension_bar_ = nullptr;
  raw_ptr<SidebarTabs> tabs_ = nullptr;
  raw_ptr<ArchivePage> archive_page_ = nullptr;
  raw_ptr<OrganizePage> organize_page_ = nullptr;
  Page page_ = Page::kTabs;
  LayerMotion tabs_motion_;
  LayerMotion archive_motion_;
  LayerMotion organize_motion_;
  raw_ptr<MediaDock> media_ = nullptr;
  raw_ptr<UtilityButtons> utility_ = nullptr;
  raw_ptr<SidebarResizeHandle> resize_handle_ = nullptr;
  FrameTransition transition_;

  base::WeakPtrFactory<SidebarView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SIDEBAR_SIDEBAR_VIEW_H_
