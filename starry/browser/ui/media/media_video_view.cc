// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_video_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "cc/layers/deadline_policy.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "skia/ext/image_operations.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/media/media_parts.h"
#include "starry/browser/ui/media/media_pip.h"
#include "starry/browser/ui/media/media_waveform.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_surface.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

using media_session::mojom::MediaSessionAction;

constexpr int kEdge = 8;
constexpr int kTopButton = 26;
constexpr int kChipHeight = 26;
constexpr int kSeekButton = 34;
constexpr int kTrackButton = 30;
constexpr int kMuteButton = 24;
constexpr int kTimeRow = 20;
constexpr int kWaveHeight = 16;
constexpr base::TimeDelta kSeekStep = base::Seconds(10);

SkColor White(float alpha) {
  return SkColorSetA(SK_ColorWHITE, static_cast<U8CPU>(alpha * 255));
}

SkColor Black(float alpha) {
  return SkColorSetA(SK_ColorBLACK, static_cast<U8CPU>(alpha * 255));
}

// 时间：「12:08 / 24:31」；不知道总长（直播）时是「直播」。
std::u16string TimeText(const std::optional<media_session::MediaPosition>& p) {
  if (!p) {
    return std::u16string();
  }
  const base::TimeDelta duration = p->duration();
  if (duration.is_zero() || duration.is_max() || duration.is_inf()) {
    return L("media.live");
  }
  return base::StrCat(
      {FormatMediaTime(p->GetPosition()), u" / ", FormatMediaTime(duration)});
}

// 左上的「回到标签页」：深色半透明的胶囊，网站图标和字（窄的时候只有图标）。
class BackChip : public views::Button {
  METADATA_HEADER(BackChip, views::Button)

 public:
  explicit BackChip(PressedCallback callback)
      : views::Button(std::move(callback)) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetAnimationDuration(base::Milliseconds(180));
    SetWindowDraggable(this, false);
    const std::u16string text = L("media.backToTab");
    SetTooltipText(text);
    GetViewAccessibility().SetName(text);
  }

  void SetFavicon(const gfx::ImageSkia& favicon) {
    favicon_ = favicon;
    SchedulePaint();
  }
  void SetCompact(bool compact) {
    if (compact_ != compact) {
      compact_ = compact;
      SchedulePaint();
    }
  }
  int FullWidth() const {
    return 8 + 14 + 6 +
           gfx::GetStringWidth(L("media.backToTab"),
                               UiFont(11, gfx::Font::Weight::SEMIBOLD)) +
           10;
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const float hover = static_cast<float>(hover_animation().GetCurrentValue());
    const gfx::RectF bounds(GetLocalBounds());
    const float radius = bounds.height() / 2;
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(Black(0.3f + 0.12f * hover));
    canvas->DrawRoundRect(bounds, radius, fill);
    cc::PaintFlags edge;
    edge.setAntiAlias(true);
    edge.setStyle(cc::PaintFlags::kStroke_Style);
    edge.setStrokeWidth(1);
    edge.setColor(White(0.18f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius - 0.5f, edge);

    const int icon_x = compact_ ? (width() - 14) / 2 : 8;
    const gfx::Rect icon(icon_x, (height() - 14) / 2, 14, 14);
    if (!favicon_.isNull()) {
      canvas->DrawImageInt(
          gfx::ImageSkiaOperations::CreateResizedImage(
              favicon_, skia::ImageOperations::RESIZE_BEST, icon.size()),
          icon.x(), icon.y());
    } else {
      PaintMediaGlyph(canvas, MediaGlyph::kShowTab, gfx::RectF(icon),
                      SK_ColorWHITE);
    }
    if (!compact_) {
      DrawMediaText(canvas, L("media.backToTab"), 11, /*semibold=*/true,
                    White(0.95f),
                    gfx::Rect(icon.right() + 6, 0,
                              width() - icon.right() - 6 - 8, height()));
    }
  }

 private:
  gfx::ImageSkia favicon_;
  bool compact_ = false;
};

BEGIN_METADATA(BackChip)
END_METADATA

}  // namespace

// 盖在画面上的控件和上下两道渐暗。
class MediaVideoView::Controls : public views::View {
  METADATA_HEADER(Controls, views::View)

