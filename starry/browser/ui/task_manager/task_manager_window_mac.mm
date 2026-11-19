// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/task_manager/task_manager_window.h"

#import <AppKit/AppKit.h>

#include <cmath>

#include "components/remote_cocoa/common/native_widget_ns_window.mojom.h"
#include "ui/views/widget/native_widget_mac.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 全尺寸内容视图，搭配透明标题栏与隐藏窗口标题。
class TaskManagerNativeWidgetMac : public views::NativeWidgetMac {
 public:
  explicit TaskManagerNativeWidgetMac(
      views::internal::NativeWidgetDelegate* delegate)
      : views::NativeWidgetMac(delegate) {}
  TaskManagerNativeWidgetMac(const TaskManagerNativeWidgetMac&) = delete;
  TaskManagerNativeWidgetMac& operator=(const TaskManagerNativeWidgetMac&) =
      delete;
  ~TaskManagerNativeWidgetMac() override = default;

 protected:
  // views::NativeWidgetMac:
  void PopulateCreateWindowParams(
      const views::Widget::InitParams& widget_params,
      remote_cocoa::mojom::CreateWindowParams* params) override {
    views::NativeWidgetMac::PopulateCreateWindowParams(widget_params, params);
    params->window_class = remote_cocoa::mojom::WindowClass::kDefault;
    params->style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                         NSWindowStyleMaskMiniaturizable |
                         NSWindowStyleMaskResizable |
                         NSWindowStyleMaskFullSizeContentView;
    params->titlebar_appears_transparent = true;
    params->window_title_hidden = true;
    params->animation_enabled = true;
  }
};

}  // namespace

views::NativeWidget* CreateTaskManagerNativeWidget(
    views::internal::NativeWidgetDelegate* delegate) {
  return new TaskManagerNativeWidgetMac(delegate);
}

TitlebarMetrics GetTitlebarMetrics(views::Widget* widget) {
  TitlebarMetrics metrics;
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  if (!window) {
    return metrics;
  }
  const CGFloat height =
      NSHeight(window.frame) - NSHeight(window.contentLayoutRect);
  if (height > 0) {
    metrics.height = static_cast<int>(std::lround(height));
  }
  if (NSButton* zoom = [window standardWindowButton:NSWindowZoomButton]) {
    metrics.buttons_end = static_cast<int>(std::lround(NSMaxX(zoom.frame)));
  }
  return metrics;
}

}  // namespace starry
