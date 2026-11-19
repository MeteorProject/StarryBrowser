// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_H_
#define STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_H_

#include <vector>

#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

class BrowserWindowInterface;
class GURL;
class Profile;

namespace base {
class CommandLine;
class FilePath;
}  // namespace base

// 小窗口（设置 › 通用 › 外部链接，默认开启）：其他应用点开的单个链接先展示在这里，
// 仅包含一个网页和一条精简工具栏（红绿灯、导航按钮、地址、「移到浏览器」），看完可直接
// 关闭；需要保留时按 ⌘O 或点击「移到浏览器」，将同一 WebContents 原样（滚动位置、登录
// 状态、播放进度均保留）迁入主浏览器窗口。
//
// 小窗口是 Chrome 的弹出窗口（TYPE_POPUP）：Chrome 本来就把它当成只有一个标签的窗口，⌘ 点
// 链接、右键「在新标签页打开」都去浏览器窗口，扩展看到的是 popup，不往里放标签。建的时候
// 带上 omit_from_session_restore，不进会话。界面是 StarryBrowserView 的小窗口样子
// （MiniWindowToolbar）；网页弹出的窗口（window.open、target=_blank）另开一个小窗口
// （starry::MiniWindowForNavigation）。
//
// 冷启动：应用是被一个链接拉起来的时候只开小窗口，不恢复上次的窗口；第一次要用浏览器窗口
// 时（移到浏览器、⌘T、点程序坞图标）再按「启动时」的设置打开（DeferLaunch）。
namespace starry::mini_window {

// 默认尺寸与最小尺寸。
inline constexpr int kDefaultWidth = 880;
inline constexpr int kDefaultHeight = 640;
inline constexpr int kMinWidth = 420;
inline constexpr int kMinHeight = 400;
// 窗口比这个窄时「移到浏览器」只留图标。
inline constexpr int kCompactWidth = 600;
// 命令栏离窗口顶边（窗口矮，比普通窗口靠上）。
inline constexpr int kCommandBarTopOffset = 52;
// 同一个位置已经有小窗口时往右下错开这么多，最多错开这么多次。
inline constexpr int kCascadeStep = 28;
inline constexpr int kMaxCascades = 12;

// 判断外部应用传入的链接是否应以小窗口打开：偏好设为小窗口且为单个 http(s) 链接。
// 本地文件、多链接或强制无痕模式均在主浏览器窗口打开。
bool OpensInMiniWindow(Profile* profile, const std::vector<GURL>& urls);

// 记下的大小（没有或比最小还小时用默认、最小）。
gfx::Size WindowSize(int saved_width, int saved_height);
// 放在 work_area（鼠标所在屏幕的可用区域）里水平居中、略偏上，放不下时缩到可用区域那么
// 大；occupied 是别的小窗口，左上角重合时往右下错开（网页弹出来的窗口要看得出是新的）。
gfx::Rect WindowBounds(const gfx::Size& size,
                       const gfx::Rect& work_area,
                       const std::vector<gfx::Rect>& occupied);

// 开一个空的小窗口（还没显示，放进第一个标签时显示）。不能建窗口时为空。
BrowserWindowInterface* Create(Profile* profile, bool user_gesture);
// 在新的小窗口里打开网址并显示。
BrowserWindowInterface* Open(Profile* profile, const GURL& url);

// 是小窗口（StarryBrowserView 记着）。
bool IsMiniWindow(BrowserWindowInterface* browser);
// StarryBrowserView 建的时候问：这一个是不是 Create 正在建的小窗口。
bool IsCreating();

// 把小窗口的网页移到浏览器窗口（当前标签后面、选中），小窗口跟着关掉。没有浏览器窗口时
// 先打开推迟的启动窗口（恢复上次的标签），等它恢复完再移过去。
void MoveToBrowser(BrowserWindowInterface* mini);

// 小窗口里要用浏览器窗口的命令（⌘T、设置、下载列表）去这里：最近用的普通窗口；没有时
// 打开推迟的启动窗口，再没有就开一个新窗口（带一个新标签页）。
BrowserWindowInterface* BrowserWindowFor(Profile* profile);

// ---- 启动 ----

// 启动时 Chrome 要开窗口（StartupBrowserCreator::LaunchBrowser）。返回 true 时这一次不开：
// - 命令行里是其他 App 打开的链接（Windows、Linux 的做法，macOS 上已经在运行时）、该用
//   小窗口：开小窗口。冷启动时把这次启动记下，推迟到要用浏览器窗口时。
// - macOS 从程序坞、访达、其他 App 启动（链接经 Apple 事件进来，这时还没到）：先记下，
//   等 DidFinishLaunching 看有没有链接再定。
bool InterceptLaunchBrowser(const base::CommandLine& command_line,
                            Profile* profile,
                            const base::FilePath& cur_dir,
                            bool process_startup,
                            bool first_run,
                            bool restore_tabbed_browser);
// macOS：启动完了（applicationDidFinishLaunching）。urls 是这之前到了的链接；为了打开
// 链接启动时（default_launch 为 false）链接也可能在这之后才到（LaunchServices 先启动、再发
// Apple 事件），这时接着等 OpenExternalLinks。要开小窗口时启动窗口接着推迟，否则现在打开。
void DidFinishLaunching(const std::vector<GURL>& urls, bool default_launch);
// 其他 App 打开的链接（macOS 的 Apple 事件）：该用小窗口时开小窗口、返回 true。还在等
// 启动时的链接、又不用小窗口时，先把启动窗口打开（Chrome 再把链接放进去）。
bool OpenExternalLinks(Profile* profile, const std::vector<GURL>& urls);
// 推迟着的启动窗口现在打开（第一次要用浏览器窗口）。没有推迟的返回 false。
bool ResumeDeferredLaunch();
// 点程序坞图标（macOS）：没有浏览器窗口时（只有小窗口也算）打开推迟的启动窗口或者新开
// 一个。处理了返回 true；有浏览器窗口、也没有小窗口时照 Chrome 的做。
bool HandleReopen();

}  // namespace starry::mini_window

#endif  // STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_H_