 public:
  Controls(base::WeakPtr<MediaPip> pip, Place place)
      : pip_(pip), place_(place) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    const auto button = [this](base::RepeatingClosure action, MediaGlyph glyph,
                               const std::u16string& tooltip, int size,
                               int glyph_size, MediaButton::Style style) {
      MediaButton* view = AddChildView(std::make_unique<MediaButton>(
          std::move(action), glyph, tooltip, size, glyph_size, style));
      view->SetGlyphColor(SK_ColorWHITE);
      SetWindowDraggable(view, false);
      return view;
    };
    const auto pip_call = [](base::WeakPtr<MediaPip> pip,
                             void (MediaPip::*method)()) {
      return base::BindRepeating(method, pip);
    };
    back_ = AddChildView(std::make_unique<BackChip>(
        base::BindRepeating(&MediaPip::BackToTab, pip)));
    if (place == Place::kInline) {
      place_button_ = button(base::BindRepeating(
                                 [](base::WeakPtr<MediaPip> pip) {
                                   if (pip) {
                                     pip->PopOut();
                                   }
                                 },
                                 pip),
                             MediaGlyph::kPopOut, L("media.popOut"), kTopButton,
                             13, MediaButton::Style::kOverlay);
    } else {
      place_button_ = button(pip_call(pip, &MediaPip::MoveInline),
                             MediaGlyph::kFloat, L("media.inWindow"),
                             kTopButton, 14, MediaButton::Style::kOverlay);
    }
    close_ = button(pip_call(pip, &MediaPip::CloseByUser), MediaGlyph::kClose,
                    L("media.closePip"), kTopButton, 11,
                    MediaButton::Style::kOverlay);
    previous_ = button(pip_call(pip, &MediaPip::PreviousTrack),
                       MediaGlyph::kPrevious, L("media.previous"), kTrackButton,
                       14, MediaButton::Style::kGhost);
    back10_ = button(base::BindRepeating(&MediaPip::SeekBy, pip, -kSeekStep),
                     MediaGlyph::kBack10, L("media.seekBackward"), kSeekButton,
                     22, MediaButton::Style::kGhost);
    play_ =
        button(pip_call(pip, &MediaPip::TogglePlayPause), MediaGlyph::kPause,
               L("media.pause"), 44, 18, MediaButton::Style::kOverlay);
    forward10_ = button(base::BindRepeating(&MediaPip::SeekBy, pip, kSeekStep),
                        MediaGlyph::kForward10, L("media.seekForward"),
                        kSeekButton, 22, MediaButton::Style::kGhost);
    next_ =
        button(pip_call(pip, &MediaPip::NextTrack), MediaGlyph::kNext,
               L("media.next"), kTrackButton, 14, MediaButton::Style::kGhost);
    mute_ = button(pip_call(pip, &MediaPip::ToggleMute), MediaGlyph::kSpeaker,
                   L("tabs.mute"), kMuteButton, 14, MediaButton::Style::kGhost);
    if (MediaHub* hub = pip ? pip->hub() : nullptr) {
      wave_ = AddChildView(std::make_unique<MediaWaveform>(
          hub, MediaWaveform::Style::kSpectrum));
      wave_->SetCanProcessEventsWithinSubtree(false);
    }
    seek_ = AddChildView(std::make_unique<MediaSeekBar>(
        base::BindRepeating(&MediaPip::SeekToFraction, pip)));
    SetWindowDraggable(seek_, false);
  }

  void Update() {
    if (!pip_) {
      return;
    }
    const MediaEntry* entry = pip_->entry();
    const Theme dark = Theme::DarkBase();
    const Theme media = MediaThemeFor(entry, dark);
    const bool playing = pip_->playing();
    play_->SetGlyph(playing ? MediaGlyph::kPause : MediaGlyph::kPlay,
                    L(playing ? "media.pause" : "media.play"));
    play_->SetVisible(pip_->can_play_pause());
    // 桌面小窗放不回窗口（标签就是当前标签）时不显示「放回窗口」：左上「回到标签页」就是它。
    place_button_->SetVisible(place_ == Place::kInline ||
                              pip_->CanMoveInline());
    const bool muted = entry && entry->muted;
    mute_->SetGlyph(muted ? MediaGlyph::kMuted : MediaGlyph::kSpeaker,
                    L(muted ? "tabs.unmute" : "tabs.mute"));
    back_->SetFavicon(entry ? entry->favicon : gfx::ImageSkia());
    if (wave_ && entry) {
      wave_->Set(*entry, White(0.85f), media.accent, White(0.25f));
    }
    UpdatePosition();
    InvalidateLayout();
  }

  // 时间和进度（在放时每 0.25 秒）。
  void UpdatePosition() {
    if (!pip_) {
      return;
    }
    const std::optional<media_session::MediaPosition>& position =
        pip_->position();
    std::optional<double> progress;
    bool seekable = false;
    if (position) {
      const base::TimeDelta duration = position->duration();
      seekable =
          !duration.is_zero() && !duration.is_max() && !duration.is_inf();
      if (seekable) {
        progress = std::clamp(position->GetPosition() / duration, 0.0, 1.0);
      }
    }
    const MediaEntry* entry = pip_->entry();
    seek_->Set(progress, MediaThemeFor(entry, Theme::DarkBase()).accent,
               Theme::DarkBase());
    back10_->SetEnabled(seekable);
    forward10_->SetEnabled(seekable);
    const std::u16string time = TimeText(position);
    if (time != time_) {
      time_ = time;
      SchedulePaint();
    }
  }

  bool HitsControl(const gfx::Point& point) const {
    for (const views::View* child : children()) {
      if (child == wave_ || !child->GetVisible()) {
        continue;
      }
      if (child->bounds().Contains(point)) {
        return true;
      }
    }
    return false;
  }

  // views::View:
  void Layout(PassKey) override {
    const int w = width();
    const int h = height();
    // 上面一排。
    close_->SetBounds(w - kEdge - kTopButton, kEdge, kTopButton, kTopButton);
    place_button_->SetBounds(close_->x() - 6 - kTopButton, kEdge, kTopButton,
                             kTopButton);
    const int chip_room =
        (place_button_->GetVisible() ? place_button_->x() : close_->x()) - 8 -
        kEdge;
    const bool compact = back_->FullWidth() > chip_room;
    back_->SetCompact(compact);
    back_->SetBounds(kEdge, kEdge, compact ? kChipHeight : back_->FullWidth(),
                     kChipHeight);

    // 下面：时间、静音；进度条；够高时再加频谱。
    const int time_y = h - kEdge - kTimeRow;
    mute_->SetBounds(w - kEdge - kMuteButton,
                     time_y + (kTimeRow - kMuteButton) / 2, kMuteButton,
                     kMuteButton);
    time_rect_ =
        gfx::Rect(12, time_y, std::max(0, mute_->x() - 12 - 4), kTimeRow);
    const int seek_y = time_y - MediaSeekBar::kHeight + 2;
    seek_->SetBounds(12, seek_y, std::max(0, w - 24), MediaSeekBar::kHeight);
    if (wave_) {
      const bool wave = h >= 160;
      wave_->SetVisible(wave);
      if (wave) {
        wave_->SetBounds(12, seek_y - kWaveHeight, std::max(0, w - 24),
                         kWaveHeight);
      }
    }

    // 中间：后退、播放暂停、前进；有上一个、下一个且够宽时两边再加。
    const int play = (place_ == Place::kDesktop && h >= 260) ? 52 : 44;
    play_->SetBounds((w - play) / 2, (h - play) / 2 - 2, play, play);
    const int cy = play_->bounds().CenterPoint().y();
    const int spread = play / 2 + 14 + kSeekButton / 2;
    back10_->SetBounds(w / 2 - spread - kSeekButton / 2, cy - kSeekButton / 2,
                       kSeekButton, kSeekButton);
    forward10_->SetBounds(w / 2 + spread - kSeekButton / 2,
                          cy - kSeekButton / 2, kSeekButton, kSeekButton);
    const bool wide = w >= 300;
    previous_->SetVisible(wide && pip_ && pip_->can_previous_track());
    next_->SetVisible(wide && pip_ && pip_->can_next_track());
    const int track_spread = spread + kSeekButton / 2 + 8 + kTrackButton / 2;
    previous_->SetBounds(w / 2 - track_spread - kTrackButton / 2,
                         cy - kTrackButton / 2, kTrackButton, kTrackButton);
    next_->SetBounds(w / 2 + track_spread - kTrackButton / 2,
                     cy - kTrackButton / 2, kTrackButton, kTrackButton);
    // 太矮时中间只留播放暂停。
    const bool tall = h >= 120;
    back10_->SetVisible(tall);
    forward10_->SetVisible(tall);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags dim;
    dim.setColor(Black(0.12f));
    canvas->DrawRect(bounds, dim);
    // 上面一道、下面一道渐暗，字和按钮读得清。
    const float top = std::min(64.0f, bounds.height() * 0.35f);
    const float bottom = std::min(104.0f, bounds.height() * 0.55f);
    const auto gradient = [canvas](const gfx::RectF& rect, SkColor from,
                                   SkColor to) {
      cc::PaintFlags flags;
      const SkPoint points[2] = {gfx::PointFToSkPoint(rect.origin()),
                                 gfx::PointFToSkPoint(rect.bottom_left())};
      const SkColor4f colors[2] = {SkColor4f::FromColor(from),
                                   SkColor4f::FromColor(to)};
      flags.setShader(cc::PaintShader::MakeLinearGradient(
          points, colors, nullptr, 2, SkTileMode::kClamp));
      canvas->DrawRect(rect, flags);
    };
    gradient(gfx::RectF(0, 0, bounds.width(), top), Black(0.45f), Black(0));
    gradient(gfx::RectF(0, bounds.height() - bottom, bounds.width(), bottom),
             Black(0), Black(0.6f));
    if (!time_.empty()) {
      DrawMediaText(canvas, time_, 11, /*semibold=*/false, White(0.92f),
                    time_rect_);
    }
  }

 private:
  const base::WeakPtr<MediaPip> pip_;
  const Place place_;
  raw_ptr<BackChip> back_ = nullptr;
  raw_ptr<MediaButton> place_button_ = nullptr;
  raw_ptr<MediaButton> close_ = nullptr;
  raw_ptr<MediaButton> previous_ = nullptr;
  raw_ptr<MediaButton> back10_ = nullptr;
  raw_ptr<MediaButton> play_ = nullptr;
  raw_ptr<MediaButton> forward10_ = nullptr;
  raw_ptr<MediaButton> next_ = nullptr;
  raw_ptr<MediaButton> mute_ = nullptr;
  raw_ptr<MediaWaveform> wave_ = nullptr;
  raw_ptr<MediaSeekBar> seek_ = nullptr;
  gfx::Rect time_rect_;
  std::u16string time_;
};

