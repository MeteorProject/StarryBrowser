// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_pip.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/picture_in_picture/picture_in_picture_window_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/video_picture_in_picture_window_controller.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/media/media_pip_window.h"
#include "starry/browser/ui/media/media_video_view.h"

namespace starry {

namespace {

// 岛登记的「下一次进画中画放到我这里」，过这么久没进就作废（网页可能不让进）。
constexpr base::TimeDelta kRequestLifetime = base::Seconds(2);

struct InlineRequest {
  base::WeakPtr<content::WebContents> contents;
  raw_ptr<MediaPip::Host> host;
  base::TimeTicks time;
  bool automatic = false;
};

std::vector<raw_ptr<MediaPip>>& Pips() {
  static base::NoDestructor<std::vector<raw_ptr<MediaPip>>> pips;
  return *pips;
}

std::vector<raw_ptr<MediaPip::Host>>& Hosts() {
  static base::NoDestructor<std::vector<raw_ptr<MediaPip::Host>>> hosts;
  return *hosts;
}

std::vector<InlineRequest>& Requests() {
  static base::NoDestructor<std::vector<InlineRequest>> requests;
  return *requests;
}

// 取出这个标签的请求（同时扔掉过期的）；岛不在了、放不下时为空。automatic：是不是切走
// 标签时自动进的。
MediaPip::Host* TakeRequest(content::WebContents* contents, bool* automatic) {
  const base::TimeTicks now = base::TimeTicks::Now();
  MediaPip::Host* found = nullptr;
  std::erase_if(Requests(), [&](const InlineRequest& request) {
    if (!request.contents || now - request.time > kRequestLifetime) {
      return true;
    }
    if (request.contents.get() == contents) {
      found = request.host;
      *automatic = request.automatic;
      return true;
    }
    return false;
  });
  if (found && std::ranges::find(Hosts(), found) != Hosts().end() &&
      found->CanHostVideo(contents)) {
    return found;
  }
  return nullptr;
}

void ExitViaUi(PictureInPictureWindowManager::UiBehavior behavior) {
  PictureInPictureWindowManager::GetInstance()->ExitPictureInPictureViaWindowUi(
      behavior);
}

}  // namespace

bool DeferAutoPictureInPictureExit(content::WebContents* contents) {
  MediaPip* pip = MediaPip::ForWebContents(contents);
  return pip && pip->is_inline();
}

bool AllowAutoPictureInPicture(content::WebContents* contents) {
  return MediaPip::AutomaticEnabled(contents);
}

// static
bool MediaPip::AutomaticEnabled(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  PrefService* prefs = prefs::PrefsFor(
      Profile::FromBrowserContext(contents->GetBrowserContext()));
  // 收起（kMediaDocked）只管音乐放在哪里，视频照样浮在角上。
  return prefs->GetBoolean(prefs::kMediaControlsEnabled) &&
         prefs->GetBoolean(prefs::kMediaAutoPip);
}

// static
std::unique_ptr<content::VideoOverlayWindow> MediaPip::Create(
    content::VideoPictureInPictureWindowController* controller) {
  return base::WrapUnique(new MediaPip(controller));
}

// static
void MediaPip::AddHost(Host* host) {
  Hosts().push_back(host);
}

// static
void MediaPip::RemoveHost(Host* host) {
  std::erase(Hosts(), host);
  std::erase_if(Requests(), [host](const InlineRequest& request) {
    return request.host == host;
  });
  for (MediaPip* pip : Pips()) {
    if (pip->host_ == host) {
      // 窗口要关了（标签跟着一起关）：关掉画中画，不再回调 host。
      pip->host_ = nullptr;
      pip->view_ = nullptr;
      if (pip->shown_) {
        ExitViaUi(PictureInPictureWindowManager::UiBehavior::kCloseWindowOnly);
      }
    }
  }
}

// static
void MediaPip::RequestInline(content::WebContents* contents,
                             Host* host,
                             bool automatic) {
  std::erase_if(Requests(), [contents](const InlineRequest& request) {
    return !request.contents || request.contents.get() == contents;
  });
  Requests().push_back(
      {contents->GetWeakPtr(), host, base::TimeTicks::Now(), automatic});
}

// static
bool MediaPip::CanShowInline(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  for (Host* host : Hosts()) {
    if (host->CanHostVideo(contents)) {
      return true;
    }
  }
  return false;
}

// static
void MediaPip::EnterFromBrowserUi(content::WebContents* contents) {
  content::MediaSession* session =
      contents ? content::MediaSession::Get(contents) : nullptr;
  if (!session) {
    return;
  }
  for (Host* host : Hosts()) {
    if (host->CanHostVideo(contents)) {
      RequestInline(contents, host, /*automatic=*/false);
      break;
    }
  }
  session->EnterPictureInPicture();
}

// static
MediaPip* MediaPip::ForWebContents(content::WebContents* contents) {
  for (MediaPip* pip : Pips()) {
    if (pip->shown_ && pip->web_contents() == contents) {
      return pip;
    }
  }
  return nullptr;
}

// static
MediaPip* MediaPip::Current() {
  for (MediaPip* pip : Pips()) {
    if (pip->shown_) {
      return pip;
    }
  }
  return nullptr;
}

std::string MediaPip::DebugState() {
  return base::StrCat(
      {host_ ? "inline"
             : (desktop_ && desktop_->IsVisible() ? "desktop" : "none"),
       " bounds=", GetBounds().ToString(), " natural=",
       natural_size_.ToString(), " playing=", playing() ? "1" : "0",
       " surface=", surface_id_.is_valid() ? "1" : "0",
       " source=", SourceBounds().ToString(),
       " hovered=", view_ && view_->hovered() ? "1" : "0",
       " automatic=", automatically_entered_ ? "1" : "0",
       " can_move_inline=", CanMoveInline() ? "1" : "0"});
}

void MediaPip::SetHoveredForTesting(bool hovered) {
  if (view_) {
    view_->SetHovered(hovered);
  }
}

views::Widget* MediaPip::desktop_widget() const {
  return desktop_ && desktop_->IsVisible() ? desktop_->widget() : nullptr;
}

MediaPip::MediaPip(content::VideoPictureInPictureWindowController* controller)
    : controller_(controller) {
  Pips().push_back(this);
}

MediaPip::~MediaPip() {
  std::erase(Pips(), this);
  if (host_) {
    Host* host = host_;
    host_ = nullptr;
    view_ = nullptr;
    host->DetachVideo(this);
  }
  view_ = nullptr;
  desktop_.reset();
}

content::WebContents* MediaPip::web_contents() const {
  return controller_->GetWebContents();
}

MediaHub* MediaPip::hub() const {
  content::WebContents* contents = web_contents();
  return contents ? MediaHub::Get(Profile::FromBrowserContext(
                        contents->GetBrowserContext()))
                  : nullptr;
}

const MediaEntry* MediaPip::entry() const {
  content::WebContents* contents = web_contents();
  MediaHub* media = hub();
  if (!contents || !media) {
    return nullptr;
  }
  return media->Find(
      content::MediaSession::GetRequestIdFromWebContents(contents));
}

gfx::Rect MediaPip::SourceBounds() const {
  content::WebContents* contents = web_contents();
  content::RenderWidgetHostView* view =
      contents ? contents->GetRenderWidgetHostView() : nullptr;
  if (!view) {
    return gfx::Rect();
  }
  // Blink 的 source_bounds 用物理像素，Views 用 DIP。
  // 按网页视图的真实比例换算，避免 Retina 上的位置、大小翻倍。
  return gfx::ScaleToEnclosingRect(controller_->GetSourceBounds(),
                                   1.f / view->GetDeviceScaleFactor());
}

void MediaPip::TogglePlayPause() {
  if (!shown_) {
    return;
  }
  // 先按要变成的样子画（网页报回来之前）。
  playback_state_ = controller_->TogglePlayPause() ? kPlaying : kPaused;
  Changed();
}

void MediaPip::SeekBy(base::TimeDelta delta) {
  if (!shown_ || !position_) {
    return;
  }
  const base::TimeDelta duration = position_->duration();
  base::TimeDelta target = position_->GetPosition() + delta;
  target = std::max(base::TimeDelta(), target);
  if (!duration.is_zero() && !duration.is_max() && !duration.is_inf()) {
    target = std::min(target, duration);
  }
  controller_->SeekTo(target);
}

void MediaPip::SeekToFraction(double fraction) {
  if (!shown_ || !position_) {
    return;
  }
  const base::TimeDelta duration = position_->duration();
  if (duration.is_zero() || duration.is_max() || duration.is_inf()) {
    return;
  }
  controller_->SeekTo(duration * std::clamp(fraction, 0.0, 1.0));
}

void MediaPip::NextTrack() {
  if (shown_) {
    controller_->NextTrack();
  }
}

void MediaPip::PreviousTrack() {
  if (shown_) {
    controller_->PreviousTrack();
  }
}

void MediaPip::ToggleMute() {
  const MediaEntry* media = entry();
  if (MediaHub* hub_ptr = hub(); media && hub_ptr) {
    hub_ptr->SetMuted(media->id, !media->muted);
  }
}

void MediaPip::BackToTab() {
  if (!shown_) {
    return;
  }
  if (host_) {
    // 切到那个标签：岛看到标签切过去了，把画面飞回网页再关掉画中画。
    if (const MediaEntry* media = entry()) {
      hub()->ShowTab(media->id);
      return;
    }
  }
  ExitViaUi(
      PictureInPictureWindowManager::UiBehavior::kCloseWindowAndFocusOpener);
}

void MediaPip::CloseByUser() {
  // 先告诉各个窗口的岛：暂停以后它不要马上又以胶囊出现在角上。
  if (content::WebContents* contents = web_contents()) {
    for (Host* host : Hosts()) {
      host->VideoClosedByUser(contents);
    }
  }
  ExitViaUi(
      show_play_pause_
          ? PictureInPictureWindowManager::UiBehavior::kCloseWindowAndPauseVideo
          : PictureInPictureWindowManager::UiBehavior::kCloseWindowOnly);
}

void MediaPip::ReturnToPage() {
  if (shown_) {
    ExitViaUi(PictureInPictureWindowManager::UiBehavior::kCloseWindowOnly);
  }
}

void MediaPip::PopOut(const std::optional<gfx::Rect>& from) {
  if (!host_ || !shown_) {
    return;
  }
  std::optional<gfx::Rect> start = from;
  if (!start && view_ && view_->GetWidget()) {
    start = view_->GetBoundsInScreen();
  }
  Host* host = host_;
  automatically_entered_ = false;
  host_ = nullptr;
  view_ = nullptr;
  host->DetachVideo(this);
  ShowDesktop(start);
}

void MediaPip::MoveInline() {
  if (!shown_ || host_) {
    return;
  }
  content::WebContents* contents = web_contents();
  for (Host* host : Hosts()) {
    if (host->CanHostVideo(contents)) {
      HideDesktop();
      automatically_entered_ = false;
      host_ = host;
      host->AttachVideo(this);
      return;
    }
  }
  // 标签就是那个窗口的当前标签（小窗上这时没有这个按钮，只有调试步骤会到这里）：
  // 画面直接回到网页。
  ExitViaUi(
      PictureInPictureWindowManager::UiBehavior::kCloseWindowAndFocusOpener);
}

bool MediaPip::CanMoveInline() const {
  return shown_ && !host_ && CanShowInline(web_contents());
}

void MediaPip::DesktopBoundsChanged() {
  for (Host* host : Hosts()) {
    host->DesktopVideoMoved();
  }
}

void MediaPip::SetView(MediaVideoView* view) {
  view_ = view;
  if (view_) {
    view_->Update();
  }
}

void MediaPip::ViewBoundsChanged() {
  if (!shown_ || !view_) {
    return;
  }
  const gfx::Size size = view_->size();
  if (size == last_size_ || size.IsEmpty()) {
    return;
  }
  last_size_ = size;
  // 网页里 PictureInPictureWindow 的大小、resize 事件。
  controller_->UpdateLayerBounds();
}

void MediaPip::OnDesktopWindowClosed() {
  // 系统关掉了小窗（比如退出时）：在它自己的回调外面删、关掉画中画。
  if (desktop_) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(desktop_));
  }
  view_ = nullptr;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<MediaPip> pip) {
            if (pip && pip->shown_) {
              ExitViaUi(
                  PictureInPictureWindowManager::UiBehavior::kCloseWindowOnly);
            }
          },
          weak_factory_.GetWeakPtr()));
}

