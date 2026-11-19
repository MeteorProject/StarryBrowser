// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/frame/starry_browser_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/immersive/immersive_mode_controller.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/unload_controller.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_delegate.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/location_bar/zoom_bubble_coordinator.h"
#include "chrome/browser/ui/zoom/browser_window_zoom_observer.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/archive/archive_notice.h"
#include "starry/browser/ui/archive/archive_toast.h"
#include "starry/browser/ui/assistant/agent_overlay.h"
#include "starry/browser/ui/assistant/assistant_island.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/downloads/download_launch.h"
#include "starry/browser/ui/downloads/downloads_panel.h"
#include "starry/browser/ui/find/find_bar_view.h"
#include "starry/browser/ui/frame/starry_browser_view_layout.h"
#include "starry/browser/ui/frame/window_background_view.h"
#include "starry/browser/ui/frame/window_buttons.h"
#include "starry/browser/ui/frame/window_controls.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/media/media_capsule.h"
#include "starry/browser/ui/media/media_dock.h"
#include "starry/browser/ui/media/media_island.h"
#include "starry/browser/ui/media/media_pip.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/mini_window/mini_window.h"
#include "starry/browser/ui/mini_window/mini_window_activation.h"
#include "starry/browser/ui/mini_window/mini_window_toolbar.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/popover/bookmark_editor.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "starry/browser/ui/popover/zoom_indicator.h"
#include "starry/browser/ui/sidebar/extension_bar.h"
#include "starry/browser/ui/sidebar/sidebar_parts.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "starry/browser/ui/sidebar/utility_buttons.h"
#include "starry/browser/ui/split/split_overlay.h"
#include "starry/browser/ui/swipe/swipe_indicator.h"
#include "starry/browser/ui/tabs/pinned_peek.h"
#include "starry/browser/ui/tabs/tab_preview.h"
#include "starry/browser/ui/tabs/tab_tree.h"
#include "starry/browser/ui/tabs/top_tab_strip.h"
#include "starry/browser/ui/toolbar/address_pill.h"
#include "starry/browser/ui/toolbar/browser_toolbar.h"
#include "starry/browser/ui/toolbar/compact_toolbar.h"
#include "starry/browser/ui/toolbar/page_color.h"
#include "starry/browser/ui/toolbar/toolbar_parts.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "ui/actions/actions.h"
#include "ui/base/hit_test.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/views/border.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/client_view.h"
#include "ui/views/window/hit_test_utils.h"

namespace starry {

namespace {

// 有下载在进行时关窗口、退出的确认（同 Chrome 的 DownloadInProgressDialogView）。安全的
// 那个（继续下载）是默认按钮，Esc 也是它；第 0 个按钮是退出（关闭）。
DialogSpec DownloadsCloseDialog(int download_count,
                                DownloadCloseType dialog_type) {
  const bool incognito =
      dialog_type == DownloadCloseType::kLastWindowInIncognitoProfile;
  const bool guest =
      dialog_type == DownloadCloseType::kLastWindowInGuestSession;
  DialogSpec spec;
  spec.icon = &icons::kArrowDownCircle;
  spec.tone = DialogSpec::Tone::kWarning;
  if (incognito) {
    spec.title = L("app.closePrivate.title");
    spec.message =
        L("app.closePrivate.downloadsCanceled", {{"count", download_count}});
  } else if (guest) {
    spec.title = L("app.exitGuest.title");
    spec.message =
        L("app.exitGuest.downloadsCanceled", {{"count", download_count}});
  } else {
    spec.title = L("app.quit.title");
    spec.message =
        L("app.quit.downloadsCanceled", {{"count", download_count}});
  }
  spec.buttons = {
      {incognito || guest ? L("common.close") : L("app.quit.confirm"),
       ButtonVariant::kDanger},
      {L("downloads.resumeDownload"), ButtonVariant::kFilled,
       /*cancel=*/true},
  };
  return spec;
}

}  // namespace

// 当前标签和它的网页：换了标签、分屏变了，网页的
// theme-color、底色变了（窗口跟着网页变 色）。
class StarryBrowserView::ChromeObserver : public TabStripModelObserver,
                                          public content::WebContentsObserver {
 public:
  explicit ChromeObserver(StarryBrowserView* view) : view_(view) {
    view_->browser()->GetTabStripModel()->AddObserver(this);
    Track();
  }
  ChromeObserver(const ChromeObserver&) = delete;
  ChromeObserver& operator=(const ChromeObserver&) = delete;
  ~ChromeObserver() override {
    view_->browser()->GetTabStripModel()->RemoveObserver(this);
  }

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    Track();
    view_->UpdateForActiveTab();
  }
  void OnSplitTabChanged(const SplitTabChange& change) override {
    view_->UpdateForActiveTab();
  }
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override {
    if (tab == view_->browser()->GetActiveTabInterface()) {
      view_->UpdateForActiveTab();
    }
  }

  // content::WebContentsObserver:
  void DidChangeThemeColor() override { view_->UpdateTheme(); }
  void OnBackgroundColorChanged() override { view_->UpdateTheme(); }
  void PrimaryPageChanged(content::Page& page) override {
    view_->UpdateTheme();
    view_->UpdateWindowRailTitle();
  }
  void TitleWasSet(content::NavigationEntry* entry) override {
    view_->UpdateWindowRailTitle();
  }

 private:
  void Track() {
    tabs::TabInterface* tab = view_->browser()->GetActiveTabInterface();
    content::WebContents* contents = tab ? tab->GetContents() : nullptr;
    if (contents != web_contents()) {
      Observe(contents);
    }
  }

  const raw_ptr<StarryBrowserView> view_;
};

// 小窗口观察者：记录用户调整后的窗口尺寸以便下次沿用；关闭时恢复普通窗口层级
// （mini_window_activation.h）。
class StarryBrowserView::MiniWindowObserver : public views::WidgetObserver {
 public:
  MiniWindowObserver(StarryBrowserView* view, views::Widget* widget)
      : view_(view) {
    observation_.Observe(widget);
  }
  MiniWindowObserver(const MiniWindowObserver&) = delete;
  MiniWindowObserver& operator=(const MiniWindowObserver&) = delete;
  ~MiniWindowObserver() override = default;

  // views::WidgetObserver:
  void OnWidgetUserResizeEnded(views::Widget* widget) override {
    const gfx::Size size = widget->GetWindowBoundsInScreen().size();
    PrefService* prefs =
        starry::prefs::PrefsFor(view_->browser()->GetProfile());
    prefs->SetInteger(starry::prefs::kMiniWindowWidth, size.width());
    prefs->SetInteger(starry::prefs::kMiniWindowHeight, size.height());
  }
  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override {
    if (visible) {
      view_->PresentMiniWindow();
      mini_window::WindowShown(widget);
    }
  }
  void OnWidgetClosing(views::Widget* widget) override {
    mini_window::WindowClosing(widget, view_->moving_to_browser());
  }
  void OnWidgetDestroying(views::Widget* widget) override {
    observation_.Reset();
  }

 private:
  const raw_ptr<StarryBrowserView> view_;
  base::ScopedObservation<views::Widget, views::WidgetObserver> observation_{
      this};
};

// 上游的钩子（starry_hooks.h）。

BrowserView* CreateBrowserView(BrowserWindowInterface* browser) {
  return new StarryBrowserView(browser);
}

bool UsesStarryWindowControls(BrowserView* browser_view) {
  auto* starry = views::AsViewClass<StarryBrowserView>(browser_view);
  return starry && WindowsControlsEnabled() &&
         (starry->has_starry_chrome() || starry->is_mini_window());
}

std::unique_ptr<BrowserViewLayout> MaybeCreateBrowserViewLayout(
    std::unique_ptr<BrowserViewLayoutDelegate>& delegate,
    BrowserWindowInterface* browser,
    BrowserViewLayoutViews& views) {
  auto* browser_view =
      views::AsViewClass<StarryBrowserView>(views.browser_view.get());
  if (!browser_view ||
      (!browser_view->has_starry_chrome() && !browser_view->is_mini_window())) {
    return nullptr;
  }
  return std::make_unique<StarryBrowserViewLayout>(
      std::move(delegate), std::move(views), browser_view);
}