BEGIN_METADATA(MediaVideoView, Controls)
END_METADATA

MediaVideoView::MediaVideoView(base::WeakPtr<MediaPip> pip, Place place)
    : pip_(pip) {
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  video_ = AddChildView(std::make_unique<views::View>());
  video_->SetPaintToLayer(ui::LAYER_SURFACE);
  video_->layer()->SetMasksToBounds(true);
  video_->layer()->SetFillsBoundsOpaquely(false);
  video_->layer()->SetName("MediaVideoView:Video");
  video_->layer()->AsSurface()->SetFallbackBackgroundColor(SkColors::kBlack);
  video_->SetCanProcessEventsWithinSubtree(false);
  controls_ = AddChildView(std::make_unique<Controls>(pip, place));
  controls_->layer()->SetOpacity(0);
  controls_->SetCanProcessEventsWithinSubtree(false);
  Update();
}

MediaVideoView::~MediaVideoView() {
  UnregisterFrameSink();
}

void MediaVideoView::Update() {
  if (!pip_) {
    return;
  }
  if (pip_->surface_id().is_valid() && pip_->surface_id() != surface_id_) {
    surface_id_ = pip_->surface_id();
    ShowSurface();
  }
  controls_->Update();
  UpdateControlsShown();
  InvalidateLayout();
}