void MediaPip::ShowDesktop(const std::optional<gfx::Rect>& from) {
  if (!desktop_) {
    desktop_ = std::make_unique<MediaPipWindow>(weak_factory_.GetWeakPtr());
  }
  view_ = desktop_->video();
  desktop_->ShowWindow(from);
  Changed();
  DesktopBoundsChanged();
}

void MediaPip::HideDesktop() {
  if (!desktop_) {
    return;
  }
  if (MediaVideoView* video = desktop_->video()) {
    if (view_ == video) {
      view_ = nullptr;
    }
    video->ClearSurface();
  }
  desktop_->Hide();
  DesktopBoundsChanged();
}

void MediaPip::Changed() {
  if (view_) {
    view_->Update();
  }
  if (host_) {
    host_->VideoChanged(this);
  }
}

// content::VideoOverlayWindow ------------------------------------------------

bool MediaPip::IsActive() const {
  return desktop_ && desktop_->IsVisible() && desktop_->IsActive();
}

void MediaPip::Close() {
  Hide();
  desktop_.reset();
}

void MediaPip::ShowInactive() {
  const bool was_shown = shown_;
  shown_ = true;
  if (was_shown && (host_ || (desktop_ && desktop_->IsVisible()))) {
    // 同一个窗口换了一段视频（网页里另一个播放器进了画中画）。
    Changed();
    return;
  }
  bool automatic = false;
  if (Host* host = TakeRequest(web_contents(), &automatic)) {
    automatically_entered_ = automatic;
    host_ = host;
    host->AttachVideo(this);
    return;
  }
  ShowDesktop(std::nullopt);
}

