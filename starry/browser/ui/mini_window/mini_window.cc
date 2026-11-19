// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/mini_window/mini_window.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

#include "base/auto_reset.h"
#include "base/callback_list.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"
#include "chrome/browser/ui/startup/startup_tab.h"
#include "chrome/browser/ui/startup/startup_tab_provider.h"
#include "chrome/browser/ui/startup/startup_types.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/mini_window/mini_window_activation.h"
#include "starry/browser/ui/mini_window/mini_window_transfer.h"
#include "starry/browser/ui/tabs/pinned_peek.h"
#include "ui/base/base_window.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"
#include "url/url_constants.h"

#if BUILDFLAG(IS_MAC)
#include <unistd.h>
#endif

namespace starry::mini_window {

namespace {

// Create 正在建小窗口（StarryBrowserView 的构造函数里问）。
bool g_creating = false;
// MoveToBrowser 正在截图（过渡的起点）：这时再按一次不算。
bool g_preparing_move = false;

// ---- 推迟的启动窗口 ----

// 启动时 Chrome 要开的窗口（StartupBrowserCreator::LaunchBrowser 的参数）。
struct DeferredLaunch {
  base::CommandLine command_line{base::CommandLine::NO_PROGRAM};
  base::FilePath profile_path;
  base::FilePath cur_dir;
  bool restore_tabbed_browser = true;
  // true：开了小窗口，等第一次要用浏览器窗口。false：macOS 上还在等启动时的链接。
  bool for_mini_window = false;
};

// macOS 上等启动时的链接：LaunchServices 先启动 App、在 applicationDidFinishLaunching 之后
// 才发链接的 Apple 事件（实测晚几毫秒）。那之后再等这么久，没来就照常打开窗口。
constexpr base::TimeDelta kLaunchUrlsGrace = base::Milliseconds(250);
// 移到浏览器时最多等会话恢复这么久。
constexpr base::TimeDelta kRestoreWaitLimit = base::Seconds(3);

// 有浏览器窗口建起来了（Chrome 自己开的：⌘ 点链接、⌘N……，它会按设置恢复会话）：推迟的
// 启动窗口就不要了，不然之后又会恢复一次。
class DeferredLaunchState : public BrowserCollectionObserver {
 public:
  static DeferredLaunchState& Get() {
    static base::NoDestructor<DeferredLaunchState> state;
    return *state;
  }

  std::optional<DeferredLaunch>& launch() { return launch_; }
  bool resuming() const { return resuming_; }

  void Set(DeferredLaunch launch) {
    launch_ = std::move(launch);
    if (!observation_.IsObserving()) {
      observation_.Observe(GlobalBrowserCollection::GetInstance());
    }
  }

  std::optional<DeferredLaunch> Take() {
    std::optional<DeferredLaunch> launch = std::move(launch_);
    launch_.reset();
    observation_.Reset();
    return launch;
  }

  // 打开推迟的启动窗口：和启动时一样（按「启动时」的设置恢复上次的窗口或开新标签页）。
  bool Resume() {
    std::optional<DeferredLaunch> launch = Take();
    if (!launch || !g_browser_process->profile_manager()) {
      return false;
    }
    Profile* profile = g_browser_process->profile_manager()->GetProfileByPath(
        launch->profile_path);
    if (!profile) {
      return false;
    }
    base::AutoReset<bool> resuming(&resuming_, true);
    StartupBrowserCreator creator;
    creator.LaunchBrowser(launch->command_line, profile, launch->cur_dir,
                          chrome::startup::IsProcessStartup::kYes,
                          chrome::startup::IsFirstRun::kNo,
                          launch->restore_tabbed_browser);
    return true;
  }

  // BrowserCollectionObserver:
  void OnBrowserCreated(BrowserWindowInterface* browser) override {
    if (resuming_ ||
        browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
        browser->GetProfile()->IsOffTheRecord()) {
      return;
    }
    // 还在等启动时的链接：Chrome 自己开了窗口（比如别的启动方式），不再等。
    Take();
  }

