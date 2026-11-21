// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FRAME_STARRY_BROWSER_VIEW_H_
#define STARRY_BROWSER_UI_FRAME_STARRY_BROWSER_VIEW_H_

#include <array>
#include <memory>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/prefs/pref_change_registrar.h"
#include "starry/browser/ui/command_bar/command_bar_view.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/chrome_layout.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/frame_transition.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"

class ContentsContainerView;

namespace content {
class WebContents;
}

namespace ui {
class MenuModel;
}

namespace starry {

class AgentOverlay;
class ArchiveNotice;
class ArchiveToastHost;
class AssistantIsland;
class BrowserToolbar;
class CompactToolbar;
class DownloadLaunchView;
class DownloadsPanelHost;
class FindBarView;
class FrameMotion;
enum class IconBadge;
class MediaIsland;
class MiniWindowToolbar;
class PopoverHost;
class SidebarView;
class PinnedPeek;
class SplitOverlay;
class SwipeIndicator;
class TabPreview;
class TopTabStrip;
class UtilityButtons;
class WindowBackgroundView;
class WindowControlsView;
class WindowRailView;
enum class WindowsControlsStyle;

namespace tab_tree {
class WindowTree;
}

// Starry 的浏览器窗口：基于 Chrome 的 BrowserView 叠加 Starry 自定义界面。
//
// 继承 BrowserView 而不是自己实现 BrowserWindow，是因为 Chrome 的大量功能直接找
// BrowserView：权限提示、扩展弹窗、开发者工具停靠、画中画、各平台的窗口边框。Chrome
// 自己的标签栏和工具栏仍在视图树中，只是在 StarryBrowserViewLayout 中隐藏，实际展示
// Starry 的侧栏和内容卡片。
//
// 所有窗口均由此类创建，但只有普通窗口启用完整 Starry 界面；小窗口（其他应用打开的
// 链接，mini_window/mini_window.h）为弹出窗口，使用独立工具栏和内容卡片（无侧栏）；
// 其余弹出窗口和应用窗口沿用 Chromium 默认样式。
class StarryBrowserView : public BrowserView {
  METADATA_HEADER(StarryBrowserView, BrowserView)

 public:
  explicit StarryBrowserView(BrowserWindowInterface* browser);
  StarryBrowserView(const StarryBrowserView&) = delete;
  StarryBrowserView& operator=(const StarryBrowserView&) = delete;
  ~StarryBrowserView() override;

  // 这个浏览器的窗口；窗口还没建或者已经关了时为空。
  static StarryBrowserView* From(BrowserWindowInterface* browser);