void MediaPip::Hide() {
  shown_ = false;
  automatically_entered_ = false;
  last_size_ = gfx::Size();
  if (host_) {
    Host* host = host_;
    host_ = nullptr;
    view_ = nullptr;
    host->DetachVideo(this);
  }
  HideDesktop();
}

bool MediaPip::IsVisible() const {
  return shown_;
}

gfx::Rect MediaPip::GetBounds() {
  if (view_ && view_->GetWidget()) {
    return view_->GetBoundsInScreen();
  }
  if (desktop_ && desktop_->IsVisible()) {
    return desktop_->GetBoundsInScreen();
  }
  return gfx::Rect();
}

void MediaPip::UpdateNaturalSize(const gfx::Size& natural_size) {
  if (natural_size.IsEmpty()) {
    return;
  }
  natural_size_ = natural_size;
  if (desktop_) {
    desktop_->UpdateNaturalSize();
  }
  Changed();
}

void MediaPip::SetPlaybackState(PlaybackState playback_state) {
  playback_state_ = playback_state;
  Changed();
}

void MediaPip::SetPlayPauseButtonVisibility(bool is_visible) {
  show_play_pause_ = is_visible;
  Changed();
}

void MediaPip::SetSkipAdButtonVisibility(bool is_visible) {}

void MediaPip::SetNextTrackButtonVisibility(bool is_visible) {
  show_next_track_ = is_visible;
  Changed();
}

