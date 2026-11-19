// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_VIDEO_VIEW_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_VIDEO_VIEW_H_

#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/common/surfaces/surface_id.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_observer.h"
#include "ui/views/view.h"

namespace starry {

class MediaPip;

// 画中画的画面：视频播放器给画中画的那个 viz
// surface（不另外解码），上面盖一层控件。
// 窗内（媒体岛上的卡片）和桌面的小窗共用。圆角由外面裁（岛上的卡片自己的图层、桌面
// 小窗是系统窗口的圆角）。
//
// 控件在鼠标悬停或视频暂停时淡入：
// -
// 左上「回到标签页」（网站图标和字）；右上窗内时是「弹到桌面」、桌面时是「放回窗口」，
//   再右边关闭。
// - 中间后退 10 秒、播放暂停、前进 10
// 秒；网页有上一个、下一个时两边再各加一个。
// - 下面实时频谱、进度条、时间，右边静音。画面矮时只留进度条和时间。
class MediaVideoView : public views::View, public ui::CompositorObserver {
  METADATA_HEADER(MediaVideoView, views::View)

 public:
  enum class Place { kInline, kDesktop };

  // pip 没了以后画面停在最后一帧，控件不再动（岛上淡出时）。
  MediaVideoView(base::WeakPtr<MediaPip> pip, Place place);
  MediaVideoView(const MediaVideoView&) = delete;
  MediaVideoView& operator=(const MediaVideoView&) = delete;
  ~MediaVideoView() override;

  // 画中画的状态（播放、进度、可用的按钮、画面）变了。
  void Update();
  void SetHovered(bool hovered);
  bool hovered() const { return hovered_; }
  // 这一点（本视图的坐标）在看得见的控件上：桌面小窗那里是按钮，别处是拖动窗口。
  bool HitsControl(const gfx::Point& point) const;
  // 不再显示画面（画中画关了、换到别处显示了）：注销 surface。
  void ClearSurface();

  // views::View:
  void Layout(PassKey) override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;

  // ui::CompositorObserver:
  void OnCompositingShuttingDown(ui::Compositor* compositor) override;

 private:
  class Controls;

  // 画面在本视图里的位置：按视频的比例放进来，居中。
  gfx::Rect VideoBounds() const;
  void ShowSurface();
  void RegisterFrameSink();
  void UnregisterFrameSink();
  void UpdateControlsShown();
  void UpdateTicker();

  const base::WeakPtr<MediaPip> pip_;
  raw_ptr<views::View> video_ = nullptr;
  raw_ptr<Controls> controls_ = nullptr;
  bool hovered_ = false;
  bool controls_shown_ = false;
  LayerMotion controls_fade_;
  viz::SurfaceId surface_id_;
  // 最后知道的视频大小（pip 没了以后用）。
  gfx::Size natural_;
  // 视频的 frame sink 挂在哪个合成器下面（窗口换了要重新挂）。
  raw_ptr<ui::Compositor> compositor_ = nullptr;
  viz::FrameSinkId registered_;
  base::ScopedObservation<ui::Compositor, ui::CompositorObserver>
      compositor_observation_{this};
  // 在放、控件看得见时每 0.25 秒更新时间和进度。
  base::RepeatingTimer ticker_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_VIDEO_VIEW_H_