bool ConfirmQuitWithDownloads() {
  // 同一次退出不叠两个对话框（连按两次 ⌘Q）。
  static bool asking = false;
  if (asking) {
    return true;
  }
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_manager) {
    return false;
  }
  // 数一数所有用户资料（含无痕）里会被退出打断的下载（同 Chrome 的
  // shouldQuitWithInProgressDownloads）。
  int count = 0;
  for (Profile* profile : profile_manager->GetLoadedProfiles()) {
    std::vector<Profile*> all = profile->GetAllOffTheRecordProfiles();
    all.push_back(profile);
    for (Profile* each : all) {
      DownloadCoreService* service =
          DownloadCoreServiceFactory::GetForBrowserContext(each);
      if (service && service->HasCreatedDownloadManager()) {
        count += each->GetDownloadManager()->BlockingShutdownCount();
      }
      // Starry 自己下载的只算无痕的（普通的下次启动接着下）。
      if (DownloadService* downloads = DownloadService::Get(each)) {
        count += downloads->LostOnShutdownCount();
      }
    }
  }
  if (count == 0) {
    return false;
  }
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  StarryBrowserView* view = browser ? StarryBrowserView::From(browser) : nullptr;
  if (!view || !view->has_starry_chrome()) {
    return false;
  }
  asking = true;
  view->PresentDialog(
      DownloadsCloseDialog(count, DownloadCloseType::kBrowserShutdown),
      base::BindOnce([](std::optional<size_t> button,
                        const std::u16string& input) {
        asking = false;
        if (button != 0u) {
          return;
        }
        // 选了退出：取消下载（不然又会问），再退出一次。
        DownloadCoreService::CancelAllDownloads(
            DownloadCoreService::CancelDownloadsTrigger::kShutdown);
        if (ProfileManager* manager = g_browser_process->profile_manager()) {
          for (Profile* profile : manager->GetLoadedProfiles()) {
            for (Profile* each : profile->GetAllOffTheRecordProfiles()) {
              if (DownloadService* downloads = DownloadService::Get(each)) {
                downloads->CancelLostOnShutdown();
              }
            }
          }
        }
        chrome::AttemptUserExit();
      }));
  return true;
}

bool ShowStarrySettings(Profile* profile, BrowserWindowInterface* browser) {
  if (!profile) {
    return false;
  }
  if (browser && browser->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
    OpenStarrySettings(browser);
    return true;
  }
  // 没有窗口（或者前面是应用、弹出窗口）：在普通窗口里打开，没有就新开一个。
  ShowSingletonTabOverwritingNTP(profile->GetOriginalProfile(),
                                 StarrySettingsUrl(""),
                                 NavigateParams::IGNORE_AND_NAVIGATE);
  return true;
}

bool HandleNewTabCommand(BrowserWindowInterface* browser) {
  StarryBrowserView* view = StarryBrowserView::From(browser);
  if (view && view->is_mini_window()) {
    // 小窗口中的 ⌘T 切回普通浏览器窗口处理（小窗口内仅响应 ⌘L）。
    BrowserWindowInterface* target =
        mini_window::BrowserWindowFor(browser->GetProfile());
    view = target ? StarryBrowserView::From(target) : nullptr;
    if (!view) {
      return true;
    }
    target->GetWindow()->Show();
    target->GetWindow()->Activate();
  }
  if (!view || !view->command_bar()) {
    return false;
  }
  // ⌘T 先打开命令栏，确认后再在新标签打开。
  view->OpenCommandBar(CommandBarView::Mode::kNewTab);
  return true;
}

StarryBrowserView::StarryBrowserView(BrowserWindowInterface* browser)
    : BrowserView(browser),
      windows_controls_style_(WindowsControlsStyle::kGlass),
      strip_presence_(0,
                      base::BindRepeating(&StarryBrowserView::RelayoutChrome,
                                          base::Unretained(this))),
      toolbar_presence_(0,
                        base::BindRepeating(&StarryBrowserView::RelayoutChrome,
                                            base::Unretained(this))),
      compact_presence_(0,
                        base::BindRepeating(&StarryBrowserView::RelayoutChrome,
                                            base::Unretained(this))),
      top_inset_(metrics::kContentInset,
                 base::BindRepeating(&StarryBrowserView::RelayoutChrome,
                                     base::Unretained(this))),
      rail_presence_(0,
                     base::BindRepeating(&StarryBrowserView::RelayoutChrome,
                                         base::Unretained(this))),
      dock_inset_(0,
                  base::BindRepeating(&StarryBrowserView::RelayoutChrome,
                                      base::Unretained(this))) {
  if (browser->GetType() == BrowserWindowInterface::Type::TYPE_POPUP &&
      mini_window::IsCreating()) {
    BuildMiniWindowChrome();
    return;
  }
  if (browser->GetType() != BrowserWindowInterface::Type::TYPE_NORMAL) {
    return;
  }
  // 树要在标签列表（下面的顶部标签行、侧栏）之前观察模型：列表刷新时已经改好。
  tab_tree_ = std::make_unique<tab_tree::WindowTree>(browser);
  // 自动整理的提示：侧栏的「已归档」那一行和左下角的提示条都听它的。
  archive_notice_ = std::make_unique<ArchiveNotice>(browser);
  matched_ = std::make_unique<MatchedGeometry>(
      this, base::BindRepeating(&StarryBrowserView::RelayoutChrome,
                                base::Unretained(this)));
  theme_motion_ = std::make_unique<ValueMotion>(
      1, base::BindRepeating(&StarryBrowserView::StepTheme,
                             base::Unretained(this)));
  // 底在最下面；侧栏在 BrowserView 自己建的视图上面。
  background_view_ =
      AddChildViewAt(std::make_unique<WindowBackgroundView>(), 0);
  // 紧凑地址栏（分屏时每一格一条）、顶部标签行、工具栏。
  for (auto& bar : compact_toolbars_) {
    bar =
        AddChildView(std::make_unique<CompactToolbar>(browser, matched_.get()));
    bar->SetStyleChangedCallback(base::BindRepeating(
        &StarryBrowserView::UpdateWindowControlsTheme, base::Unretained(this)));
    bar->SetVisible(false);
  }
  if (WindowsControlsEnabled()) {
    window_rail_ = AddChildView(std::make_unique<WindowRailView>());
    window_rail_->SetVisible(false);
    window_controls_ = AddChildView(std::make_unique<WindowControlsView>());
    window_controls_->SetActivationChangedCallback(base::BindRepeating(
        [](StarryBrowserView* view, bool active) {
          if (view->window_rail_) {
            view->window_rail_->SetWindowActive(active);
          }
        },
        base::Unretained(this)));
  }
  tab_strip_ = AddChildView(std::make_unique<TopTabStrip>(browser));
  tab_strip_->SetVisible(false);
  toolbar_ =
      AddChildView(std::make_unique<BrowserToolbar>(browser, matched_.get()));
  toolbar_->SetVisible(false);
  sidebar_ =
      AddChildView(std::make_unique<SidebarView>(browser, matched_.get()));
  sidebar_->SetUnderlaySource(background_view_);
  // 侧栏在动（切换样子、调宽度）：每一帧重新排窗口。
  sidebar_->SetLayoutChangedCallback(
      base::BindRepeating([](views::View* view) { view->InvalidateLayout(); },
                          base::Unretained(this)));
  // 有没有带按钮的扩展变了：工具栏里扩展那一组要不要有。
  sidebar_->SetExtensionsChangedCallback(
      base::BindRepeating(&StarryBrowserView::ApplyChromeLayout,
                          base::Unretained(this), /*animated=*/true));
  sidebar_edge_ = AddChildView(std::make_unique<SidebarEdgeTrigger>(
      base::BindRepeating(&SidebarView::Peek, base::Unretained(sidebar_))));
  sidebar_edge_->SetVisible(false);
  pinned_peek_ = AddChildView(std::make_unique<PinnedPeek>(this));
  split_overlay_ = AddChildView(std::make_unique<SplitOverlay>(this));
  agent_overlay_ = AddChildView(std::make_unique<AgentOverlay>(this));
  swipe_indicator_ = AddChildView(std::make_unique<SwipeIndicator>());
  assistant_island_ = AddChildView(std::make_unique<AssistantIsland>(this));
  media_island_ = AddChildView(std::make_unique<MediaIsland>(this));
  tab_preview_ = AddChildView(std::make_unique<TabPreview>(this));
  // 查找栏在网页上面；命令栏、菜单盖在所有东西上面（AddedToWidget 时再挪到最上面一次）。
  find_bar_view_ = AddChildView(std::make_unique<FindBarView>());
  find_bar_view_->SetShownChangedCallback(base::BindRepeating(
      &StarryBrowserView::MediaObstaclesChanged, base::Unretained(this)));
  command_bar_ = AddChildView(std::make_unique<CommandBarView>(browser));
  download_launch_ =
      AddChildView(std::make_unique<DownloadLaunchView>(browser));
  archive_toast_ = AddChildView(std::make_unique<ArchiveToastHost>(browser));
  downloads_panel_ =
      AddChildView(std::make_unique<DownloadsPanelHost>(browser));
  downloads_panel_subscription_ =
      downloads_panel_->AddShowingChangedCallback(base::BindRepeating(
          [](StarryBrowserView* view) {
            const bool open = view->downloads_panel()->is_showing();
            view->sidebar()->utility_buttons()->SetDownloadsPanelOpen(open);
            view->toolbar()->utility_buttons()->SetDownloadsPanelOpen(open);
            for (CompactToolbar* bar : view->compact_toolbars_) {
              bar->utility_buttons()->SetDownloadsPanelOpen(open);
            }
          },
          base::Unretained(this)));
  popover_host_ = AddChildView(std::make_unique<PopoverHost>());
  dialog_host_ = AddChildView(std::make_unique<DialogHostView>());
  menu_host_ = AddChildView(std::make_unique<MenuHostView>());
  ApplyChromeDefaults();

  // 布局设置变更时同步更新所有窗口，并播放布局过渡动画。
  const auto relayout = base::BindRepeating(
      &StarryBrowserView::ApplyChromeLayout, base::Unretained(this),
      /*animated=*/true);
  for (const char* pref :
       {starry::prefs::kSidebarLayout, starry::prefs::kSidebarAutoHide,
        starry::prefs::kAddressBarOnTop, starry::prefs::kExtensionsOnTop,
        starry::prefs::kCompactAddressBar, starry::prefs::kHiddenUtilityButtons,
        starry::prefs::kAssistantEnabled, starry::prefs::kWindowsControlsStyle,
        starry::prefs::kWindowsControlsAutoHide}) {
    layout_prefs_.Add(pref, relayout);
  }
  chrome_observer_ = std::make_unique<ChromeObserver>(this);
  ApplyChromeLayout(/*animated=*/false);
}

