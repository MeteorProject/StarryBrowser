// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_PIP_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_PIP_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "components/viz/common/surfaces/surface_id.h"
#include "content/public/browser/overlay_window.h"
#include "services/media_session/public/cpp/media_position.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace views {
class Widget;
}

namespace content {
class VideoPictureInPictureWindowController;
class WebContents;
}  // namespace content

namespace starry {

class MediaHub;
class MediaPipWindow;
class MediaVideoView;
struct MediaEntry;

// 视频的画中画（代替 Chrome 的 VideoOverlayWindowViews：
// StarryContentBrowserClient::CreateWindowForVideoPictureInPicture 建它，Chrome
// 的画中画 控制器拿着它，一个标签一个）。整个浏览器同一时间只有一个视频画中画
// （PictureInPictureWindowManager）。两种放法，可以来回换：
//
// - 窗内：放在那个窗口的媒体岛上（Host）。视频在放的标签切到后台时，岛先登记
//   （RequestInline），再让网页进画中画；切回那个标签时岛把画面飞回网页里原来的位置，
//   落地后才关掉画中画（ReturnToPage）。浏览器自己的画中画按钮（播放器里的，
//   EnterFromBrowserUi）也先放到岛上。
// - 桌面：一个浮在所有窗口上面的小窗（MediaPipWindow）。网页自己点画中画、在岛上点
//   「弹到桌面」、把画面拖出窗口，或者浏览器的按钮点的是当前标签里的视频时。
//
// 画面是视频播放器给画中画的那个 viz surface（MediaVideoView
// 显示），不另外解码。
class MediaPip : public content::VideoOverlayWindow {
 public:
  // 窗内的放法：一个窗口的媒体岛。
  class Host {
   public:
    // 这个标签在这个窗口里、能放到岛上（不是当前标签、岛没收起）。
    virtual bool CanHostVideo(content::WebContents* contents) const = 0;
    // 放上来、拿走（关掉了、弹到桌面了）。拿走时不要再调 pip。
    virtual void AttachVideo(MediaPip* pip) = 0;
    virtual void DetachVideo(MediaPip* pip) = 0;
    // 状态（播放、进度、画面比例）变了。
    virtual void VideoChanged(MediaPip* pip) = 0;
    // 用户点关闭关掉了这个标签的画中画（窗内、桌面都算）：它再放起来以前不要在岛上
    // 冒出来。
    virtual void VideoClosedByUser(content::WebContents* contents) = 0;
    // 桌面小窗出现、挪动、缩放、没了：要不要让开。
    virtual void DesktopVideoMoved() = 0;

   protected:
    virtual ~Host() = default;
  };

  static std::unique_ptr<content::VideoOverlayWindow> Create(
      content::VideoPictureInPictureWindowController* controller);
  // 窗口的媒体岛在、不在了。不在时它上面的画中画换到桌面。
  static void AddHost(Host* host);
  static void RemoveHost(Host* host);
  // 这个标签下一次进画中画时放到 host 上（两秒内有效）。automatic：切走标签时自动
  // 进的（设置关掉自动画中画时要结束）。
  static void RequestInline(content::WebContents* contents,
                            Host* host,
                            bool automatic);
  // 浏览器界面上的画中画按钮：能放到那个窗口的岛上就放（窗内），不能（当前标签、岛
  // 不能接）时是桌面小窗。
  static void EnterFromBrowserUi(content::WebContents* contents);
  // 这个标签现在进画中画会放到岛上。
  static bool CanShowInline(content::WebContents* contents);
  // 这个标签现在显示着的画中画；没有时为空。
  static MediaPip* ForWebContents(content::WebContents* contents);
  // 现在显示着的画中画（整个浏览器最多一个）；没有时为空。
  static MediaPip* Current();
  // 设置允许自动画中画（手动进入不受这个开关影响）。
  static bool AutomaticEnabled(content::WebContents* contents);

  MediaPip(const MediaPip&) = delete;
  MediaPip& operator=(const MediaPip&) = delete;
  ~MediaPip() override;