 private:
  std::optional<DeferredLaunch> launch_;
  base::ScopedObservation<BrowserCollection, BrowserCollectionObserver>
      observation_{this};
  bool resuming_ = false;
};

// 用户看得到的浏览器窗口（普通窗口，显示着或最小化了）。智能体的临时无痕窗口平时不显示，
// 不算。
bool HasBrowserWindow() {
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
        browser->IsDeleteScheduled() || !browser->GetWindow()) {
      continue;
    }
    if (browser->GetWindow()->IsVisible() ||
        browser->GetWindow()->IsMinimized()) {
      return true;
    }
  }
  return false;
}

// 还在等启动时的链接（没有为小窗口推迟）：不等了，照常打开启动窗口。
void ResumeIfAwaitingUrls() {
  std::optional<DeferredLaunch>& launch = DeferredLaunchState::Get().launch();
  if (launch && !launch->for_mini_window) {
    DeferredLaunchState::Get().Resume();
  }
}

bool HasMiniWindow() {
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (IsMiniWindow(browser) && !browser->IsDeleteScheduled()) {
      return true;
    }
  }
  return false;
}

// 命令行里的网址（不算开关）：Chrome 打开它们的同一个办法（StartupTabProviderImpl）。
std::vector<GURL> CommandLineUrls(const base::CommandLine& command_line,
                                  const base::FilePath& cur_dir,
                                  Profile* profile) {
  std::vector<GURL> urls;
  for (const StartupTab& tab : StartupTabProviderImpl().GetCommandLineTabs(
           command_line, cur_dir, profile)) {
    urls.push_back(tab.url);
  }
  return urls;
}

// 推迟的启动不再带网址（它们已经在小窗口里了）。
base::CommandLine WithoutArguments(const base::CommandLine& command_line) {
  base::CommandLine stripped(command_line.GetProgram());
  for (const auto& [name, value] : command_line.GetSwitches()) {
    stripped.AppendSwitchNative(name, value);
  }
  return stripped;
}

// 移到浏览器窗口：等它恢复完会话再放（不然恢复出来的标签会把它挤到后面、换走选中）。
struct PendingMove {
  base::WeakPtr<BrowserWindowInterface> mini;
  base::WeakPtr<BrowserWindowInterface> target;
  // 目标窗口是刚为它打开的：里面只有一个空白的新标签页时换掉。
  bool replace_blank = false;
  // 盖在小窗口网页上的截图，放过去以后飞到浏览器窗口。
  base::WeakPtr<Transfer> transfer;
  base::CallbackListSubscription subscription;
};

std::unique_ptr<PendingMove>& Pending() {
  static base::NoDestructor<std::unique_ptr<PendingMove>> pending;
  return *pending;
}

void FinishMove(BrowserWindowInterface* mini,
                BrowserWindowInterface* target,
                bool replace_blank,
                base::WeakPtr<Transfer> transfer);

void CancelTransfer(const base::WeakPtr<Transfer>& transfer) {
  if (transfer) {
    transfer->Cancel();
  }
}

void FinishPendingMove() {
  std::unique_ptr<PendingMove> move = std::move(Pending());
  if (!move) {
    return;
  }
  if (!move->mini || !move->target) {
    CancelTransfer(move->transfer);
    return;
  }
  FinishMove(move->mini.get(), move->target.get(), move->replace_blank,
             move->transfer);
}

void FinishMove(BrowserWindowInterface* mini,
                BrowserWindowInterface* target,
                bool replace_blank,
                base::WeakPtr<Transfer> transfer) {
  TabStripModel* source = mini->GetTabStripModel();
  TabStripModel* destination = target->GetTabStripModel();
  if (source->empty() || source->active_index() == TabStripModel::kNoTab) {
    CancelTransfer(transfer);
    return;
  }
  content::WebContents* const contents = source->GetActiveWebContents();
  content::WebContents* blank = nullptr;
  if (replace_blank && destination->count() == 1) {
    content::WebContents* only = destination->GetWebContentsAt(0);
    if (IsBlankTabUrl(only->GetVisibleURL()) &&
        !only->GetController().CanGoBack()) {
      blank = only;
    }
  }
  if (StarryBrowserView* view = StarryBrowserView::From(mini)) {
    view->WillMoveToBrowser();
  }
  const int index = destination->active_index() == TabStripModel::kNoTab
                        ? destination->count()
                        : destination->active_index() + 1;
  std::unique_ptr<tabs::TabModel> tab =
      source->DetachTabAtForInsertion(source->active_index());
  // 插入当前标签之后并激活；关闭该标签时返回原标签。
  destination->InsertDetachedTabAt(
      index, std::move(tab),
      AddTabTypes::ADD_ACTIVE | AddTabTypes::ADD_INHERIT_OPENER);
  if (blank) {
    const int blank_index = destination->GetIndexOfWebContents(blank);
    if (blank_index != TabStripModel::kNoTab) {
      destination->CloseWebContentsAt(blank_index, TabCloseTypes::CLOSE_NONE);
    }
  }
  target->GetWindow()->Show();
  target->GetWindow()->Activate();
  // 小窗口没有标签了，Chrome 自己关掉它（Browser::TabStripEmpty）；截图还盖在原来的地方，
  // 等网页放进浏览器窗口再飞过去。
  if (transfer) {
    transfer->FlyTo(target, contents);
  }
}