void StarryBrowserView::BuildMiniWindowChrome() {
  BrowserWindowInterface* const browser = this->browser();
  theme_motion_ = std::make_unique<ValueMotion>(
      1, base::BindRepeating(&StarryBrowserView::StepTheme,
                             base::Unretained(this)));
  background_view_ =
      AddChildViewAt(std::make_unique<WindowBackgroundView>(), 0);
  mini_toolbar_ = AddChildView(std::make_unique<MiniWindowToolbar>(browser));
  if (WindowsControlsEnabled()) {
    window_controls_ = AddChildView(std::make_unique<WindowControlsView>());
    window_controls_->SetPresentation(
        WindowControlsView::Presentation::kInlineBar, /*animated=*/false);
  }
  swipe_indicator_ = AddChildView(std::make_unique<SwipeIndicator>());
  find_bar_view_ = AddChildView(std::make_unique<FindBarView>());
  command_bar_ = AddChildView(std::make_unique<CommandBarView>(browser));
  command_bar_->SetTopOffset(mini_window::kCommandBarTopOffset);
  // 小窗口没有下载按钮：开始下载时总是左下角的提示条，点它到浏览器窗口看下载列表。
  download_launch_ =
      AddChildView(std::make_unique<DownloadLaunchView>(browser));
  popover_host_ = AddChildView(std::make_unique<PopoverHost>());
  dialog_host_ = AddChildView(std::make_unique<DialogHostView>());
  menu_host_ = AddChildView(std::make_unique<MenuHostView>());
  ApplyChromeDefaults();
  chrome_observer_ = std::make_unique<ChromeObserver>(this);
}

void StarryBrowserView::ApplyChromeDefaults() {
  BrowserWindowInterface* const browser = this->browser();
  if (auto* zoom = BrowserWindowZoomObserver::From(browser)) {
    // 在 BrowserView 的之后登记：Chrome 的缩放气泡先弹出来，这里再关掉它。
    zoom_subscription_ = zoom->RegisterZoomChangedCallback(base::BindRepeating(
        &StarryBrowserView::OnZoomChanged, base::Unretained(this)));
  }

  // 下载由 Starry 的下载按钮和弹出框显示：Chrome 的下载气泡挂在藏起来的工具栏上，
  // 默认不在下载完成时自己弹出来（用户在 Chrome 的设置里改过的照旧）。
  Profile* profile = browser->GetProfile();
  for (Profile* each : {profile, profile->GetOriginalProfile()}) {
    each->GetPrefs()->SetDefaultPrefValue(
        ::prefs::kDownloadBubblePartialViewEnabled, base::Value(false));
    // 拖标签、链接到网页边上分屏是 Starry 自己的落点预览（SplitOverlay），不要 Chrome
    // 的那一条。
    each->GetPrefs()->SetDefaultPrefValue(
        ::prefs::kSplitViewDragAndDropEnabled, base::Value(false));
  }
  OverrideActions();

  layout_prefs_.Init(starry::prefs::PrefsFor(profile));
  layout_prefs_.Add(starry::prefs::kTintFromWebsite,
                    base::BindRepeating(&StarryBrowserView::UpdateTheme,
                                        base::Unretained(this)));
}

void StarryBrowserView::WillMoveToBrowser() {
  moving_to_browser_ = true;
}

void StarryBrowserView::PresentMiniWindow() {
  if (!window_fade_ || mini_dismissing_ || window_fade_->target() == 1) {
    return;
  }
  // 网页先按没有缩放的大小排好再缩放（见 ApplyMiniCardAppearance）。
  GetWidget()->LayoutRootViewIfNecessary();
  window_fade_->AnimateTo(1, MotionCurve::EaseOut(motion::kMiniWindowFadeIn));
  card_appear_->AnimateTo(1, MotionCurve::Of(motion::kMiniWindowIn));
}

void StarryBrowserView::ApplyMiniCardAppearance() {
  // 以卡片中心缩放并淡入，弹簧略微过冲时不透明度钳制在 1；减弱动态效果时不缩放。
  //
  // 图层的变换就是视图的变换：macOS 上 NativeViewHost 排版时按缩放后的大小设网页的大小
  // （网页会变小、再被图层缩一次）。所以显示出来之前只设不透明度，显示时先按原来的大小
  // 排好（PresentMiniWindow）再缩放，动完再让网页重新排一次，万一中间排过也能恢复。
  const double progress = card_appear_ ? card_appear_->value() : 1;
  const bool presented = !card_appear_ || card_appear_->target() == 1;
  const bool settled =
      !card_appear_ || (!card_appear_->is_animating() && progress == 1);
  const float opacity = static_cast<float>(std::clamp(progress, 0.0, 1.0));
  const float scale =
      motion::ReduceMotion() || !presented
          ? 1.f
          : static_cast<float>(motion::kMiniWindowCardScale +
                               (1 - motion::kMiniWindowCardScale) * progress);
  MultiContentsView* const contents = multi_contents_view();
  if (settled) {
    if (card_layer_added_) {
      card_layer_added_ = false;
      contents->DestroyLayer();
      for (ContentsWebView* web : GetAllVisibleContentsWebViews()) {
        web->holder()->InvalidateLayout();
      }
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(
                         [](base::WeakPtr<StarryBrowserView> view) {
                           if (view && view->GetWidget()) {
                             view->GetWidget()->LayoutRootViewIfNecessary();
                           }
                         },
                         weak_factory_.GetWeakPtr()));
    }
  } else {
    if (!contents->layer()) {
      contents->SetPaintToLayer();
      contents->layer()->SetFillsBoundsOpaquely(false);
      card_layer_added_ = true;
    }
    const gfx::PointF center(contents->width() / 2.f, contents->height() / 2.f);
    gfx::Transform transform;
    transform.Translate(center.x(), center.y());
    transform.Scale(scale, scale);
    transform.Translate(-center.x(), -center.y());
    contents->layer()->SetTransform(transform);
    contents->layer()->SetOpacity(opacity);
  }
  // 窗口底上画的卡片（底色和阴影）跟着一起变。
  std::vector<gfx::Rect> cards;
  if (!mini_card_.IsEmpty()) {
    const gfx::RectF card(
        views::View::ConvertRectToTarget(this, background_view_, mini_card_));
    const gfx::PointF center = card.CenterPoint();
    cards.push_back(gfx::ToRoundedRect(
        gfx::RectF(center.x() - card.width() * scale / 2,
                   center.y() - card.height() * scale / 2, card.width() * scale,
                   card.height() * scale)));
  }
  background_view_->SetCards(
      std::move(cards),
      static_cast<int>(std::lround(radius::kContent * scale)));
  background_view_->SetCardOpacity(opacity);
}