  content::WebContents* web_contents() const;
  // 这个标签的媒体（标题、封面、静音）；没有时为空。
  const MediaEntry* entry() const;
  MediaHub* hub() const;
  bool is_inline() const { return host_ != nullptr; }
  bool automatically_entered() const { return automatically_entered_; }
  Host* host() const { return host_; }
  bool shown() const { return shown_; }
  const viz::SurfaceId& surface_id() const { return surface_id_; }
  const gfx::Size& natural_size() const { return natural_size_; }
  bool playing() const { return playback_state_ == kPlaying; }
  const std::optional<media_session::MediaPosition>& position() const {
    return position_;
  }
  bool can_play_pause() const { return show_play_pause_; }
  bool can_next_track() const { return show_next_track_; }
  bool can_previous_track() const { return show_previous_track_; }
  // 视频进画中画以前在网页里的位置（相对标签内容区的 DIP）；不知道时为空。
  gfx::Rect SourceBounds() const;
  base::WeakPtr<MediaPip> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // 画面上的控件。
  void TogglePlayPause();
  void SeekBy(base::TimeDelta delta);
  void SeekToFraction(double fraction);
  void NextTrack();
  void PreviousTrack();
  void ToggleMute();
  // 关掉画中画、切到那个标签（画面回到网页里）。
  void BackToTab();
  // 关闭按钮：关掉画中画，能暂停时暂停（同 Chrome 的画中画）。
  void CloseByUser();
  // 岛上飞回网页落地了：关掉画中画（不暂停）。
  void ReturnToPage();
  // 窗内 → 桌面。from：画面现在在屏幕上的位置（小窗从那里出现）。
  void PopOut(const std::optional<gfx::Rect>& from = std::nullopt);
  // 桌面 → 窗内：放回标签所在窗口的岛上；那个标签就是当前标签时直接回到网页。
  void MoveInline();
  // 在桌面上、能放回窗口里（那个窗口的岛能接）。
  bool CanMoveInline() const;

  // 显示画面的视图换了、动了（MediaVideoView 告诉）。
  void SetView(MediaVideoView* view);
  void ViewBoundsChanged();
  // 桌面小窗被系统关掉了。
  void OnDesktopWindowClosed();
  // 桌面小窗出现、挪动、缩放、藏起来了（各个窗口的岛让开）。
  void DesktopBoundsChanged();
  // 调试：放在哪里、多大、在不在放。
  std::string DebugState();
  // 桌面小窗（岛的让位、调试）；不在桌面上时为空。
  views::Widget* desktop_widget() const;
  // 调试：当作鼠标移到了画面上（控件出现）、移走了。
  void SetHoveredForTesting(bool hovered);

  // content::VideoOverlayWindow:
  bool IsActive() const override;
  void Close() override;
  void ShowInactive() override;
  void Hide() override;
  bool IsVisible() const override;
  gfx::Rect GetBounds() override;
  void UpdateNaturalSize(const gfx::Size& natural_size) override;
  void SetPlaybackState(PlaybackState playback_state) override;
  void SetPlayPauseButtonVisibility(bool is_visible) override;
  void SetSkipAdButtonVisibility(bool is_visible) override;
  void SetNextTrackButtonVisibility(bool is_visible) override;
  void SetPreviousTrackButtonVisibility(bool is_visible) override;
  void SetHidePictureInPictureButtonVisibility(bool is_visible) override;
  void SetMicrophoneMuted(bool muted) override;
  void SetCameraState(bool turned_on) override;
  void SetMediaMuted(bool muted) override;
  void SetToggleMicrophoneButtonVisibility(bool is_visible) override;
  void SetToggleCameraButtonVisibility(bool is_visible) override;
  void SetHangUpButtonVisibility(bool is_visible) override;
  void SetNextSlideButtonVisibility(bool is_visible) override;
  void SetPreviousSlideButtonVisibility(bool is_visible) override;
  void SetMediaPosition(const media_session::MediaPosition& position) override;
  void SetSourceTitle(const std::u16string& source_title) override;
  void SetFaviconImages(
      const std::vector<media_session::MediaImage>& images) override;
  void SetSurfaceId(const viz::SurfaceId& surface_id) override;
  void SetPlaybackControlsVisibility(bool is_visible) override;
  void SetImmersiveVideoOptions(
      const content::ImmersiveOptions& options) override;

 private:
  explicit MediaPip(content::VideoPictureInPictureWindowController* controller);

  void ShowDesktop(const std::optional<gfx::Rect>& from);
  void HideDesktop();
  void Changed();

  const raw_ptr<content::VideoPictureInPictureWindowController> controller_;
  raw_ptr<Host> host_ = nullptr;
  std::unique_ptr<MediaPipWindow> desktop_;
  raw_ptr<MediaVideoView> view_ = nullptr;
  bool shown_ = false;
  bool automatically_entered_ = false;
  viz::SurfaceId surface_id_;
  gfx::Size natural_size_;
  PlaybackState playback_state_ = kPaused;
  bool show_play_pause_ = true;
  bool show_next_track_ = false;
  bool show_previous_track_ = false;
  std::optional<media_session::MediaPosition> position_;
  gfx::Size last_size_;
  base::WeakPtrFactory<MediaPip> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_PIP_H_