// 截好图、盖住小窗口的网页之后（不做过渡时马上）：找到或打开浏览器窗口，放过去。
void StartMove(base::WeakPtr<BrowserWindowInterface> mini_ptr,
               base::WeakPtr<Transfer> transfer) {
  g_preparing_move = false;
  BrowserWindowInterface* const mini = mini_ptr.get();
  if (!mini || !IsMiniWindow(mini) || mini->IsDeleteScheduled() ||
      mini->GetTabStripModel()->empty() || Pending()) {
    CancelTransfer(transfer);
    return;
  }
  Profile* profile = mini->GetProfile();
  BrowserWindowInterface* target =
      ProfileBrowserCollection::GetForProfile(profile)->FindTabbedBrowser();
  bool replace_blank = false;
  if (!target) {
    // 没有浏览器窗口：先打开推迟的启动窗口（恢复上次的标签），再没有就开一个空的。
    if (ResumeDeferredLaunch()) {
      target =
          ProfileBrowserCollection::GetForProfile(profile)->FindTabbedBrowser();
      replace_blank = true;
    }
    if (!target) {
      target = CreateBrowserWindow(
          BrowserWindowCreateParams(profile, /*from_user_gesture=*/true));
    }
  }
  if (!target) {
    CancelTransfer(transfer);
    return;
  }
  if (!SessionRestore::IsRestoring(profile)) {
    FinishMove(mini, target, replace_blank, transfer);
    return;
  }
  // 会话还在恢复：恢复完再放过去。
  auto pending = std::make_unique<PendingMove>();
  pending->mini = mini->GetWeakPtr();
  pending->target = target->GetWeakPtr();
  pending->replace_blank = replace_blank;
  pending->transfer = transfer;
  pending->subscription = SessionRestore::RegisterOnSessionRestoredCallback(
      base::BindRepeating([](Profile* profile, int num_tabs_restored) {
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&FinishPendingMove));
      }));
  Pending() = std::move(pending);
  // 恢复完的通知万一没来：等一会儿也放过去，小窗口不能一直移不动。
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&FinishPendingMove), kRestoreWaitLimit);
}

}  // namespace