  // 这个窗口用的是 Starry 的界面（普通窗口，有侧栏那一套）。
  bool has_starry_chrome() const { return sidebar_ != nullptr; }
  // 小窗口：仅包含顶部工具栏和内容卡片。命令栏、查找栏、对话框、菜单、小弹出框和
  // 下载提示条与普通窗口共用。
  bool is_mini_window() const { return mini_toolbar_ != nullptr; }
  MiniWindowToolbar* mini_toolbar() { return mini_toolbar_; }
  // 小窗口的网页正要移到浏览器窗口：关掉它时不再把窗口的层级还回去、也不把焦点交给
  // 别的 App（就是要把浏览器窗口带到前面）。
  void WillMoveToBrowser();
  bool moving_to_browser() const { return moving_to_browser_; }
  WindowBackgroundView* background_view() { return background_view_; }
  WindowControlsView* window_controls() { return window_controls_; }
  WindowRailView* window_rail() { return window_rail_; }
  double rail_presence() const { return rail_presence_.value(); }
  SidebarView* sidebar() { return sidebar_; }
  // 自动隐藏的侧栏收起时窗口左边缘的感应条。
  views::View* sidebar_edge() { return sidebar_edge_; }
  CommandBarView* command_bar() { return command_bar_; }
  MenuHostView* menu_host() { return menu_host_; }
  FindBarView* find_bar_view() { return find_bar_view_; }
  DialogHostView* dialog_host() { return dialog_host_; }
  DownloadsPanelHost* downloads_panel() { return downloads_panel_; }
  DownloadLaunchView* download_launch() { return download_launch_; }
  // 自动整理后在内容卡片左下角显示的提示，可撤销这一批。
  ArchiveNotice* archive_notice() { return archive_notice_.get(); }
  ArchiveToastHost* archive_toast() { return archive_toast_; }
  // 窗口里的小弹出框（加书签、缩放）。
  PopoverHost* popover_host() { return popover_host_; }
  // 分屏的分隔线、把手、描边和拖标签时的落点预览。
  PinnedPeek* pinned_peek() const { return pinned_peek_; }
  SplitOverlay* split_overlay() { return split_overlay_; }
  // 悬停标签时的预览卡片。
  TabPreview* tab_preview() { return tab_preview_; }
  // 顶部标签布局的标签行、内容卡片上方的工具栏、内容卡片顶部的紧凑地址栏（分屏时每格一
  // 条）。
  TopTabStrip* tab_strip() { return tab_strip_; }
  BrowserToolbar* toolbar() { return toolbar_; }
  CompactToolbar* compact_toolbar(size_t index) {
    return compact_toolbars_[index];
  }
  // 窗口现在的布局（动画的终点）。
  const ChromeLayout& chrome_layout() const { return layout_; }
  // 显示着的那一组工具按钮：侧栏底部的，顶部标签布局下工具栏或紧凑地址栏里的。
  UtilityButtons* active_utility_buttons();
  // 显示着的地址栏（加书签的卡片挂在它下面）；侧栏收着时为空。
  views::View* visible_address_view();
  // 「显示的按钮」菜单（工具按钮、右上角的 AI 助手按钮的右键），point
  // 是本视图的坐标。
  void ShowUtilityButtonsMenu(const gfx::Point& point);
  // 紧凑地址栏在内容卡片（每一格）顶上占的高度，没有时是 0。
  int compact_inset() const;
  // 网页那一块（本视图的坐标：内容卡片去掉紧凑地址栏）。查找栏、缩放提示放在它的角上。
  gfx::Rect PageBounds();
  // 右上角组件的可用区域（本视图坐标），给 Windows 窗口按钮留出位置。
  gfx::Rect BoundsBelowWindowControls(gfx::Rect bounds) const;
  // 分屏的格子（上游的 ContentsContainerView）的边距：紧凑地址栏的位置。
  void ApplyPaneInsets(ContentsContainerView* container);
  // 排版用（StarryBrowserViewLayout）：标签行和工具栏现在显示了多少（0…1）
  // 和没有工具栏时内容卡片离上面多远。
  double strip_presence() const { return strip_presence_.value(); }
  double toolbar_presence() const { return toolbar_presence_.value(); }
  double content_top_inset() const { return top_inset_.value(); }
  // 助手停在右边（侧栏的样子）时内容卡片让出来的宽度（动画里在变）。
  double assistant_dock_inset() const { return dock_inset_.value(); }
  // 助手停靠在右侧打开时，内容卡片平滑收窄（0.32 秒）。
  void SetAssistantDocked(bool docked);
  // 排版之后：紧凑地址栏放到每一格的顶上，红绿灯摆到位。
  void UpdateChromeGeometry();
  // 排版之后（小窗口）：卡片的底、红绿灯。card 是这个视图的坐标，全屏时为空。
  void UpdateMiniWindowGeometry(const gfx::Rect& card);
  // 网页全屏（只显示网页）：紧凑地址栏不占位置。
  void SetContentOnly(bool content_only);

  // 设置向导（onboarding/onboarding.h）：隐藏浏览器外框，内容卡片铺满窗口；
  // presence 0…1 在结束时由弹簧从 1 过渡到 0。
  bool in_onboarding() const { return onboarding_active_; }
  double onboarding_presence() const;
  views::View* onboarding_drag_bar() { return onboarding_drag_; }
  void StartOnboarding();
  void FinishOnboarding(content::WebContents* contents);

