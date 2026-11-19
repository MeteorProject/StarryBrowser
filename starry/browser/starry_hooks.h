// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_STARRY_HOOKS_H_
#define STARRY_BROWSER_STARRY_HOOKS_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/permissions/permission_prompt.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/common/javascript_dialog_type.h"
#include "extensions/browser/extension_install_prompt_client.h"

class BrowserView;
class BrowserViewLayout;
class BrowserViewLayoutDelegate;
class BrowserWindowInterface;
class ContentsContainerView;
class ExtensionInstallPromptShowParams;
class Profile;
class RenderViewContextMenuBase;
class TabStripModel;
struct NavigateParams;
struct StartupTab;

namespace base {
class CommandLine;
class DictValue;
class FilePath;
}  // namespace base

namespace content {
class BrowserContext;
class DownloadManager;
class NavigationHandle;
class WebContents;
}

namespace download {
class DownloadItem;
}

namespace extensions {
class InstallPromptData;
}

namespace javascript_dialogs {
class TabModalDialogView;
}

namespace tabs {
class TabInterface;
}

struct BrowserViewLayoutViews;

// 供 patches/ 中的上游补丁钩子调用的函数。上游文件 #include 这个头文件时需加
// `// nogncheck`：//starry/browser 依赖 //chrome/browser/ui，反过来不能再依赖，符号在
// 链接 Chrome 时解析。所以这里只用前置声明。
namespace starry {

// chrome/browser/picture_in_picture/auto_picture_in_picture_tab_helper.cc：
// 窗内画中画由媒体岛在切回标签时飞回播放器，落地后再关闭；Chrome 的自动退出不能
// 提前关掉它。桌面小窗照 Chrome 的逻辑处理。
bool DeferAutoPictureInPictureExit(content::WebContents* contents);
// Chromium 自己触发的自动画中画也遵守「媒体」设置。
bool AllowAutoPictureInPicture(content::WebContents* contents);

// chrome/browser/history/history_tab_helper.cc：replaceState 只更新当前页的位置，
// 不新增浏览历史；真正的导航（包括 pushState、加载新文档的替换）照常记录。
bool ShouldRecordHistoryNavigation(content::NavigationHandle* navigation);

// chrome/browser/sessions/session_restore.cc 的 FinishedTabCreation：恢复上次的窗口和
// 标签时（冷启动、重新启动、崩溃后恢复），每个窗口里不是当前显示的标签交给 Chrome 在
// 后台逐个载入之前先问这里。设置里开着「恢复的标签页先休眠」时把它标成休眠、返回 true，
// 不排进载入队列，切过去时 Chrome 才载入。
// 见 starry/browser/sessions/restored_tabs.cc。
bool DeferRestoredTabLoad(content::WebContents* contents);

// chrome/browser/ui/views/session_crashed_bubble_view.cc：崩溃后的会话恢复提示用
// Starry 的对话框，不依赖隐藏的 Chrome 菜单按钮和窗口尚未显示时不可用的气泡锚点。
// 接管了返回 true；保留 ExitTypeService 的崩溃锁，用户选择以后才确认或恢复会话。
bool ShowSessionCrashedDialog(BrowserWindowInterface* browser,
                              bool skip_tab_checking);

// 返回 true 时 Starry 已处理关闭命令。
bool HandleCloseTabCommand(BrowserWindowInterface* browser);
// 普通冷启动通过 PinnedTabCodec 读写固定项的元数据。
bool EncodeStartupPin(content::WebContents* contents, base::DictValue& value);
void EnsureRestoredPin(content::WebContents* contents);
void DecodeStartupPin(const base::DictValue& value, StartupTab& tab);
bool RestoreStartupPin(BrowserWindowInterface* browser,
                       const StartupTab& tab,
                       int add_types);
bool FinishPinnedTabClose(BrowserWindowInterface* browser,
                          content::WebContents* contents);
void AddTabExtraData(content::WebContents* contents,
                     bool pinned,
                     std::map<std::string, std::string>* extra_data);
void RestoreTabExtraData(content::WebContents* contents,
                         bool from_session_restore,
                         const std::map<std::string, std::string>& extra_data);

namespace split {
// Preserve a surviving pair after a tab in a three-pane group is detached.
void PreserveRemainingPanes(TabStripModel* model, tabs::TabInterface* removing);
}  // namespace split

// 树状标签页（starry/browser/ui/tabs/tab_tree.h）。
namespace tab_tree {
// chrome/browser/ui/tabs/tab_strip_model.cc 的 AddTab：网页里的链接打开的新标签放在哪
// （chrome_index 是 Chrome 算的）。开着树时放在打开它的标签整枝的末尾，记下父标签。
int InsertionIndex(TabStripModel* model,
                   tabs::TabInterface* tab,
                   int chrome_index);
// RemoveTabFromIndexImpl：关掉当前标签后切到哪（关掉以后的下标）。
std::optional<int> SelectionAfterClose(TabStripModel* model,
                                       tabs::TabInterface* tab,
                                       std::optional<int> chrome_choice);
// chrome/browser/ui/navigator/browser_navigator.cc：新标签是哪个标签里的网页打开的。
void NoteSource(content::WebContents* source, content::WebContents* contents);
// 侧栏的树里折起来、看不到的标签：⌃Tab、⌘1…8 跳过（同折叠的标签组）。
bool IsHidden(TabStripModel* model, int index);
// 复制标签页：新的放在原来那一枝的后面、和原来的同级，返回插入的位置
// （chrome_index 是 Chrome 的，紧挨着原来的）。
int DuplicateIndex(TabStripModel* model,
                   int index,
                   int chrome_index,
                   content::WebContents* duplicate);
}  // namespace tab_tree

// chrome/browser/ui/views/frame/browser_window_factory.cc：窗口用 StarryBrowserView。
BrowserView* CreateBrowserView(BrowserWindowInterface* browser);

// chrome/browser/ui/browser_command_controller.cc 的 IDC_NEW_TAB：Starry 的窗口里打开
// 命令栏（选好了才建标签），返回 true；别的窗口返回 false，照 Chrome 的做。
bool HandleNewTabCommand(BrowserWindowInterface* browser);

// chrome/browser/ui/views/tab_contents/chrome_web_contents_view_delegate_views
// （.cc 和 _mac.mm）的 ShowMenu：网页的右键菜单用 Starry 的菜单显示。接管了就把 menu
// 拿走并返回 true；不在 Starry 的窗口里时返回 false，照 Chrome 的做。
bool ShowWebContextMenu(std::unique_ptr<RenderViewContextMenuBase>& menu);

// chrome/browser/ui/views/javascript_tab_modal_dialog_view_views.cc 的
// CreateNewDialog：网页的 alert / confirm / prompt 用 Starry 的对话框。接管了就拿走两个
// 回调、返回对话框；不在 Starry 的窗口里时返回空的，照 Chrome 的做。
base::WeakPtr<javascript_dialogs::TabModalDialogView> CreateJavaScriptDialog(
    content::WebContents* parent_web_contents,
    const std::u16string& title,
    content::JavaScriptDialogType dialog_type,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    content::JavaScriptDialogManager::DialogClosedCallback& dialog_callback,
    base::OnceClosure& dialog_force_closed_callback);

// chrome/browser/app_controller_mac.mm 的 tryToTerminateApplication（⌘Q）：有下载在进行
// 时用 Starry 的对话框问要不要退出（Chrome 在那里同步弹一个系统的提示框）。接管了返回
// true，这一次不退出；选了「退出」时取消下载、再退出一次。没有 Starry 的窗口时返回 false，
// 照 Chrome 的做。
bool ConfirmQuitWithDownloads();

// chrome/browser/ui/views/browser_dialogs_views.cc 的 ShowTaskManager、HideTaskManager：
// 任务管理器用 Starry 的窗口（starry/browser/ui/task_manager/task_manager_window.h）。
// 「窗口」菜单、快捷键、没有窗口时的主菜单都经过这里。接管了返回 true。browser 可以是空的。
bool ShowTaskManager(BrowserWindowInterface* browser);
void HideTaskManager();

// chrome/browser/app_controller_mac.mm 的 showPreferencesForProfile（菜单栏的「设置…」、
// 没有窗口在前面时的 ⌘,）：打开 Starry 的设置页。browser 是 Chrome 刚激活的窗口，没有窗口时
// 为空（新开一个）。接管了返回 true。
bool ShowStarrySettings(Profile* profile, BrowserWindowInterface* browser);

// chrome/browser/ui/views/permissions/permission_prompt_factory.cc 的
// CreateNormalPrompt：普通的网站权限请求（Chrome 挂在地址栏上的气泡、小条）用 Starry 的
// 对话框问。不在 Starry 的窗口里时返回空，照 Chrome 的做。安静的提示（被认为在滥用通知的
// 网站）、网页里的权限元素、鼠标键盘锁定还是 Chrome 的。
std::unique_ptr<permissions::PermissionPrompt> CreatePermissionPrompt(
    content::WebContents* web_contents,
    permissions::PermissionPrompt::Delegate* delegate);

// chrome/browser/ui/views/extensions/extension_install_dialog_view.cc 的
// ShowExtensionInstallDialogImpl：安装扩展、批准新权限用 Starry 的对话框问。接管了就拿走
// 三个参数、返回 true；不在 Starry 的窗口里、要填理由的审批返回 false，照 Chrome 的做。
bool ShowExtensionInstallDialog(
    std::unique_ptr<ExtensionInstallPromptShowParams>& show_params,
    extensions::ExtensionInstallPromptClient::DoneCallback& done_callback,
    std::unique_ptr<extensions::InstallPromptData>& prompt);

// chrome/browser/chrome_resource_bundle_helper.cc 的 InitResourceBundleAndDetermineLocale：
// 在 Chrome 定下界面语言之前调用，preferred_locale 是本地状态里的 intl.app_locale（macOS 上
// 总是空的）。Linux 上 Chrome 只看环境变量，这里把设置里选的界面语言放进 LANGUAGE
// （starry/browser/l10n/interface_language.h）。
void WillInitResourceBundle(const std::string& preferred_locale);

// chrome/browser/ui/views/frame/contents_container_view.cc 的 UpdateBorderAndOverlay：
// Starry 的窗口里分屏的格子不要 Chrome 的内边距、描边和角上的小工具条，圆角同
// 内容卡片（starry/browser/ui/split/split_overlay.cc）。接管了返回 true，Chrome 的
// 那一套不做；不是 Starry 的窗口时返回 false。
bool OverrideSplitPaneDecorations(ContentsContainerView* container,
                                  bool is_in_split);

// chrome/browser/extensions/api/downloads/downloads_api.cc：扩展的 chrome.downloads 也看得到
// Starry 自己下载的（starry/browser/downloads/chrome_download_bridge.h）。
// - GetDownload：按编号先找 Starry 的（context 的用户资料，要求时加上它的无痕）。
// - RunDownloadQuery：搜索时加上这两个 DownloadManager 的用户资料里 Starry 的（给了编号
//   时只加那一项）。`incognito_manager` 可以是空的。
// - ShouldExport：交给了 Starry 的 Chrome 下载不再给扩展看（Starry 接手的那一项用同一个
//   编号）。
// - search 的结果：Starry 的一项是不是无痕的（Chrome 的看它在不在无痕的 DownloadManager 里）。
download::DownloadItem* FindDownloadForExtensions(
    content::BrowserContext* context,
    bool include_incognito,
    int id);
void AppendDownloadsForExtensions(
    content::DownloadManager* manager,
    content::DownloadManager* incognito_manager,
    const std::optional<int>& id,
    std::vector<raw_ptr<download::DownloadItem, VectorExperimental>>* items);
bool IsHiddenFromExtensions(const download::DownloadItem& item);
bool IsOffTheRecordDownload(const download::DownloadItem& item);

// chrome/browser/download/download_status_updater.cc 的 GetProgress：程序坞图标（Windows
// 的任务栏）上的下载个数和总进度加上 Starry 自己下载的（进行中的，含暂停、排队，同
// Chrome）。不知道大小的把 `certain` 设成 false。
void AddDownloadProgress(int* count,
                         int64_t* received_bytes,
                         int64_t* total_bytes,
                         bool* certain);

// chrome/browser/download/download_core_service_impl.cc 的 BlockingShutdownCount：关最后一个
// 无痕窗口、退出时要确认的下载加上 Starry 会丢掉的（无痕的没下完的；普通的下次启动
// 接着下，不算）。
int DownloadsLostOnShutdown(Profile* profile);

// chrome/browser/ui/views/frame/layout/browser_view_layout.cc：Starry 的窗口用
// StarryBrowserViewLayout。不归 Starry 排版的窗口返回空，`delegate` 和 `views` 原样留给
// 上游。
std::unique_ptr<BrowserViewLayout> MaybeCreateBrowserViewLayout(
    std::unique_ptr<BrowserViewLayoutDelegate>& delegate,
    BrowserWindowInterface* browser,
    BrowserViewLayoutViews& views);

// 小窗口（其他 App 里点的链接，starry/browser/ui/mini_window/mini_window.h）：
//
// chrome/browser/ui/startup/startup_browser_creator.cc 的 LaunchBrowser：Chrome 要开
// 启动窗口。命令行里是其他 App 打开的一个链接（Windows、Linux，macOS 上已经在运行时）
// 且该用小窗口时开小窗口，冷启动的窗口推迟到要用浏览器窗口时；macOS 从 LaunchServices
// 启动时先记下，等 DidFinishLaunching。返回 true 时这一次不开窗口。
bool InterceptLaunchBrowser(const base::CommandLine& command_line,
                            Profile* profile,
                            const base::FilePath& cur_dir,
                            bool process_startup,
                            bool first_run,
                            bool restore_tabbed_browser);
// chrome/browser/app_controller_mac.mm：
// - applicationDidFinishLaunching：启动完了，tabs 是已经到了的其他 App 打开的链接
//   （可能一个也没有），default_launch 是 AppKit 的 NSApplicationLaunchIsDefaultLaunchKey
//   （从程序坞、访达直接打开；为了打开链接启动时是 false，链接可能这之后才到）。只为了
//   打开一个链接时启动窗口接着推迟或等链接，否则现在打开。
// - OpenStartupTabsInBrowserWithProfile：其他 App 打开的链接。该用小窗口时开小窗口、
//   返回 true。
// - applicationShouldHandleReopen（点程序坞图标）：只有小窗口、启动窗口还推迟着时打开
//   它（或者新开一个），返回 true。
void DidFinishLaunching(const std::vector<StartupTab>& tabs,
                        bool default_launch);
bool OpenExternalLinks(const std::vector<StartupTab>& tabs, Profile* profile);
bool HandleDockReopen();
// chrome/browser/ui/navigator/browser_navigator.cc 的 GetBrowserAndTabForDisposition：
// 小窗口里的网页自己开的窗口（window.open、target=_blank）另开一个小窗口，返回它；
// 别的返回空，照 Chrome 的做。
BrowserWindowInterface* MiniWindowForNavigation(const NavigateParams& params);
// chrome/browser/ui/browser_command_controller.cc 的 IDC_OPEN_FILE（⌘O）：小窗口里是
// 「移到浏览器」，返回 true；别的窗口返回 false，照常打开文件。
bool HandleOpenFileCommand(BrowserWindowInterface* browser);
// chrome/browser/ui/views/frame/browser_native_widget_mac.mm：
// - PopulateCreateWindowParams：小窗口是弹出窗口，但窗口的样子同普通窗口（标题栏透明、
//   内容铺满、不显示标题，红绿灯由 Starry 摆）。
// - GetWindowFrameTitlebarHeight：小窗口的标题栏和它的工具栏一样高（Chrome 按标签栏算，
//   弹出窗口没有标签栏，红绿灯就挤在顶上）。不是小窗口时返回 0，照 Chrome 的算。
bool UsesStarryWindowFrame(BrowserView* browser_view);
int StarryTitlebarHeight(BrowserView* browser_view);
// chrome/browser/ui/views/frame/browser_frame_view_win.cc：Windows 上由 Starry
// 自己画窗口控制按钮（最小化、最大化/还原、关闭），隐藏上游的
// caption_button_container_、去掉非客户区排除区和原生按钮命中测试。
bool UsesStarryWindowControls(BrowserView* browser_view);

}  // namespace starry

#endif  // STARRY_BROWSER_STARRY_HOOKS_H_
