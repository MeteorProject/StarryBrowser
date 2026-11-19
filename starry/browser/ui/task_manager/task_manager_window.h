// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_WINDOW_H_
#define STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_WINDOW_H_

#include "build/build_config.h"

namespace views {
class NativeWidget;
class Widget;
namespace internal {
class NativeWidgetDelegate;
}
}  // namespace views

namespace starry {

class TaskManagerView;

// 任务管理器单例窗口：替代 Chrome 默认任务管理器，通过「窗口」菜单、快捷键与标签右键
// 菜单打开（starry_hooks.h 的 ShowTaskManager）。重复打开时激活已有窗口；关闭时同步停止
// 采样并释放。默认 760 × 600，最小 560 × 440，持久化位置与尺寸。
void ShowTaskManagerWindow();
void HideTaskManagerWindow();
// 退出前关掉（StarryBrowserMainExtraParts::PostMainMessageLoopRun）。
void CloseTaskManagerWindowNow();

// 调试用：开着的任务管理器的内容和窗口；没开时为空。
TaskManagerView* GetTaskManagerViewForTesting();
views::Widget* GetTaskManagerWidgetForTesting();

#if BUILDFLAG(IS_MAC)
// macOS：内容铺满窗口（透明标题栏且隐藏系统标题），红绿灯位于左上角，标题和汇总与之
// 同行。Widget 拥有返回的 NativeWidget。
views::NativeWidget* CreateTaskManagerNativeWidget(
    views::internal::NativeWidgetDelegate* delegate);

// 红绿灯那一行：高度（从系统的标题栏量，不同版本的 macOS 不一样）和缩放按钮的右边。
struct TitlebarMetrics {
  int height = 28;
  int buttons_end = 70;
};
TitlebarMetrics GetTitlebarMetrics(views::Widget* widget);
#endif

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_WINDOW_H_