  // AI 助手的岛（⌘E、侧栏的 ✦）。
  AssistantIsland* assistant_island() { return assistant_island_; }
  // 媒体岛（有媒体在放时内容卡片右下角的一块）。
  MediaIsland* media_island() { return media_island_; }
  // 有视频（包括淡出的最后一帧）时，岛只盖在网页上，低于浏览器的其他界面；
  // 没有视频时恢复到侧栏上方，让胶囊能飞进坞。
  void UpdateMediaIslandStacking(bool has_video);
  // 媒体岛收起后的去处（侧栏底部的坞、标签行的胶囊）在本视图里的位置；不在屏幕上时
  // 是空的。
  gfx::Rect MediaDockBounds();
  // 媒体岛飞到了坞、胶囊：它们淡入接上。
  void RevealMediaDock();
  // 媒体岛要让开的东西（本视图的坐标）：悬浮着开着的助手的岛、查找栏、智能体的提示条和
  // 后台卡片、自动整理的提示条，还有桌面的画中画小窗。
  std::vector<gfx::Rect> MediaObstacles();
  // 上面这些动了、出现了、没了。
  void MediaObstaclesChanged();
  // 岛开了、关了：各处的 ✦ 按钮常亮或恢复。
  void SetAssistantOpen(bool open);
  // 岛关着、助手还在做事（渐变）或者等用户（警示色）：各处的 ✦ 右上角一个小点。
  // badge 是 IconBadge（design/controls.h）。
  void SetAssistantBadge(IconBadge badge);
  // 智能体在网页上做事时的描边和元素高亮。
  AgentOverlay* agent_overlay() { return agent_overlay_; }
  // 触控板返回、前进时网页边上的指示器（swipe/swipe_navigation.h）。
  SwipeIndicator* swipe_indicator() { return swipe_indicator_; }
  // 显示着 contents 的那一格网页（本视图的坐标；分屏时是它那一格，去掉紧凑地址栏）。
  // 不在显示时是 PageBounds()。
  gfx::Rect PageBoundsFor(content::WebContents* contents);
  void ToggleAssistant();
  // 下载按钮：打开、关闭最近下载的弹出框。侧栏自动隐藏着时先滑出来，下载按钮被隐藏时先
  // 显示出来，等它就位再弹出。
  void ToggleDownloadsPanel();
  // 弹出框开着，或者正等着侧栏滑出来再弹出。
  bool IsDownloadsPanelOpenOrPending() const;
  // 在 point（本视图的坐标）打开 model 的菜单（Starry 的样子）；关掉时调用 closed。
  void ShowMenu(ui::MenuModel* model,
                const gfx::Point& point,
                base::OnceClosure closed,
                MenuOptions options = MenuOptions());
  // 显示一个 Starry 的对话框（排队）；不是 Starry 的窗口时丢掉，按没回答算。
  void PresentDialog(DialogSpec spec, DialogAnswerCallback callback);
  // ⌘L（编辑当前网址）、⌘T 和「新标签页」（在新标签打开）。
  void OpenCommandBar(CommandBarView::Mode mode);
  // 按新标签页风格打开新标签页或命令栏（new_tab/new_tab.h）。
  void NewTab();
  void OpenNewTabSearch(content::WebContents* contents,
                        const std::u16string& text,
                        bool drop = false);
  void NewTabSearchMoved(content::WebContents* contents);
  const Theme& theme() const { return theme_; }

  // BrowserView:
  void Show() override;
  void SetFocusToLocationBar(bool is_user_initiated) override;
  // 小窗口是弹出窗口，Chrome 的地址栏不显示：⌘L 照样打开命令栏。
  bool IsLocationBarVisible() const override;
  int NonClientHitTest(const gfx::Point& point) override;
  // 关小窗口（⌘W、红绿灯）：先淡出再关。
  views::CloseRequestResult OnWindowCloseRequested() override;
  void OnThemeChanged() override;
  void AddedToWidget() override;
  std::unique_ptr<FindBar> CreateFindBar() override;
  void ConfirmBrowserCloseWithPendingDownloads(
      int download_count,
      DownloadCloseType dialog_type,
      base::OnceCallback<void(bool)> callback) override;
  // ⌘D 之后：Starry 的书签卡片（Chrome 的书签气泡挂在藏起来的工具栏上）。
  void ShowBookmarkBubble(const GURL& url, bool already_bookmarked) override;

 private:
  class ChromeObserver;
  class MiniWindowObserver;

  // 小窗口的那一套界面（构造函数里）。
  void BuildMiniWindowChrome();
  // 小窗口首次出现：窗口淡入，内容卡片缩放淡入。
  void PresentMiniWindow();
  // 卡片这一帧的样子：网页那一层的缩放、不透明度和窗口底上画的卡片。
  void ApplyMiniCardAppearance();
  // 关闭小窗口前的淡出动画，完成后再真正关闭。
  void DismissMiniWindow();
  void FinishDismissMiniWindow();
  // 两种窗口都要的：Chrome 自己的下载气泡、分屏落点不要；跟着网页变色的设置。
  void ApplyChromeDefaults();
  void UpdateTheme();
  // 把显示着的主题（跟着网页变色时是中间的颜色）交给各部分。
  void ApplyTheme(const Theme& theme);
  void StepTheme();
  // 布局的设置变了（或者刚建好、扩展有没有变了）：各部分过渡到新的布局。
  void ApplyChromeLayout(bool animated);
  // 当前标签、分屏变了：紧凑地址栏属于哪一格、工具按钮哪个常亮。
  void UpdateForActiveTab();
  void UpdateWindowButtons(bool animated);
  void UpdateWindowControlsTheme();
  void UpdateWindowRailTitle();
  bool AssistantButtonShown() const;
  void RelayoutChrome();
  // 换掉 Chrome 的几个动作：⌘,（设置）打开 Starry 的设置页。
  void OverrideActions();
  // 当前标签的缩放变了：Starry 的缩放提示（Chrome 的缩放气泡关掉）。
  void OnZoomChanged(bool can_show_bubble);
  void KeepOnboardingTab();
  void ReplaceWelcomeTab(base::WeakPtr<content::WebContents> contents);
  // 新标签页搜索框在命令栏坐标中的位置；网页已销毁时为空。
  std::optional<gfx::Rect> NewTabSearchSlot(
      base::WeakPtr<content::WebContents> contents);