bool OpensInMiniWindow(Profile* profile, const std::vector<GURL>& urls) {
  if (!profile || profile->IsOffTheRecord() || urls.size() != 1 ||
      !urls[0].SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  if (IncognitoModePrefs::GetAvailability(profile) ==
      policy::IncognitoModeAvailability::kForced) {
    return false;
  }
  return prefs::PrefsFor(profile)->GetString(prefs::kExternalLinks) !=
         prefs::kExternalLinksBrowserWindow;
}

gfx::Size WindowSize(int saved_width, int saved_height) {
  if (saved_width <= 0 || saved_height <= 0) {
    return gfx::Size(kDefaultWidth, kDefaultHeight);
  }
  return gfx::Size(std::max(saved_width, kMinWidth),
                   std::max(saved_height, kMinHeight));
}

gfx::Rect WindowBounds(const gfx::Size& size,
                       const gfx::Rect& work_area,
                       const std::vector<gfx::Rect>& occupied) {
  const int width = std::min(size.width(), work_area.width());
  const int height = std::min(size.height(), work_area.height());
  double left = work_area.x() + (work_area.width() - width) / 2.0;
  double top = work_area.y() + (work_area.height() - height) * 0.45;
  for (int i = 0; i < kMaxCascades; ++i) {
    const bool taken = std::ranges::any_of(occupied, [&](const gfx::Rect& r) {
      return std::abs(r.x() - left) < kCascadeStep / 2.0 &&
             std::abs(r.y() - top) < kCascadeStep / 2.0;
    });
    if (!taken) {
      break;
    }
    left = std::min(left + kCascadeStep,
                    static_cast<double>(work_area.right() - width));
    top = std::min(top + kCascadeStep,
                   static_cast<double>(work_area.bottom() - height));
  }
  return gfx::Rect(static_cast<int>(std::lround(left)),
                   static_cast<int>(std::lround(top)), width, height);
}

BrowserWindowInterface* Create(Profile* profile, bool user_gesture) {
  if (!profile || GetBrowserWindowCreationStatusForProfile(*profile) !=
                      BrowserWindowInterface::CreationStatus::kOk) {
    return nullptr;
  }
  // 放在鼠标所在的屏幕上（刚点的链接多半在那里），大小沿用上次拖出来的。
  PrefService* prefs = prefs::PrefsFor(profile);
  const gfx::Size size =
      WindowSize(prefs->GetInteger(prefs::kMiniWindowWidth),
                 prefs->GetInteger(prefs::kMiniWindowHeight));
  display::Screen* screen = display::Screen::Get();
  const display::Display display =
      screen->GetDisplayNearestPoint(screen->GetCursorScreenPoint());
  std::vector<gfx::Rect> occupied;
  for (BrowserWindowInterface* each : GetAllBrowserWindowInterfaces()) {
    if (IsMiniWindow(each) && each->GetWindow()) {
      occupied.push_back(each->GetWindow()->GetBounds());
    }
  }

  const gfx::Rect bounds = WindowBounds(size, display.work_area(), occupied);
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP, profile,
                                   user_gesture);
  params.initial_bounds = bounds;
  params.initial_origin_specified =
      BrowserWindowCreateParams::ValueSpecified::kSpecified;
  params.omit_from_session_restore = true;
  params.should_trigger_session_restore = false;
  BrowserWindowInterface* mini = nullptr;
  {
    base::AutoReset<bool> creating(&g_creating, true);
    mini = CreateBrowserWindow(std::move(params));
  }
  // 弹出窗口的 initial_bounds 是网页那一块的大小（window.open 的写法），Chrome 会再加上
  // 标题栏；小窗口的内容铺满整个窗口，按窗口的大小重新摆一次。
  if (mini && mini->GetWindow()) {
    mini->GetWindow()->SetBounds(bounds);
  }
  return mini;
}

BrowserWindowInterface* Open(Profile* profile, const GURL& url) {
  BrowserWindowInterface* mini = Create(profile, /*user_gesture=*/true);
  if (!mini) {
    return nullptr;
  }
  NavigateParams params(mini, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  params.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&params);
  return mini;
}

bool IsMiniWindow(BrowserWindowInterface* browser) {
  if (!browser || browser->GetType() != BrowserWindowInterface::TYPE_POPUP) {
    return false;
  }
  StarryBrowserView* view = StarryBrowserView::From(browser);
  return view && view->is_mini_window();
}

bool IsCreating() {
  return g_creating;
}

void MoveToBrowser(BrowserWindowInterface* mini) {
  if (!IsMiniWindow(mini) || mini->GetTabStripModel()->empty() || Pending() ||
      g_preparing_move) {
    return;
  }
  // 先截图盖住小窗口的网页（飞到浏览器窗口的起点），再移过去。
  g_preparing_move = true;
  Transfer::Prepare(mini, base::BindOnce(&StartMove, mini->GetWeakPtr()));
}

BrowserWindowInterface* BrowserWindowFor(Profile* profile) {
  profile = profile->GetOriginalProfile();
  ProfileBrowserCollection* collection =
      ProfileBrowserCollection::GetForProfile(profile);
  if (BrowserWindowInterface* browser = collection->FindTabbedBrowser()) {
    return browser;
  }
  if (ResumeDeferredLaunch()) {
    if (BrowserWindowInterface* browser = collection->FindTabbedBrowser()) {
      return browser;
    }
  }
  return chrome::OpenEmptyWindow(profile, /*should_trigger_session_restore=*/
                                 true);
}