void MediaVideoView::ClearSurface() {
  UnregisterFrameSink();
  surface_id_ = viz::SurfaceId();
}

void MediaVideoView::SetHovered(bool hovered) {
  if (hovered_ == hovered) {
    return;
  }
  hovered_ = hovered;
  if (hovered_) {
    // 能不能放回窗口随当前标签变，控件出现时重新看一遍。
    controls_->Update();
  }
  UpdateControlsShown();
}

bool MediaVideoView::HitsControl(const gfx::Point& point) const {
  if (!controls_shown_) {
    return false;
  }
  gfx::Point local = point;
  views::View::ConvertPointToTarget(this, controls_, &local);
  return controls_->HitsControl(local);
}

void MediaVideoView::UpdateControlsShown() {
  // 悬停、暂停着时显示（暂停时一眼看出来，也好点播放）。
  const bool shown = pip_ && (hovered_ || !pip_->playing());
  if (shown == controls_shown_) {
    return;
  }
  controls_shown_ = shown;
  controls_->SetCanProcessEventsWithinSubtree(shown);
  controls_fade_.AnimateOpacity(controls_->layer(), shown ? 1 : 0,
                                MotionCurve::EaseOut(motion::kMediaFade));
  UpdateTicker();
}

void MediaVideoView::UpdateTicker() {
  const bool tick = controls_shown_ && pip_ && pip_->playing() && GetWidget();
  if (tick && !ticker_.IsRunning()) {
    ticker_.Start(FROM_HERE, base::Milliseconds(250),
                  base::BindRepeating(&Controls::UpdatePosition,
                                      base::Unretained(controls_.get())));
  } else if (!tick) {
    ticker_.Stop();
  }
}