void StarryBrowserView::DismissMiniWindow() {
  mini_dismissing_ = true;
  window_fade_->AnimateTo(0, MotionCurve::EaseIn(motion::kMiniWindowFadeOut));
}

void StarryBrowserView::FinishDismissMiniWindow() {
  views::Widget* const widget = GetWidget();
  if (!mini_dismissing_ || !widget) {
    return;
  }
  mini_dismissing_ = false;
  mini_dismissed_ = true;
  widget->Close();
  // 没关成（淡出的时候网页加了 beforeunload，要问用户）：窗口还在，不能一直透明着。
  if (!widget->IsClosed() && widget->IsVisible()) {
    mini_dismissed_ = false;
    window_fade_->SetValue(1);
    widget->SetOpacity(1);
  }
}

views::CloseRequestResult StarryBrowserView::OnWindowCloseRequested() {
  if (mini_toolbar_ && !mini_dismissed_) {
    if (mini_dismissing_) {
      return views::CloseRequestResult::kCannotClose;
    }
    // 只在用户关窗口时淡出：网页自己 window.close()、移到浏览器（都是标签先没了）、退出
    // 时直接关；网页有 beforeunload 要先问的也照 Chrome 的来。
    views::Widget* const widget = GetWidget();
    if (window_fade_ && widget && widget->IsVisible() &&
        !widget->IsMinimized() && !moving_to_browser_ &&
        !browser_shutdown::IsTryingToQuit() &&
        !browser()->GetTabStripModel()->empty() &&
        !UnloadController::From(browser())->TabsNeedBeforeUnloadFired()) {
      DismissMiniWindow();
      return views::CloseRequestResult::kCannotClose;
    }
  }
  return BrowserView::OnWindowCloseRequested();
}

void StarryBrowserView::OverrideActions() {
  BrowserActions* actions = BrowserActions::From(browser());
  if (!actions || !actions->root_action_item()) {
    return;
  }
  if (actions::ActionItem* settings = actions::ActionManager::Get().FindAction(
          kActionOptions, actions->root_action_item())) {
    settings->SetInvokeActionCallback(base::BindRepeating(
        [](BrowserWindowInterface* browser, actions::ActionItem* item,
           actions::ActionInvocationContext context) {
          // 小窗口里的 ⌘, 在浏览器窗口里打开设置。
          if (mini_window::IsMiniWindow(browser)) {
            browser = mini_window::BrowserWindowFor(browser->GetProfile());
            if (!browser) {
              return;
            }
            browser->GetWindow()->Show();
            browser->GetWindow()->Activate();
          }
          OpenStarrySettings(browser);
        },
        browser()));
  }
}

StarryBrowserView::~StarryBrowserView() {
  chrome_observer_.reset();
}

void StarryBrowserView::RelayoutChrome() {
  InvalidateLayout();
  if (!sidebar_) {
    return;
  }
  sidebar_->InvalidateLayout();
  toolbar_->InvalidateLayout();
  tab_strip_->InvalidateLayout();
  for (CompactToolbar* bar : compact_toolbars_) {
    bar->InvalidateLayout();
  }
}

bool StarryBrowserView::AssistantButtonShown() const {
  PrefService* prefs = starry::prefs::PrefsFor(
      const_cast<StarryBrowserView*>(this)->browser()->GetProfile());
  // 设置里关掉了助手：哪里都没有 ✦。
  if (!prefs->GetBoolean(starry::prefs::kAssistantEnabled)) {
    return false;
  }
  const base::ListValue& hidden =
      prefs->GetList(starry::prefs::kHiddenUtilityButtons);
  return std::ranges::find(
             hidden,
             base::Value(UtilityButtonKey(UtilityButton::kAssistant))) ==
         hidden.end();
}

void StarryBrowserView::ApplyChromeLayout(bool animated) {
  if (!sidebar_) {
    return;
  }
  PrefService* prefs = starry::prefs::PrefsFor(browser()->GetProfile());
  const ChromeLayout layout =
      ChromeLayout::FromPrefs(prefs, sidebar_->extension_bar()->HasContent());
  const bool assistant = AssistantButtonShown();
  const WindowsControlsStyle controls_style =
      WindowsControlsStyleFromPrefs(prefs);
  const bool controls_auto_hide = WindowsControlsAutoHideFromPrefs(prefs);
  if (layout_applied_ && layout == layout_ && assistant == assistant_shown_ &&
      controls_style == windows_controls_style_ &&
      controls_auto_hide == windows_controls_auto_hide_) {
    return;
  }
  animated = animated && layout_applied_ && GetWidget();
  if (animated) {
    // 地址栏、导航按钮、AI 助手的按钮从一边飞到另一边：先记下它们现在在哪。
    matched_->Begin(motion::ChromeLayout());
  }
  layout_ = layout;
  assistant_shown_ = assistant;
  windows_controls_style_ = controls_style;
  windows_controls_auto_hide_ = controls_auto_hide;
  const MotionCurve curve = motion::ChromeLayout();
  const auto set = [&](ValueMotion& motion, double value) {
    if (animated) {
      motion.AnimateTo(value, curve);
    } else {
      motion.SetValue(value);
    }
  };
  const bool sidebar_only =
      !layout.tab_strip() && !layout.toolbar() && !layout.compact;
  const bool wants_rail = window_rail_ && sidebar_only &&
                          controls_style == WindowsControlsStyle::kRail;
  set(strip_presence_, layout.tab_strip() ? 1 : 0);
  set(toolbar_presence_, layout.toolbar() ? 1 : 0);
  set(compact_presence_, layout.compact ? 1 : 0);
  set(top_inset_, layout.content_top_inset());
  set(rail_presence_, wants_rail ? 1 : 0);
  if (window_controls_) {
    WindowControlsView::Presentation presentation =
        WindowControlsView::Presentation::kInlineBar;
    if (layout.tab_strip() || layout.toolbar()) {
      presentation = WindowControlsView::Presentation::kInlineBar;
    } else if (layout.compact) {
      presentation = WindowControlsView::Presentation::kCompactBar;
    } else if (controls_style == WindowsControlsStyle::kRail) {
      presentation = WindowControlsView::Presentation::kSlimRail;
    } else {
      presentation = WindowControlsView::Presentation::kFloatingGlass;
    }
    window_controls_->SetPresentation(presentation, animated);
    window_controls_->SetAutoHide(controls_auto_hide);
  }
  sidebar_->SetChromeLayout(layout, animated);
  toolbar_->SetChromeLayout(layout, assistant, animated);
  tab_strip_->assistant_button()->SetVisible(assistant);
  tab_strip_->media_capsule()->SetStripActive(layout.tab_strip());
  tab_strip_->InvalidateLayout();
  for (CompactToolbar* bar : compact_toolbars_) {
    bar->SetChromeLayout(layout, assistant, animated);
  }
  layout_applied_ = true;
  // 紧凑地址栏的位置：网页直接下移，细栏从顶部滑入。
  if (MultiContentsView* contents = multi_contents_view()) {
    for (ContentsContainerView* container :
         contents->contents_container_views()) {
      ApplyPaneInsets(container);
    }
  }
  UpdateForActiveTab();
  RelayoutChrome();
}

