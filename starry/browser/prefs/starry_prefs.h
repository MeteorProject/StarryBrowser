// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_PREFS_STARRY_PREFS_H_
#define STARRY_BROWSER_PREFS_STARRY_PREFS_H_

#include <string_view>

class PrefService;
class Profile;

// Starry 自己的设置项，存在 Chrome 的配置（Preferences）里，和 Chrome 的设置一样按
// 用户资料分开。无痕窗口读写普通用户资料的设置（整个应用共享一份设置）。
//
// 注册不用改上游：StarryPrefsFactory 是一个不建服务的 KeyedServiceFactory，Chrome
// 建用户资料时会让所有的工厂注册自己的设置项（见 starry_prefs.cc）。
namespace starry::prefs {

// 网页时区：IANA 名字，空字符串是跟随系统；重新启动后生效。
inline constexpr char kWebTimeZone[] = "starry.web_time_zone";

// 指纹保护：off / privateBrowsing（默认）/ allBrowsing。例外按主站点保存，包含
// 子域名；设置变化时，现有文档和 Worker 的下一次读出使用新策略。
inline constexpr char kFingerprintProtection[] =
    "starry.privacy.fingerprint_protection";
inline constexpr char kFingerprintProtectionExceptions[] =
    "starry.privacy.fingerprint_exceptions";

// 位置模拟（starry/browser/privacy/location_simulation.h）：off（默认）/
// privateBrowsing / allBrowsing；全局位置 {latitude, longitude, accuracy, timeZone,
// label}；网站单独设置按主站点：{mode: "real"} 或 {mode: "simulated", 位置…}。
inline constexpr char kLocationSimulation[] =
    "starry.privacy.location_simulation";
inline constexpr char kSimulatedLocation[] =
    "starry.privacy.simulated_location";
inline constexpr char kSimulatedLocationSites[] =
    "starry.privacy.simulated_location_sites";

// 其他应用里点击的链接在哪里打开（设置 › 通用 › 外部链接）：
// "miniWindow"（默认，小窗口）或 "browserWindow"（当前浏览器窗口的新标签页）。见
// starry/browser/ui/mini_window/mini_window.h。
inline constexpr char kExternalLinks[] = "starry.external_links";
inline constexpr char kExternalLinksMiniWindow[] = "miniWindow";
inline constexpr char kExternalLinksBrowserWindow[] = "browserWindow";
// 小窗口的大小（DIP，用户拖动窗口后记下），0 是默认大小。
inline constexpr char kMiniWindowWidth[] = "starry.mini_window.width";
inline constexpr char kMiniWindowHeight[] = "starry.mini_window.height";

// 标签布局："docked"（贴边的固定侧栏，默认）、"floating"（自动隐藏的浮动
// 玻璃侧栏，鼠标移到窗口左边缘时滑出）或 "top"（顶部标签，下面一行是工具栏）。
inline constexpr char kPinnedPeek[] = "starry.tabs.pinned_peek";
inline constexpr char kFavorites[] = "starry.tabs.favorites";
inline constexpr char kPinsCollapsed[] = "starry.tabs.pins_collapsed";
inline constexpr char kPinFolders[] = "starry.tabs.pin_folders";
// 常用图块里的文件夹（所有窗口共用）：ID → {name, order, color}。常用标签
// （kFavorites）的 "folder" 是它在哪个文件夹里。
inline constexpr char kFavoriteFolders[] = "starry.tabs.favorite_folders";
inline constexpr char kSidebarLayout[] = "starry.sidebar.layout";
inline constexpr char kSidebarLayoutDocked[] = "docked";
inline constexpr char kSidebarLayoutFloating[] = "floating";
inline constexpr char kSidebarLayoutTop[] = "top";
// 侧栏布局下把地址栏、扩展按钮移到内容卡片上方的工具栏（顶部标签布局总在工具栏里）。
inline constexpr char kAddressBarOnTop[] = "starry.toolbar.address_on_top";
inline constexpr char kExtensionsOnTop[] = "starry.toolbar.extensions_on_top";
// 紧凑地址栏：地址栏在顶部时不单独占一行，并进内容卡片顶部的一条细栏。
inline constexpr char kCompactAddressBar[] = "starry.toolbar.compact";
// 窗口、侧栏和紧凑地址栏跟随网页的主题色（默认开）。
inline constexpr char kTintFromWebsite[] =
    "starry.appearance.tint_from_website";
// Windows 窗口控制按钮样式（只在 Windows 上显示于「设置 › 外观」）：
// "glass"（默认：侧栏模式下内容卡片顶至 8px，右上角悬浮液态玻璃胶囊）或
// "rail"（侧栏模式下保留 30px 纤细顶檐与软角按钮，不遮挡网页右上角）。
inline constexpr char kWindowsControlsStyle[] =
    "starry.appearance.windows_controls_style";
inline constexpr char kWindowsControlsStyleGlass[] = "glass";
inline constexpr char kWindowsControlsStyleRail[] = "rail";
// 悬浮玻璃胶囊样式的自动隐藏开关：默认关；开启后纯侧栏模式仅在右上角边框保留微弱视觉
// 提示，鼠标移入右上角边缘再弹出液态玻璃胶囊。
inline constexpr char kWindowsControlsAutoHide[] =
    "starry.appearance.windows_controls_auto_hide";
// 兼容已迁移的独立「自动隐藏侧栏」配置键（现已并入 kSidebarLayoutFloating）。
inline constexpr char kSidebarAutoHide[] = "starry.sidebar.auto_hide";
// 侧栏宽度（DIP），0 是默认宽度。界面负责限制在允许的范围里。
inline constexpr char kSidebarWidth[] = "starry.sidebar.width";
// 底部工具按钮里藏起来的（"downloads"、"history"、"bookmarks"、"extensions"、
// "layout"、"settings"）。
inline constexpr char kHiddenUtilityButtons[] = "starry.sidebar.hidden_buttons";
// 恢复上次的窗口和标签时，没有显示出来的标签先休眠，切过去才载入（默认开）。
// 关掉时照 Chrome 的做：在后台逐个载入。见 starry/browser/sessions/restored_tabs.cc。
inline constexpr char kSleepRestoredTabs[] = "starry.tabs.sleep_restored";
// 悬停标签时显示预览卡片（默认开），卡片上显示内存和 CPU 占用（默认开）。
inline constexpr char kTabPreviewsEnabled[] = "starry.tab_previews.enabled";
inline constexpr char kTabPreviewShowsUsage[] = "starry.tab_previews.show_usage";
// 树状标签页（starry/browser/ui/tabs/tab_tree.h，只在侧栏布局下显示）：总开关（默认开）、
// 新标签最多挂到第几层（默认 4，0 是不限）、关掉展开着的父标签时怎么办（0 智能、1 子标签
// 上移一级、2 连同子标签一起关，默认 0）、切换标签时折起其他组（默认关）。
inline constexpr char kTreeTabs[] = "starry.tabs.tree";
inline constexpr char kTreeTabsMaxDepth[] = "starry.tabs.tree_max_depth";
inline constexpr char kTreeTabsCloseParent[] = "starry.tabs.tree_close_parent";
inline constexpr char kTreeTabsAutoCollapse[] =
    "starry.tabs.tree_auto_collapse";
// 标签管理：自动归档闲置的普通标签（默认开）、闲置多少小时后
// 归档（默认 1 周）、归档重复的标签（默认开）、不整理的网站（网站的列表，含子域名）、
// 归档留多少天（0 是一直留着，默认 90 天）。
inline constexpr char kAutoArchive[] = "starry.tabs.auto_archive";
inline constexpr char kAutoArchiveHours[] = "starry.tabs.auto_archive_hours";
inline constexpr char kArchiveDuplicates[] = "starry.tabs.archive_duplicates";
inline constexpr char kOrganizerExceptions[] = "starry.tabs.organizer_exceptions";
inline constexpr char kArchiveRetentionDays[] =
    "starry.tabs.archive_retention_days";
// AI 整理用哪个模型：后端（空的是和 AI 助手相同，否则是 assistant_settings.h 的
// kBackend*）、OpenAI 兼容服务的服务和模型（Key 用 AI 助手存的那份）。
inline constexpr char kOrganizerBackend[] = "starry.tabs.organizer_backend";
inline constexpr char kOrganizerProvider[] = "starry.tabs.organizer_provider";
inline constexpr char kOrganizerModel[] = "starry.tabs.organizer_model";
// 标签较多时在「新标签页」行提示「✦ 整理」（默认开）、归档时用 AI 整理的
// 模型给标签打主题（默认开，未配置模型时不执行）。
inline constexpr char kOrganizeHint[] = "starry.tabs.organize_hint";
inline constexpr char kArchiveTopics[] = "starry.tabs.archive_topics";
// 新打开的网页符合按网站的规则时提示放进文件夹（默认开）；记住的整理习惯
// （规则、放过的次数、忽略记录、命名偏好，tab_organizer/organizer_memory.h）。
inline constexpr char kOrganizeSuggest[] = "starry.tabs.organize_suggest";
inline constexpr char kOrganizerMemory[] = "starry.tabs.organizer_memory";
// 媒体播放器（媒体岛、侧栏和标签栏里的控制）的总开关，默认开。
inline constexpr char kMediaControlsEnabled[] = "starry.media.controls_enabled";
// 收起到侧栏底部（顶部标签布局时是标签行），不再浮在网页上；默认浮着。
inline constexpr char kMediaDocked[] = "starry.media.docked";
// 切走正在播放视频的标签页时自动进画中画；标签里的动态音量条。默认都开。
inline constexpr char kMediaAutoPip[] = "starry.media.auto_pip";
inline constexpr char kMediaTabMeters[] = "starry.media.tab_meters";
// 媒体岛停在内容卡片的哪个角：右边 1、下边 2 两位（默认
// 3，右下角）。拖过去松手时改，所有窗口一起变。
inline constexpr char kMediaCorner[] = "starry.media.corner";
// 内置助手的总开关（默认开）：关掉时没有 ✦ 按钮、
// ⌘E 不管用，开着的对话存好并结束。外部智能体另有开关（kAgentAllowExternal）。
inline constexpr char kAssistantEnabled[] = "starry.assistant.enabled";
// 内置助手（starry/browser/assistant/assistant_settings.h）：服务的 ID、每家选的模型、
// 加密过的 Key、自定义服务的地址、确认规则（"ask" / "agent"）。
inline constexpr char kAssistantProvider[] = "starry.assistant.provider";
inline constexpr char kAssistantModels[] = "starry.assistant.models";
inline constexpr char kAssistantKeys[] = "starry.assistant.keys";
inline constexpr char kAssistantCustomUrl[] = "starry.assistant.custom_url";
inline constexpr char kAssistantMode[] = "starry.assistant.mode";
// 用什么跑助手："chat"（OpenAI 兼容服务，默认）、"claudeCode"、"codex"（本机的智能体）。
// 本机智能体的可执行文件和模型按后端记（空着是自动找、用它自己的默认）。
inline constexpr char kAssistantBackend[] = "starry.assistant.backend";
inline constexpr char kAssistantLocalPaths[] = "starry.assistant.local_paths";
inline constexpr char kAssistantLocalModels[] = "starry.assistant.local_models";
// 思考程度：后端的键（chat:<服务>、claudeCode、codex）→ 等级，没有就用默认。
inline constexpr char kAssistantReasoning[] = "starry.assistant.reasoning";
// 助手的样子："float"（网页下方的岛）、"dock"（右边的侧栏）。
inline constexpr char kAssistantPanelStyle[] = "starry.assistant.panel_style";
// 允许外部 AI 智能体（MCP 客户端）经 `--mcp` 控制浏览器，默认关。
inline constexpr char kAgentAllowExternal[] = "starry.agent.allow_external";
// 允许智能体执行 JavaScript（browser_evaluate），默认关。
inline constexpr char kAgentAllowScripts[] = "starry.agent.allow_scripts";
// 允许智能体替网页选本机的文件上传（browser_file_upload），默认开：客户端调用前会请
// 用户确认，内置助手总要先问。
inline constexpr char kAgentAllowUploads[] = "starry.agent.allow_uploads";
// 允许智能体打开临时无痕标签（browser_tab_new 的 private），默认开：搜索、查资料不留
// 历史和网站数据。关掉时已经开着的全部关掉。
inline constexpr char kAgentAllowPrivateTabs[] =
    "starry.agent.allow_private_tabs";

// 新标签页（starry/browser/new_tab/new_tab.h）：风格（minimal / card / overlay）、
// 背景（theme / sky / image / solid）、启用的小组件顺序、快捷网站行数（1 或 2）与便签。
inline constexpr char kNewTabStyle[] = "starry.new_tab.style";
inline constexpr char kNewTabStyleMinimal[] = "minimal";
inline constexpr char kNewTabStyleCard[] = "card";
inline constexpr char kNewTabStyleOverlay[] = "overlay";
inline constexpr char kNewTabBackground[] = "starry.new_tab.background";
inline constexpr char kNewTabWidgets[] = "starry.new_tab.widgets";
inline constexpr char kNewTabShortcutRows[] = "starry.new_tab.shortcut_rows";
inline constexpr char kNewTabNote[] = "starry.new_tab.note";
// 每日拦截计数（"YYYY-MM-DD" → 次数），保留最近 14 天；无痕窗口不记录。
inline constexpr char kBlockedStats[] = "starry.content_blocking.daily_blocked";

// 设置向导（starry/browser/onboarding/onboarding.h）：是否待完成及已完成的版本号（0 表示未完成）。
inline constexpr char kOnboardingPending[] = "starry.onboarding.pending";
inline constexpr char kOnboardingVersion[] = "starry.onboarding.version";

// Starry 分段下载设置（见 starry/browser/downloads/download_service.h）：
// 多线程下载（默认开）、每个文件的连接数（2、4、
// 8、16，默认 8）、同时下载的文件数（1、2、3、5、10，默认 3）、限速（KB/s，0 是不限速，
// 所有下载合计）、连接中断时自动重试（默认开）、启动时继续上次没下完的（默认开）、
// 浏览器在后台时下载完成发系统通知（默认开，Chrome 下载的也算）。
inline constexpr char kDownloadMultiConnection[] =
    "starry.downloads.multi_connection";
inline constexpr char kDownloadConnections[] = "starry.downloads.connections";
inline constexpr char kDownloadMaxConcurrent[] =
    "starry.downloads.max_concurrent";
inline constexpr char kDownloadSpeedLimit[] = "starry.downloads.speed_limit";
inline constexpr char kDownloadAutoRetry[] = "starry.downloads.auto_retry";
inline constexpr char kDownloadResumeOnLaunch[] =
    "starry.downloads.resume_on_launch";
inline constexpr char kDownloadNotifyOnCompletion[] =
    "starry.downloads.notify_on_completion";

// 在 Chrome 建第一个用户资料之前调用一次（StarryBrowserMainExtraParts::PreProfileInit）。
void EnsureRegistered();

// Starry 的设置读写这一份：无痕的用户资料换成原来的。
PrefService* PrefsFor(Profile* profile);

}  // namespace starry::prefs

#endif  // STARRY_BROWSER_PREFS_STARRY_PREFS_H_