gfx::Rect MediaVideoView::VideoBounds() const {
  const gfx::Rect bounds = GetLocalBounds();
  const gfx::Size natural = pip_ ? pip_->natural_size() : natural_;
  if (natural.IsEmpty() || bounds.IsEmpty()) {
    return bounds;
  }
  // 按比例放进来；只差一两个像素时铺满（同 Chrome
  // 的画中画，免得边上一道黑线）。
  const double scale =
      std::min(static_cast<double>(bounds.width()) / natural.width(),
               static_cast<double>(bounds.height()) / natural.height());
  int w = static_cast<int>(std::lround(natural.width() * scale));
  int h = static_cast<int>(std::lround(natural.height() * scale));
  if (bounds.width() - w <= 2) {
    w = bounds.width();
  }
  if (bounds.height() - h <= 2) {
    h = bounds.height();
  }
  return gfx::Rect((bounds.width() - w) / 2, (bounds.height() - h) / 2, w, h);
}

void MediaVideoView::Layout(PassKey) {
  const gfx::Rect video = VideoBounds();
  if (video != video_->bounds()) {
    video_->SetBoundsRect(video);
    video_->layer()->AsSurface()->SetSurfaceSize(video.size());
  }
  controls_->SetBoundsRect(GetLocalBounds());
  if (pip_) {
    natural_ = pip_->natural_size();
    pip_->ViewBoundsChanged();
  }
}

void MediaVideoView::ShowSurface() {
  if (!surface_id_.is_valid()) {
    return;
  }
  RegisterFrameSink();
  video_->layer()->AsSurface()->SetShowSurface(
      surface_id_, VideoBounds().size(),
      cc::DeadlinePolicy::UseDefaultDeadline(),
      /*stretch_content_to_fill_bounds=*/true);
}

void MediaVideoView::RegisterFrameSink() {
  views::Widget* widget = GetWidget();
  ui::Compositor* compositor = widget ? widget->GetCompositor() : nullptr;
  if (!surface_id_.is_valid() || !compositor) {
    return;
  }
  if (compositor_ == compositor && registered_ == surface_id_.frame_sink_id()) {
    return;
  }
  UnregisterFrameSink();
  compositor->AddChildFrameSink(surface_id_.frame_sink_id());
  compositor_ = compositor;
  registered_ = surface_id_.frame_sink_id();
  compositor_observation_.Observe(compositor);
}

void MediaVideoView::UnregisterFrameSink() {
  if (!compositor_) {
    return;
  }
  compositor_->RemoveChildFrameSink(registered_);
  compositor_observation_.Reset();
  compositor_ = nullptr;
  registered_ = viz::FrameSinkId();
}

void MediaVideoView::OnCompositingShuttingDown(ui::Compositor* compositor) {
  UnregisterFrameSink();
}

void MediaVideoView::AddedToWidget() {
  if (surface_id_.is_valid()) {
    ShowSurface();
  }
  UpdateTicker();
}

void MediaVideoView::RemovedFromWidget() {
  UnregisterFrameSink();
  ticker_.Stop();
}

void MediaVideoView::VisibilityChanged(views::View* starting_from,
                                       bool is_visible) {
  UpdateTicker();
}

BEGIN_METADATA(MediaVideoView)
END_METADATA

}  // namespace starry