int StarryBrowserView::compact_inset() const {
  // 网页全屏时只有网页。
  if (!layout_.compact || !sidebar_ || content_only_) {
    return 0;
  }
  return metrics::kCompactToolbarHeight;
}

void StarryBrowserView::SetContentOnly(bool content_only) {
  if (content_only_ == content_only) {
    return;
  }
  content_only_ = content_only;
  if (MultiContentsView* contents = multi_contents_view()) {
    for (ContentsContainerView* container :
         contents->contents_container_views()) {
      ApplyPaneInsets(container);
    }
  }
}

void StarryBrowserView::ApplyPaneInsets(ContentsContainerView* container) {
  const int top = compact_inset();
  if (container->GetInsets().top() == top) {
    return;
  }
  container->SetBorder(
      top > 0 ? views::CreateEmptyBorder(gfx::Insets::TLBR(top, 0, 0, 0))
              : nullptr);
}

gfx::Rect StarryBrowserView::PageBounds() {
  gfx::Rect bounds = multi_contents_view()->bounds();
  bounds.Inset(gfx::Insets::TLBR(compact_inset(), 0, 0, 0));
  return bounds;
}

gfx::Rect StarryBrowserView::BoundsBelowWindowControls(gfx::Rect bounds) const {
  if (window_controls_ && window_controls_->GetVisible() &&
      bounds.Intersects(window_controls_->bounds())) {
    const int inset = window_controls_->bounds().bottom() - bounds.y();
    bounds.Inset(gfx::Insets::TLBR(inset, 0, 0, 0));
  }
  return bounds;
}

gfx::Rect StarryBrowserView::PageBoundsFor(content::WebContents* contents) {
  if (MultiContentsView* view = multi_contents_view(); view && contents) {
    for (ContentsContainerView* container : view->contents_container_views()) {
      ContentsWebView* web_view = container->contents_view();
      if (container->GetVisible() && web_view &&
          web_view->web_contents() == contents) {
        return views::View::ConvertRectToTarget(web_view, this,
                                                web_view->GetLocalBounds());
      }
    }
  }
  return PageBounds();
}

UtilityButtons* StarryBrowserView::active_utility_buttons() {
  if (!sidebar_) {
    return nullptr;
  }
  if (layout_.toolbar_utilities()) {
    if (layout_.compact) {
      for (CompactToolbar* bar : compact_toolbars_) {
        if (bar->GetVisible() && bar->focused()) {
          return bar->utility_buttons();
        }
      }
    }
    return toolbar_->utility_buttons();
  }
  return sidebar_->utility_buttons();
}

views::View* StarryBrowserView::visible_address_view() {
  if (mini_toolbar_) {
    return mini_toolbar_->GetVisible()
               ? static_cast<views::View*>(mini_toolbar_->address())
               : nullptr;
  }
  if (!sidebar_) {
    return nullptr;
  }
  if (layout_.address_in_toolbar) {
    if (layout_.compact) {
      for (CompactToolbar* bar : compact_toolbars_) {
        if (bar->GetVisible() && bar->focused()) {
          return bar->address_pill();
        }
      }
      return nullptr;
    }
    return toolbar_->address_pill();
  }
  if (sidebar_->GetVisible() &&
      (!sidebar_->is_auto_hide() || sidebar_->is_peeking())) {
    return sidebar_->address_pill_view();
  }
  return nullptr;
}

void StarryBrowserView::ShowUtilityButtonsMenu(const gfx::Point& point) {
  if (sidebar_) {
    sidebar_->utility_buttons()->ShowButtonsMenuAt(point);
  }
}

void StarryBrowserView::SetAssistantDocked(bool docked) {
  // 停靠的助手宽 392，和卡片之间留 4（contentInset / 2）。
  const double target =
      docked ? metrics::kAssistantDockWidth + metrics::kContentInset / 2.0 : 0;
  if (dock_inset_.target() == target && !dock_inset_.is_animating() &&
      dock_inset_.value() == target) {
    return;
  }
  if (motion::ReduceMotion() || !GetWidget()) {
    dock_inset_.SetValue(target);
    RelayoutChrome();
  } else {
    dock_inset_.AnimateTo(target, motion::Sidebar());
  }
}

void StarryBrowserView::SetAssistantOpen(bool open) {
  if (!sidebar_) {
    return;
  }
  assistant_open_ = open;
  sidebar_->utility_buttons()->SetAssistantOpen(open);
  toolbar_->utility_buttons()->SetAssistantOpen(open);
  toolbar_->assistant_button()->SetActive(open);
  tab_strip_->assistant_button()->SetActive(open);
  for (CompactToolbar* bar : compact_toolbars_) {
    bar->utility_buttons()->SetAssistantOpen(open);
    bar->assistant_button()->SetActive(open);
  }
  // 岛盖着网页底边时：内置助手的提示条藏起来，外部智能体的挪到岛上面。
  agent_overlay_->Update();
  MediaObstaclesChanged();
}

void StarryBrowserView::SetAssistantBadge(IconBadge badge) {
  if (!sidebar_) {
    return;
  }
  sidebar_->utility_buttons()->SetAssistantBadge(badge);
  toolbar_->utility_buttons()->SetAssistantBadge(badge);
  toolbar_->assistant_button()->SetBadge(badge);
  tab_strip_->assistant_button()->SetBadge(badge);
  for (CompactToolbar* bar : compact_toolbars_) {
    bar->utility_buttons()->SetAssistantBadge(badge);
    bar->assistant_button()->SetBadge(badge);
  }
}

void StarryBrowserView::UpdateForActiveTab() {
  if (mini_toolbar_) {
    UpdateTheme();
    InvalidateLayout();
    return;
  }
  if (!sidebar_) {
    return;
  }
  tabs::TabInterface* tab = browser()->GetActiveTabInterface();
  const InternalPage page =
      tab ? InternalPageForUrl(tab->GetContents()->GetVisibleURL())
          : InternalPage::kNone;
  sidebar_->utility_buttons()->SetActivePage(page);
  toolbar_->utility_buttons()->SetActivePage(page);
  for (CompactToolbar* bar : compact_toolbars_) {
    bar->utility_buttons()->SetActivePage(page);
  }
  UpdateWindowRailTitle();
  UpdateTheme();
  InvalidateLayout();
}

void StarryBrowserView::UpdateWindowRailTitle() {
  if (!window_rail_) {
    return;
  }
  tabs::TabInterface* tab = browser()->GetActiveTabInterface();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  window_rail_->SetTitle(contents ? contents->GetTitle() : std::u16string());
}

void StarryBrowserView::UpdateWindowControlsTheme() {
  if (!window_controls_) {
    return;
  }
  // 紧凑地址栏模式：跟随最右侧紧凑栏的网页取色主题，图标深浅自动反转。
  if (window_controls_->presentation() ==
      WindowControlsView::Presentation::kCompactBar) {
    CompactToolbar* rightmost = nullptr;
    for (CompactToolbar* bar : compact_toolbars_) {
      if (bar && bar->GetVisible() &&
          (!rightmost || bar->bounds().right() > rightmost->bounds().right())) {
        rightmost = bar;
      }
    }
    if (rightmost) {
      window_controls_->SetTheme(rightmost->style().theme);
      return;
    }
  }
  // 悬浮玻璃胶囊模式：感知当前网页顶边亮度自动切亮/暗玻璃，防止亮网页出现黑坨或
  // 暗网页发灰。
  if (window_controls_->presentation() ==
      WindowControlsView::Presentation::kFloatingGlass) {
    tabs::TabInterface* tab = browser()->GetActiveTabInterface();
    content::WebContents* contents = tab ? tab->GetContents() : nullptr;
    if (contents && CanTintFromPage(contents)) {
      std::optional<SkColor> color = PageTopColor::Get(contents);
      if (!color) {
        color = contents->GetThemeColor();
      }
      if (!color) {
        color = contents->GetBackgroundColor();
      }
      if (color) {
        const bool light_page = RelativeLuminance(*color) > 0.5;
        window_controls_->SetGlassDark(!light_page);
        window_controls_->SetTheme(light_page ? Theme::LightBase()
                                              : Theme::DarkBase());
        return;
      }
    }
  }
  window_controls_->SetGlassDark(theme_.is_dark);
  window_controls_->SetTheme(theme_);
}

