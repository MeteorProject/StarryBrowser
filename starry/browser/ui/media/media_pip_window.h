// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_PIP_WINDOW_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_PIP_WINDOW_H_

#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "build/build_config.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/widget/widget_delegate.h"

namespace views {
class NativeWidget;
class Widget;
namespace internal {
class NativeWidgetDelegate;
}
}  // namespace views

namespace starry {

class MediaPip;
class MediaVideoView;

// 桌面的画中画小窗（同 Chrome
// 的画中画：浮在所有窗口上面、每个桌面都显示）。没有标题栏，
// 整块是视频画面，macOS
// 上是系统窗口的圆角和阴影。按住画面拖动窗口，拖边缘按视频的比例
// 缩放。控件同岛上的视频（MediaVideoView），右上是「放回窗口」。
//
// 第一次出现在屏幕右下角，宽是工作区的五分之一；挪过、缩放过的位置和大小这次运行里记着。
// 从岛上弹出来时从岛上画面的位置飞过去（kMediaPipFlight）。
//
// 它是窗口的 WidgetDelegate，自己拿着窗口（CLIENT_OWNS_WIDGET）。
class MediaPipWindow : public views::WidgetDelegate {
 public:
  explicit MediaPipWindow(base::WeakPtr<MediaPip> pip);
  MediaPipWindow(const MediaPipWindow&) = delete;
  MediaPipWindow& operator=(const MediaPipWindow&) = delete;
  ~MediaPipWindow() override;

  MediaVideoView* video() const { return video_; }
  views::Widget* widget() const;
  // from：画面现在在屏幕上的位置（岛上弹出来时）；没有时直接出现。
  void ShowWindow(const std::optional<gfx::Rect>& from);
  void Hide();
  bool IsVisible() const;
  bool IsActive() const;
  gfx::Rect GetBoundsInScreen() const;
  void UpdateNaturalSize();

  // views::WidgetDelegate:
  std::unique_ptr<views::FrameView> CreateFrameView(
      views::Widget* widget) override;

 private:
  class PipWidget;
  friend class PipWidget;

  gfx::Rect WorkArea() const;
  gfx::Size MinimumSize() const;
  gfx::Size MaximumSize() const;
  // 按视频的比例、记着的大小和位置算出来的窗口位置。
  gfx::Rect TargetBounds() const;
  void OnFlightStep();
  void OnBoundsChanged();
  void OnNativeClosed();

  base::WeakPtr<MediaPip> pip_;
  std::unique_ptr<PipWidget> widget_;
  raw_ptr<MediaVideoView> video_ = nullptr;
  ValueMotion flight_;
  gfx::Rect flight_from_;
  gfx::Rect flight_to_;
  bool flying_ = false;
  bool closing_ = false;
};

#if BUILDFLAG(IS_MAC)
// macOS：没有标题栏，但保留系统窗口的圆角和阴影（同 Chrome 的
// VideoOverlayWindowNativeWidgetMac）。Widget 拥有返回的 NativeWidget。
views::NativeWidget* CreateMediaPipNativeWidget(
    views::internal::NativeWidgetDelegate* delegate);
#endif

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_PIP_WINDOW_H_