  Theme theme_ = Theme::DarkBase();
  // 不跟网页时的主题（深浅跟着系统和 Chrome 的设置）。
  Theme base_theme_ = Theme::DarkBase();
  // 跟着网页变色：从 theme_from_ 过渡到 theme_target_。
  Theme theme_from_ = Theme::DarkBase();
  Theme theme_target_ = Theme::DarkBase();
  bool theme_applied_ = false;
  std::unique_ptr<ValueMotion> theme_motion_;
  std::unique_ptr<ChromeObserver> chrome_observer_;
  // 树状标签页：插入、挪动、关闭后修正父子（在标签列表之前观察模型）。
  std::unique_ptr<tab_tree::WindowTree> tab_tree_;
  std::unique_ptr<ArchiveNotice> archive_notice_;
  PrefChangeRegistrar layout_prefs_;
  ChromeLayout layout_;
  WindowsControlsStyle windows_controls_style_;
  bool windows_controls_auto_hide_ = false;
  bool layout_applied_ = false;
  bool assistant_shown_ = false;
  bool assistant_open_ = false;
  bool content_only_ = false;
  bool window_buttons_placed_ = false;
  std::unique_ptr<MatchedGeometry> matched_;
  ValueMotion strip_presence_;
  ValueMotion toolbar_presence_;
  ValueMotion compact_presence_;
  ValueMotion top_inset_;
  ValueMotion rail_presence_;
  ValueMotion dock_inset_;
  raw_ptr<TopTabStrip> tab_strip_ = nullptr;
  raw_ptr<BrowserToolbar> toolbar_ = nullptr;
  std::array<raw_ptr<CompactToolbar>, 3> compact_toolbars_{};
  raw_ptr<WindowBackgroundView> background_view_ = nullptr;
  raw_ptr<WindowRailView> window_rail_ = nullptr;
  raw_ptr<WindowControlsView> window_controls_ = nullptr;
  raw_ptr<SidebarView> sidebar_ = nullptr;
  raw_ptr<views::View> sidebar_edge_ = nullptr;
  raw_ptr<FindBarView> find_bar_view_ = nullptr;
  raw_ptr<CommandBarView> command_bar_ = nullptr;
  raw_ptr<DialogHostView> dialog_host_ = nullptr;
  raw_ptr<DownloadsPanelHost> downloads_panel_ = nullptr;
  raw_ptr<DownloadLaunchView> download_launch_ = nullptr;
  raw_ptr<ArchiveToastHost> archive_toast_ = nullptr;
  raw_ptr<PopoverHost> popover_host_ = nullptr;
  raw_ptr<PinnedPeek> pinned_peek_ = nullptr;
  raw_ptr<SplitOverlay> split_overlay_ = nullptr;
  raw_ptr<TabPreview> tab_preview_ = nullptr;
  raw_ptr<AssistantIsland> assistant_island_ = nullptr;
  raw_ptr<MediaIsland> media_island_ = nullptr;
  raw_ptr<AgentOverlay> agent_overlay_ = nullptr;
  raw_ptr<SwipeIndicator> swipe_indicator_ = nullptr;
  raw_ptr<MiniWindowToolbar> mini_toolbar_ = nullptr;
  std::unique_ptr<MiniWindowObserver> mini_observer_;
  std::unique_ptr<ValueMotion> window_fade_;
  // 内容卡片出现（0→1，弹簧）。
  std::unique_ptr<ValueMotion> card_appear_;
  // 内容卡片（这个视图的坐标，全屏时为空）。
  gfx::Rect mini_card_;
  // 网页那一层（MultiContentsView）的图层是为出现的动画加的，动完拿掉。
  bool card_layer_added_ = false;
  // 关窗口前的淡出：正在淡出、淡完了（这次关窗口不再拦）。
  bool mini_dismissing_ = false;
  bool mini_dismissed_ = false;
  bool moving_to_browser_ = false;
  base::CallbackListSubscription zoom_subscription_;
  base::CallbackListSubscription downloads_panel_subscription_;
  // 等侧栏滑出、按钮出现后再弹出下载列表。
  base::OneShotTimer downloads_panel_timer_;
  raw_ptr<MenuHostView> menu_host_ = nullptr;
  bool onboarding_checked_ = false;
  bool onboarding_active_ = false;
  base::WeakPtr<content::WebContents> onboarding_contents_;
  base::WeakPtr<content::WebContents> docked_contents_;
  std::unique_ptr<FrameMotion> onboarding_motion_;
  raw_ptr<views::View> onboarding_drag_ = nullptr;
  base::WeakPtrFactory<StarryBrowserView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FRAME_STARRY_BROWSER_VIEW_H_