bool InterceptLaunchBrowser(const base::CommandLine& command_line,
                            Profile* profile,
                            const base::FilePath& cur_dir,
                            bool process_startup,
                            bool first_run,
                            bool restore_tabbed_browser) {
  DeferredLaunchState& state = DeferredLaunchState::Get();
  if (state.resuming() || !profile || profile->IsOffTheRecord() || first_run ||
      command_line.HasSwitch(switches::kNoStartupWindow) ||
      command_line.HasSwitch(switches::kIncognito)) {
    return false;
  }
  // 重新启动（改了语言、更新）要恢复上次的窗口：照常开。这两个只在启动的那一刻读得到。
  if (process_startup &&
      (StartupBrowserCreator::WasRestarted() ||
       StartupBrowserCreator::ShouldRestoreLastSession(command_line))) {
    return false;
  }
  const std::vector<GURL> urls =
      CommandLineUrls(command_line, cur_dir, profile);
#if BUILDFLAG(IS_MAC)
  // macOS 的链接经 Apple 事件进来，命令行里有网址的是脚本、测试直接启动的：照常开。
  // 已经在运行时（另一个进程把命令行转过来）和 Windows、Linux 一样。
  if (process_startup) {
    // 由 LaunchServices 启动（程序坞、访达、其他 App 打开的链接，父进程是 launchd）：
    // 先不开，等 DidFinishLaunching 看这次是不是为了打开一个链接。
    if (!urls.empty() || getppid() != 1) {
      return false;
    }
    LOG(INFO) << "[starry] mini-window: launch deferred until startup urls";
    DeferredLaunch launch;
    launch.command_line = command_line;
    launch.profile_path = profile->GetPath();
    launch.cur_dir = cur_dir;
    launch.restore_tabbed_browser = restore_tabbed_browser;
    state.Set(std::move(launch));
    // 万一 applicationDidFinishLaunching 没走到时的兜底（从推迟的那一刻算）。
    constexpr base::TimeDelta kLaunchUrlsTimeout = base::Seconds(3);
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&ResumeIfAwaitingUrls), kLaunchUrlsTimeout);
    return true;
  }
#endif
  if (!OpensInMiniWindow(profile, urls)) {
    return false;
  }
  if (process_startup) {
    DeferredLaunch launch;
    launch.command_line = WithoutArguments(command_line);
    launch.profile_path = profile->GetPath();
    launch.cur_dir = cur_dir;
    launch.restore_tabbed_browser = restore_tabbed_browser;
    launch.for_mini_window = true;
    state.Set(std::move(launch));
  }
  Open(profile, urls[0]);
  return true;
}

void DidFinishLaunching(const std::vector<GURL>& urls, bool default_launch) {
  DeferredLaunchState& state = DeferredLaunchState::Get();
  std::optional<DeferredLaunch>& launch = state.launch();
  LOG(INFO) << "[starry] mini-window: did finish launching urls=" << urls.size()
            << " default=" << default_launch
            << " deferred=" << launch.has_value();
  if (!launch || launch->for_mini_window) {
    return;
  }
  // 不是从程序坞直接打开、链接还没到：再等一小会儿 OpenExternalLinks。
  if (urls.empty() && !default_launch) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&ResumeIfAwaitingUrls), kLaunchUrlsGrace);
    return;
  }
  Profile* profile =
      g_browser_process->profile_manager()
          ? g_browser_process->profile_manager()->GetProfileByPath(
                launch->profile_path)
          : nullptr;
  if (profile && OpensInMiniWindow(profile, urls)) {
    // 只为了打开这个链接：只开小窗口（Chrome 接着把它交给 OpenExternalLinks）。
    launch->for_mini_window = true;
    return;
  }
  // 普通的启动，或者链接要放进浏览器窗口：先照常打开，Chrome 再把链接放进去。
  state.Resume();
}

