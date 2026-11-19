// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_pip_window.h"

#import <AppKit/AppKit.h>

#include "components/remote_cocoa/common/native_widget_ns_window.mojom.h"
#include "ui/views/widget/native_widget_mac.h"

namespace starry {

namespace {

// 同 Chrome 的
// VideoOverlayWindowNativeWidgetMac：无边框的窗口类，内容铺满，系统给圆角
// 和阴影。
class MediaPipNativeWidgetMac : public views::NativeWidgetMac {
 public:
  explicit MediaPipNativeWidgetMac(
      views::internal::NativeWidgetDelegate* delegate)
      : views::NativeWidgetMac(delegate) {}
  MediaPipNativeWidgetMac(const MediaPipNativeWidgetMac&) = delete;
  MediaPipNativeWidgetMac& operator=(const MediaPipNativeWidgetMac&) = delete;
  ~MediaPipNativeWidgetMac() override = default;

 protected:
  // views::NativeWidgetMac:
  void PopulateCreateWindowParams(
      const views::Widget::InitParams& widget_params,
      remote_cocoa::mojom::CreateWindowParams* params) override {
    params->window_class = remote_cocoa::mojom::WindowClass::kFrameless;
    params->style_mask = NSWindowStyleMaskFullSizeContentView;
  }
};

}  // namespace

views::NativeWidget* CreateMediaPipNativeWidget(
    views::internal::NativeWidgetDelegate* delegate) {
  return new MediaPipNativeWidgetMac(delegate);
}

}  // namespace starry