void StarryBrowserView::UpdateWindowButtons(bool animated) {
  if (!sidebar_ || !GetWidget()) {
    return;
  }
  SetWindowButtonsPlacement(
      GetWidget(),
      WindowsControlsEnabled()
          ? std::nullopt
          : layout_.WindowButtonsOrigin(sidebar_->is_peeking()),
      animated && window_buttons_placed_);
  window_buttons_placed_ = true;
}

void StarryBrowserView::UpdateChromeGeometry() {
  if (!sidebar_) {
    return;
  }
  MultiContentsView* contents = multi_contents_view();
  const double presence = std::clamp(compact_presence_.value(), 0.0, 1.0);
  const bool split = contents && contents->IsInSplitView();
  tabs::TabInterface* active = browser()->GetActiveTabInterface();
  const bool shown = presence > 0.001 && compact_inset() > 0;
  for (size_t i = 0; i < compact_toolbars_.size(); ++i) {
    CompactToolbar* bar = compact_toolbars_[i];
    ContentsContainerView* container = nullptr;
    if (contents && shown) {
      if (split) {
        container = contents->contents_container_views()[i];
      } else if (i == 0) {
        container = contents->GetActiveContentsContainerView();
      }
    }
    if (!container || !container->GetVisible() || !container->parent()) {
      bar->SetVisible(false);
      continue;
    }
    content::WebContents* web_contents =
        container->contents_view()->web_contents();
    tabs::TabInterface* tab =
        web_contents ? tabs::TabInterface::MaybeGetFromContents(web_contents)
                     : nullptr;
    bar->SetPane(tab, !split || tab == active, split);
    // 细栏从卡片顶边移入并淡入，超出顶边的部分裁切隐藏。
    const gfx::Rect pane = views::View::ConvertRectToTarget(
        container->parent(), this, container->bounds());
    const int height = metrics::kCompactToolbarHeight;
    const int offset = static_cast<int>(std::lround((1 - presence) * height));
    bar->SetBounds(pane.x(), pane.y() - offset, pane.width(), height);
    bar->layer()->SetClipRect(
        gfx::Rect(0, offset, pane.width(), height - offset));
    bar->layer()->SetOpacity(static_cast<float>(presence));
    bar->SetVisible(true);
  }
  // Windows 紧凑地址栏：把系统控件并入最右侧那一格的紧凑栏右端。
  if (window_controls_) {
    CompactToolbar* rightmost = nullptr;
    const bool in_compact = window_controls_->presentation() ==
                            WindowControlsView::Presentation::kCompactBar;
    if (in_compact && !content_only_) {
      for (CompactToolbar* bar : compact_toolbars_) {
        if (bar && bar->GetVisible() &&
            (!rightmost ||
             bar->bounds().right() > rightmost->bounds().right())) {
          rightmost = bar;
        }
      }
    }
    const int reserve = in_compact ? window_controls_->ReservedBarWidth() : 0;
    for (CompactToolbar* bar : compact_toolbars_) {
      if (bar) {
        bar->SetTrailingInset(bar == rightmost ? reserve : 0);
      }
    }
    if (in_compact) {
      window_controls_->SetVisible(rightmost != nullptr);
    }
    if (rightmost) {
      const gfx::Size visual = window_controls_->VisualSize();
      const bool maximized = GetWidget() && GetWidget()->IsMaximized();
      const int visual_right = rightmost->bounds().right() - 6;
      const int visual_y =
          rightmost->y() + (rightmost->height() - visual.height()) / 2;
      // 只有紧靠窗口右上角的栏才扩展到屏幕边缘，不能跨过停靠的助手或其他分屏。
      const bool at_corner =
          rightmost->bounds().right() >= width() - metrics::kContentInset &&
          rightmost->y() <= metrics::kContentInset;
      const gfx::Insets pad =
          maximized && at_corner
              ? gfx::Insets::TLBR(std::max(0, visual_y), 0, 0,
                                  std::max(0, width() - visual_right))
              : gfx::Insets();
      window_controls_->SetHitPadding(pad);
      window_controls_->SetBounds(visual_right - visual.width(),
                                  visual_y - pad.top(),
                                  visual.width() + pad.right(),
                                  visual.height() + pad.top());
    }
    UpdateWindowControlsTheme();
  }
  UpdateWindowButtons(/*animated=*/true);
}

void StarryBrowserView::UpdateMiniWindowGeometry(const gfx::Rect& card) {
  if (!mini_toolbar_ || !GetWidget()) {
    return;
  }
  mini_card_ = card;
  ApplyMiniCardAppearance();
  // 红绿灯与工具栏垂直居中对齐，横向间距与顶部标签布局一致。
  SetWindowButtonsPlacement(
      GetWidget(),
      WindowsControlsEnabled()
          ? std::nullopt
          : std::optional<gfx::Point>(gfx::Point(
                metrics::kWindowButtonsLeading,
                (metrics::kToolbarHeight - metrics::kWindowButtonHeight) / 2)),
      /*animated=*/false);
}

// static
StarryBrowserView* StarryBrowserView::From(BrowserWindowInterface* browser) {
  return views::AsViewClass<StarryBrowserView>(
      BrowserView::GetBrowserViewForBrowser(browser));
}

void StarryBrowserView::OpenCommandBar(CommandBarView::Mode mode) {
  if (command_bar_) {
    command_bar_->Open(mode);
  }
}

void StarryBrowserView::SetFocusToLocationBar(bool is_user_initiated) {
  if (!command_bar_) {
    BrowserView::SetFocusToLocationBar(is_user_initiated);
    return;
  }
  // Chrome 给新标签页聚焦地址栏时也会走这里：只有用户要的（⌘L）才打开命令栏。
  if (is_user_initiated) {
    OpenCommandBar(CommandBarView::Mode::kEdit);
  }
}

bool StarryBrowserView::IsLocationBarVisible() const {
  return mini_toolbar_ || BrowserView::IsLocationBarVisible();
}

void StarryBrowserView::AddedToWidget() {
  BrowserView::AddedToWidget();
  if (mini_toolbar_) {
    // 查找栏、提示条在网页上面，命令栏、对话框、菜单盖在所有东西上面。
    ReorderChildView(mini_toolbar_, children().size());
    if (window_controls_) {
      ReorderChildView(window_controls_, children().size());
    }
    ReorderChildView(swipe_indicator_, children().size());
    ReorderChildView(find_bar_view_, children().size());
    ReorderChildView(download_launch_, children().size());
    ReorderChildView(popover_host_, children().size());
    ReorderChildView(command_bar_, children().size());
    ReorderChildView(dialog_host_, children().size());
    ReorderChildView(menu_host_, children().size());
    mini_observer_ = std::make_unique<MiniWindowObserver>(this, GetWidget());
    mini_window::ConfigureWindow(GetWidget());
    // 出现时从透明淡入、卡片放大（PresentMiniWindow），关掉前淡出（DismissMiniWindow）。
    window_fade_ = std::make_unique<ValueMotion>(
        0,
        base::BindRepeating(
            [](StarryBrowserView* view) {
              if (views::Widget* widget = view->GetWidget()) {
                widget->SetOpacity(
                    static_cast<float>(view->window_fade_->value()));
              }
              if (view->mini_dismissing_ &&
                  !view->window_fade_->is_animating()) {
                base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                    FROM_HERE,
                    base::BindOnce(&StarryBrowserView::FinishDismissMiniWindow,
                                   view->weak_factory_.GetWeakPtr()));
              }
            },
            base::Unretained(this)));
    card_appear_ = std::make_unique<ValueMotion>(
        0, base::BindRepeating(&StarryBrowserView::ApplyMiniCardAppearance,
                               base::Unretained(this)));
    GetWidget()->SetOpacity(0);
    ApplyMiniCardAppearance();
    // 万一没收到显示的通知，也不能一直透明着。
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&StarryBrowserView::PresentMiniWindow,
                       weak_factory_.GetWeakPtr()),
        base::Seconds(1));
    return;
  }
  if (command_bar_) {
    // 紧凑地址栏在内容卡片顶上；分屏的把手、描边盖在网页上；工具栏、标签行（拖标签时的
    // 小条要盖在网页上）和自动隐藏的侧栏又盖在它们上面。
    for (CompactToolbar* bar : compact_toolbars_) {
      ReorderChildView(bar, children().size());
    }
    ReorderChildView(pinned_peek_, children().size());
    ReorderChildView(split_overlay_, children().size());
    ReorderChildView(agent_overlay_, children().size());
    ReorderChildView(swipe_indicator_, children().size());
    ReorderChildView(assistant_island_, children().size());
    ReorderChildView(toolbar_, children().size());
    ReorderChildView(tab_strip_, children().size());
    if (window_rail_) {
      ReorderChildView(window_rail_, children().size());
    }
    ReorderChildView(sidebar_, children().size());
    ReorderChildView(sidebar_edge_, children().size());
    // 没有画中画时媒体岛飞进侧栏、标签行：盖在它们上面。
    ReorderChildView(media_island_, children().size());
    ReorderChildView(tab_preview_, children().size());
    ReorderChildView(find_bar_view_, children().size());
    if (window_controls_) {
      ReorderChildView(window_controls_, children().size());
    }
    ReorderChildView(download_launch_, children().size());
    ReorderChildView(archive_toast_, children().size());
    ReorderChildView(downloads_panel_, children().size());
    ReorderChildView(popover_host_, children().size());
    ReorderChildView(command_bar_, children().size());
    ReorderChildView(dialog_host_, children().size());
    ReorderChildView(menu_host_, children().size());
  }
}