bool OpenExternalLinks(Profile* profile, const std::vector<GURL>& urls) {
  DeferredLaunchState& state = DeferredLaunchState::Get();
  std::optional<DeferredLaunch>& launch = state.launch();
  const bool awaiting_urls = launch && !launch->for_mini_window;
  LOG(INFO) << "[starry] mini-window: external links " << urls.size()
            << " awaiting=" << awaiting_urls;
  if (!OpensInMiniWindow(profile, urls)) {
    // 放进浏览器窗口：激活时带出来的窗口就该在前面。启动窗口还在等链接时先打开它。
    DiscardWindowOrder();
    if (awaiting_urls) {
      state.Resume();
    }
    return false;
  }
  if (awaiting_urls) {
    // 启动就是为了这个链接：只开小窗口，启动窗口推迟到要用浏览器窗口时。
    launch->for_mini_window = true;
  }
  return Open(profile, urls[0]) != nullptr;
}

bool ResumeDeferredLaunch() {
  LOG(INFO) << "[starry] mini-window: resume deferred launch";
  return DeferredLaunchState::Get().Resume();
}

bool HandleReopen() {
  DiscardWindowOrder();
  const bool deferred = DeferredLaunchState::Get().launch().has_value();
  if (HasBrowserWindow() || (!deferred && !HasMiniWindow())) {
    return false;
  }
  if (ResumeDeferredLaunch() && HasBrowserWindow()) {
    return true;
  }
  Profile* profile = ProfileManager::GetLastUsedProfileIfLoaded();
  if (!profile) {
    return false;
  }
  chrome::NewEmptyWindow(profile->GetOriginalProfile());
  return true;
}

}  // namespace starry::mini_window

namespace starry {

// 上游的钩子（starry_hooks.h）。

bool InterceptLaunchBrowser(const base::CommandLine& command_line,
                            Profile* profile,
                            const base::FilePath& cur_dir,
                            bool process_startup,
                            bool first_run,
                            bool restore_tabbed_browser) {
  return mini_window::InterceptLaunchBrowser(command_line, profile, cur_dir,
                                             process_startup, first_run,
                                             restore_tabbed_browser);
}

namespace {

std::vector<GURL> TabUrls(const std::vector<StartupTab>& tabs) {
  std::vector<GURL> urls;
  for (const StartupTab& tab : tabs) {
    urls.push_back(tab.url);
  }
  return urls;
}

}  // namespace

void DidFinishLaunching(const std::vector<StartupTab>& tabs,
                        bool default_launch) {
  mini_window::DidFinishLaunching(TabUrls(tabs), default_launch);
}

bool OpenExternalLinks(const std::vector<StartupTab>& tabs, Profile* profile) {
  return mini_window::OpenExternalLinks(profile, TabUrls(tabs));
}

bool HandleDockReopen() {
  return mini_window::HandleReopen();
}

BrowserWindowInterface* MiniWindowForNavigation(const NavigateParams& params) {
  // 小窗口只放一个标签：网页自己开的窗口（window.open、target=_blank，要保留 opener，
  // 登录、支付用得到）另开一个小窗口，`window.close()` 时关掉它就回到这里。⌘ 点链接、
  // 右键「在新标签页打开」（后台标签、没有网页建好的 WebContents）照常去浏览器窗口。
  if (!params.browser || !params.contents_to_insert ||
      !mini_window::IsMiniWindow(params.browser)) {
    return nullptr;
  }
  switch (params.disposition) {
    case WindowOpenDisposition::NEW_FOREGROUND_TAB:
    case WindowOpenDisposition::NEW_POPUP:
    case WindowOpenDisposition::NEW_WINDOW:
      break;
    default:
      return nullptr;
  }
  return mini_window::Create(params.browser->GetProfile(), params.user_gesture);
}

bool HandleOpenFileCommand(BrowserWindowInterface* browser) {
  auto* view = StarryBrowserView::From(browser);
  if (view && view->pinned_peek() && view->pinned_peek()->preview()) {
    view->pinned_peek()->Expand();
    return true;
  }
  // 小窗口里的 ⌘O：移到浏览器窗口。
  if (!mini_window::IsMiniWindow(browser)) {
    return false;
  }
  mini_window::MoveToBrowser(browser);
  return true;
}

bool UsesStarryWindowFrame(BrowserView* browser_view) {
  StarryBrowserView* view = views::AsViewClass<StarryBrowserView>(browser_view);
  return view && view->is_mini_window();
}

int StarryTitlebarHeight(BrowserView* browser_view) {
  return UsesStarryWindowFrame(browser_view) ? metrics::kToolbarHeight : 0;
}

}  // namespace starry