void MediaPip::SetPreviousTrackButtonVisibility(bool is_visible) {
  show_previous_track_ = is_visible;
  Changed();
}

void MediaPip::SetHidePictureInPictureButtonVisibility(bool is_visible) {}
void MediaPip::SetMicrophoneMuted(bool muted) {}
void MediaPip::SetCameraState(bool turned_on) {}

void MediaPip::SetMediaMuted(bool muted) {
  Changed();
}

void MediaPip::SetToggleMicrophoneButtonVisibility(bool is_visible) {}
void MediaPip::SetToggleCameraButtonVisibility(bool is_visible) {}
void MediaPip::SetHangUpButtonVisibility(bool is_visible) {}
void MediaPip::SetNextSlideButtonVisibility(bool is_visible) {}
void MediaPip::SetPreviousSlideButtonVisibility(bool is_visible) {}

void MediaPip::SetMediaPosition(const media_session::MediaPosition& position) {
  position_ = position;
  Changed();
}

void MediaPip::SetSourceTitle(const std::u16string& source_title) {}

void MediaPip::SetFaviconImages(
    const std::vector<media_session::MediaImage>& images) {}

void MediaPip::SetSurfaceId(const viz::SurfaceId& surface_id) {
  surface_id_ = surface_id;
  Changed();
}

void MediaPip::SetPlaybackControlsVisibility(bool is_visible) {}

void MediaPip::SetImmersiveVideoOptions(
    const content::ImmersiveOptions& options) {}

}  // namespace starry