std::unique_ptr<FindBar> StarryBrowserView::CreateFindBar() {
  if (!find_bar_view_) {
    return BrowserView::CreateFindBar();
  }
  return std::make_unique<StarryFindBar>(find_bar_view_);
}

bool StarryBrowserView::IsDownloadsPanelOpenOrPending() const {
  return downloads_panel_ && (downloads_panel_->is_showing() ||
                              downloads_panel_timer_.IsRunning());
}

void StarryBrowserView::ToggleDownloadsPanel() {
  if (mini_toolbar_) {
    // 小窗口没有下载按钮（提示条、⌥⌘L 点进来的）：到浏览器窗口看，等它出来、排好版
    // 再弹出。
    BrowserWindowInterface* target =
        mini_window::BrowserWindowFor(browser()->GetProfile());
    if (!target) {
      return;
    }
    target->GetWindow()->Show();
    target->GetWindow()->Activate();
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<BrowserWindowInterface> target) {
              StarryBrowserView* view =
                  target ? StarryBrowserView::From(target.get()) : nullptr;
              if (view && !view->IsDownloadsPanelOpenOrPending()) {
                view->ToggleDownloadsPanel();
              }
            },
            target->GetWeakPtr()),
        base::Milliseconds(200));
    return;
  }
  if (!downloads_panel_) {
    return;
  }
  if (downloads_panel_->is_showing() || downloads_panel_timer_.IsRunning()) {
    downloads_panel_timer_.Stop();
    downloads_panel_->Close();
    active_utility_buttons()->SetDownloadsPanelOpen(false);
    return;
  }
  UtilityButtons* buttons = active_utility_buttons();
  const bool reveal_sidebar = buttons == sidebar_->utility_buttons() &&
                              sidebar_->is_auto_hide() &&
                              !sidebar_->is_peeking();
  const bool button_hidden = buttons->DownloadsButtonBounds().IsEmpty();
  // 先把按钮显示出来（藏起来的、侧栏收着的），等它就位。
  buttons->SetDownloadsPanelOpen(true);
  if (reveal_sidebar) {
    sidebar_->Peek();
  }
  const auto show = [](StarryBrowserView* view) {
    UtilityButtons* buttons = view->active_utility_buttons();
    gfx::Rect anchor = buttons->DownloadsButtonBounds();
    if (anchor.IsEmpty()) {
      buttons->SetDownloadsPanelOpen(false);
      return;
    }
    anchor = views::View::ConvertRectToTarget(buttons, view->downloads_panel(),
                                              anchor);
    view->downloads_panel()->Show(anchor);
  };
  if (!reveal_sidebar && !button_hidden) {
    show(this);
    return;
  }
  downloads_panel_timer_.Start(
      FROM_HERE, base::Milliseconds(reveal_sidebar ? 260 : 200),
      base::BindOnce(show, base::Unretained(this)));
}

void StarryBrowserView::ShowMenu(ui::MenuModel* model,
                                 const gfx::Point& point,
                                 base::OnceClosure closed,
                                 MenuOptions options) {
  if (!menu_host_) {
    std::move(closed).Run();
    return;
  }
  menu_host_->ShowMenuAt(model, point, std::move(closed), std::move(options));
}

void StarryBrowserView::PresentDialog(DialogSpec spec,
                                      DialogAnswerCallback callback) {
  std::unique_ptr<DialogRequest> request =
      MakeDialogRequest(std::move(spec), std::move(callback));
  if (!dialog_host_) {
    request->Dismissed();
    return;
  }
  dialog_host_->Present(std::move(request));
}

void StarryBrowserView::ConfirmBrowserCloseWithPendingDownloads(
    int download_count,
    DownloadCloseType dialog_type,
    base::OnceCallback<void(bool)> callback) {
  if (!dialog_host_) {
    BrowserView::ConfirmBrowserCloseWithPendingDownloads(
        download_count, dialog_type, std::move(callback));
    return;
  }
  PresentDialog(DownloadsCloseDialog(download_count, dialog_type),
                base::BindOnce(
                    [](base::OnceCallback<void(bool)> callback,
                       std::optional<size_t> button,
                       const std::u16string& input) {
                      std::move(callback).Run(button == 0u);
                    },
                    std::move(callback)));
}

int StarryBrowserView::NonClientHitTest(const gfx::Point& point) {
  if (!has_starry_chrome() && !mini_toolbar_) {
    return BrowserView::NonClientHitTest(point);
  }
  // 全屏时顶栏在另一个窗口里（macOS），照上游的处理。
  if (auto* controller = ImmersiveModeController::From(browser());
      controller && controller->IsEnabled()) {
    return BrowserView::NonClientHitTest(point);
  }
  gfx::Point local(point);
  views::View::ConvertPointToTarget(parent(), this, &local);
  if (window_controls_ && window_controls_->GetVisible() &&
      window_controls_->GetMirroredBounds().Contains(local)) {
    // 从整个窗口按 Views 的层级命中，菜单、对话框盖在按钮上时由它们接收点击。
    gfx::Point in_widget(point);
    views::View::ConvertPointToWidget(parent(), &in_widget);
    const int hit = views::GetHitTestComponent(this, in_widget);
    if (hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE) {
      return hit;
    }
  }
  if (IsWindowDraggableAt(this, local)) {
    // HTNOWHERE 交给窗口边框决定：它先看是不是点在自己的按钮上（Windows、Linux 的
    // 最小化、关闭），不是才当成标题栏。直接返回 HTCAPTION 会让那些按钮点不到。
    return HTNOWHERE;
  }
  return views::ClientView::NonClientHitTest(point);
}

void StarryBrowserView::OnThemeChanged() {
  BrowserView::OnThemeChanged();
  UpdateTheme();
}

