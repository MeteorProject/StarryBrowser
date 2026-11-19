// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_ACTIVATION_H_
#define STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_ACTIVATION_H_

namespace views {
class Widget;
}

// macOS：外部链接先激活应用，再交出 URL。激活之前记下普通窗口的层级，
// 用短暂的 Cocoa 屏幕更新事务合并激活和还原，避免主窗口闪到前面。
// 小窗口隐藏前把焦点交还给露出的外部应用；移到浏览器时保持正常激活。
// Windows、Linux 激活单个窗口不会带出其他窗口，这些函数为空。
// 只读取窗口编号、层级和进程号，不读标题或内容，不需要屏幕录制权限。
namespace starry::mini_window {

// 启动后调用一次：开始听应用激活（激活之前记下层级）和用户的操作。
void StartTrackingActivation();
// 小窗口建好了（还没显示）：开始管这次的层级。
void ConfigureWindow(views::Widget* widget);
// 小窗口显示出来了。
void WindowShown(views::Widget* widget);
// 小窗口即将隐藏（macOS 原生 orderOut:）或关闭。moving_to_browser：正移到浏览器。
void WindowClosing(views::Widget* widget, bool moving_to_browser);
// 不再用记下的层级（点程序坞图标、链接进了浏览器窗口）。
void DiscardWindowOrder();

}  // namespace starry::mini_window

#if defined(__OBJC__)
@class NativeWidgetMacNSWindow;
class BrowserView;
namespace remote_cocoa::mojom {
class CreateWindowParams;
}
namespace starry::mini_window {
// 小窗口使用 orderOut: 前移交焦点的原生子类；其他窗口返回 nil，走 Chromium 原路径。
NativeWidgetMacNSWindow* CreateNativeWindow(
    BrowserView* browser_view,
    const remote_cocoa::mojom::CreateWindowParams* params);
}  // namespace starry::mini_window
#endif

#endif  // STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_ACTIVATION_H_