void StarryBrowserView::UpdateTheme() {
  if (!has_starry_chrome() && !mini_toolbar_) {
    return;
  }
  if (!GetColorProvider()) {
    return;
  }
  // 深浅跟着窗口实际用的配色（系统外观、Chrome 的主题设置）。
  const bool dark = color_utils::IsDark(
      GetColorProvider()->GetColor(ui::kColorWindowBackground));
  const bool incognito = browser()->GetProfile()->IsOffTheRecord();
  base_theme_ = incognito ? Theme::PrivateBrowsing()
                          : Theme::Make(std::nullopt, dark,
                                        /*tint_surfaces=*/false);
  // 跟随网页的 theme-color 着色（缺失时回退到网页底色）；无痕窗口固定紫色，
  // 起始页和内置页面不着色。
  const bool tint =
      !incognito && starry::prefs::PrefsFor(browser()->GetProfile())
                        ->GetBoolean(starry::prefs::kTintFromWebsite);
  Theme target = base_theme_;
  if (tint) {
    tabs::TabInterface* tab = browser()->GetActiveTabInterface();
    content::WebContents* contents = tab ? tab->GetContents() : nullptr;
    if (CanTintFromPage(contents)) {
      std::optional<SkColor> seed = contents->GetThemeColor();
      if (!seed) {
        seed = contents->GetBackgroundColor();
      }
      if (seed) {
        target = Theme::Make(*seed, dark, /*tint_surfaces=*/true);
      }
    }
  }
  for (CompactToolbar* bar : compact_toolbars_) {
    if (bar) {
      bar->SetFollowsPage(tint);
    }
  }
  if (!theme_applied_ || !GetWidget()) {
    theme_applied_ = true;
    theme_from_ = theme_target_ = target;
    theme_motion_->SetValue(1);
    ApplyTheme(target);
    return;
  }
  if (target == theme_target_) {
    return;
  }
  // 主题变色使用 0.8 秒平滑缓动（motion::ChromeTint）。
  theme_from_ = theme_;
  theme_target_ = target;
  theme_motion_->SetValue(0);
  theme_motion_->AnimateTo(1, motion::ChromeTint());
}

void StarryBrowserView::StepTheme() {
  ApplyTheme(Theme::Lerp(theme_from_, theme_target_, theme_motion_->value()));
}

void StarryBrowserView::ApplyTheme(const Theme& theme) {
  theme_ = theme;
  background_view_->SetTheme(theme_);
  if (mini_toolbar_) {
    mini_toolbar_->SetTheme(theme_);
    if (window_controls_) {
      window_controls_->SetTheme(theme_);
    }
    command_bar_->SetTheme(theme_);
    menu_host_->SetTheme(theme_);
    find_bar_view_->SetTheme(theme_);
    dialog_host_->SetTheme(theme_);
    download_launch_->SetTheme(theme_);
    popover_host_->SetTheme(theme_);
    swipe_indicator_->SetTheme(theme_);
    return;
  }
  sidebar_->SetTheme(theme_);
  tab_strip_->SetTheme(theme_);
  toolbar_->SetTheme(theme_);
  for (CompactToolbar* bar : compact_toolbars_) {
    bar->SetWindowTheme(theme_);
  }
  command_bar_->SetTheme(theme_);
  menu_host_->SetTheme(theme_);
  find_bar_view_->SetTheme(theme_);
  dialog_host_->SetTheme(theme_);
  downloads_panel_->SetTheme(theme_);
  download_launch_->SetTheme(theme_);
  archive_toast_->SetTheme(theme_);
  popover_host_->SetTheme(theme_);
  split_overlay_->SetTheme(theme_);
  tab_preview_->SetTheme(theme_);
  agent_overlay_->SetTheme(theme_);
  swipe_indicator_->SetTheme(theme_);
  assistant_island_->SetTheme(theme_);
  media_island_->SetTheme(theme_);
  if (window_rail_) {
    window_rail_->SetTheme(theme_);
  }
  UpdateWindowControlsTheme();
}

void StarryBrowserView::UpdateMediaIslandStacking(bool has_video) {
  views::View* anchor = has_video
                            ? static_cast<views::View*>(multi_contents_view())
                            : sidebar_edge_.get();
  const size_t anchor_index = GetIndexOf(anchor).value();
  const size_t island_index = GetIndexOf(media_island_).value();
  ReorderChildView(media_island_,
                   anchor_index + (island_index > anchor_index ? 1 : 0));
}

gfx::Rect StarryBrowserView::MediaDockBounds() {
  if (!sidebar_ || !tab_strip_) {
    return gfx::Rect();
  }
  if (layout_.tab_strip()) {
    return views::View::ConvertRectToTarget(
        tab_strip_, this, tab_strip_->MediaCapsuleTargetBounds());
  }
  return sidebar_->MediaDockCardBounds(this);
}

void StarryBrowserView::RevealMediaDock() {
  if (!sidebar_ || !tab_strip_) {
    return;
  }
  if (layout_.tab_strip()) {
    tab_strip_->media_capsule()->Reveal();
  } else {
    sidebar_->media_dock()->Reveal();
  }
}

std::vector<gfx::Rect> StarryBrowserView::MediaObstacles() {
  std::vector<gfx::Rect> rects;
  const auto add = [this, &rects](views::View* view, const gfx::Rect& rect) {
    if (view && !rect.IsEmpty()) {
      rects.push_back(views::View::ConvertRectToTarget(view, this, rect));
    }
  };
  if (window_controls_ && window_controls_->GetVisible()) {
    add(window_controls_, window_controls_->GetLocalBounds());
  }
  if (assistant_island_) {
    add(assistant_island_, assistant_island_->FloatingRect());
  }
  if (find_bar_view_) {
    add(find_bar_view_, find_bar_view_->ShownRect());
  }
  if (agent_overlay_) {
    for (const gfx::Rect& rect : agent_overlay_->ObstacleRects()) {
      add(agent_overlay_, rect);
    }
  }
  // 自动整理的提示条（内容卡片左下角）：岛在左下角时往上让。
  if (archive_toast_) {
    add(archive_toast_, archive_toast_->ToastRect());
  }
  // 桌面的画中画小窗浮在所有窗口上面：盖到这个窗口时也让开（默认都在右下角）。
  if (MediaPip* pip = MediaPip::Current()) {
    if (views::Widget* desktop = pip->desktop_widget()) {
      const gfx::Rect screen = desktop->GetWindowBoundsInScreen();
      gfx::Point origin = screen.origin();
      views::View::ConvertPointFromScreen(this, &origin);
      rects.emplace_back(origin, screen.size());
    }
  }
  return rects;
}

void StarryBrowserView::MediaObstaclesChanged() {
  if (media_island_) {
    media_island_->UpdateAvoidance();
  }
}

void StarryBrowserView::ToggleAssistant() {
  if (assistant_island_) {
    assistant_island_->Toggle();
  }
}

void StarryBrowserView::ShowBookmarkBubble(const GURL& url,
                                           bool already_bookmarked) {
  if (!popover_host_) {
    BrowserView::ShowBookmarkBubble(url, already_bookmarked);
    return;
  }
  // 侧栏在时挂在地址栏下面；侧栏收着时放在内容卡片的左上角。
  gfx::Rect anchor;
  PopoverHost::Placement placement = PopoverHost::Placement::kBelow;
  if (views::View* pill = visible_address_view()) {
    anchor = views::View::ConvertRectToTarget(pill, popover_host_,
                                              pill->GetLocalBounds());
  } else {
    anchor = PageBounds();
    placement = PopoverHost::Placement::kInsideTopLeft;
  }
  popover_host_->CancelAutoClose();
  popover_host_->Show(std::make_unique<BookmarkEditor>(
                          browser(), popover_host_, url, already_bookmarked),
                      anchor, placement);
}

void StarryBrowserView::OnZoomChanged(bool can_show_bubble) {
  if (!popover_host_) {
    return;
  }
  if (auto* chrome_bubble = ZoomBubbleCoordinator::From(browser());
      chrome_bubble && chrome_bubble->IsShowing()) {
    chrome_bubble->Hide();
  }
  auto* indicator =
      views::AsViewClass<ZoomIndicator>(popover_host_->content());
  if (indicator) {
    indicator->Update();
    popover_host_->ContentSizeChanged();
  }
  if (!can_show_bubble) {
    return;
  }
  if (!indicator) {
    // 内容卡片的右上角（紧凑地址栏下面，查找栏开着时在它下面）。
    gfx::Rect anchor = BoundsBelowWindowControls(PageBounds());
    if (find_bar_view_->is_shown()) {
      anchor.Inset(gfx::Insets::TLBR(52, 0, 0, 0));
    }
    popover_host_->Show(std::make_unique<ZoomIndicator>(browser()), anchor,
                        PopoverHost::Placement::kInsideTopRight);
  }
  popover_host_->ScheduleAutoClose(base::Seconds(1.5));
}

BEGIN_METADATA(StarryBrowserView)
END_METADATA

}  // namespace starry
