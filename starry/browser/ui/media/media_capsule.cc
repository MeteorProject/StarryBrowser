// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_capsule.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/media/media_parts.h"
#include "starry/browser/ui/media/media_player_view.h"
#include "starry/browser/ui/media/media_waveform.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace starry {

namespace {

constexpr int kArt = 22;
constexpr int kPopoverWidth = 300;
constexpr int kPopoverPadding = 14;

// 弹出框里的播放器。
class MediaPopover : public PopoverContent {
  METADATA_HEADER(MediaPopover, PopoverContent)

 public:
  MediaPopover(BrowserWindowInterface* browser, MediaHub* hub)
      : browser_(browser) {
    MediaPlayerView::Options options;
    options.placement_glyph = MediaGlyph::kFloat;
    options.placement_tooltip = L("media.float");
    options.placement = base::BindRepeating(
        [](MediaPopover* popover) {
          base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE, base::BindOnce(
                             [](base::WeakPtr<MediaPopover> popover) {
                               if (!popover) {
                                 return;
                               }
                               BrowserWindowInterface* browser =
                                   popover->browser_;
                               if (StarryBrowserView* view =
                                       StarryBrowserView::From(browser)) {
                                 view->popover_host()->Close();
                               }
                               prefs::PrefsFor(browser->GetProfile())
                                   ->SetBoolean(prefs::kMediaDocked, false);
                             },
                             popover->weak_factory_.GetWeakPtr()));
        },
        base::Unretained(this));
    options.size_changed = base::BindRepeating(
        [](MediaPopover* popover) {
          if (StarryBrowserView* view =
                  StarryBrowserView::From(popover->browser_)) {
            if (view->popover_host()->content() == popover) {
              view->popover_host()->ContentSizeChanged();
            }
          }
        },
        base::Unretained(this));
    player_ = AddChildView(std::make_unique<MediaPlayerView>(hub, options));
    prefs_.Init(prefs::PrefsFor(browser->GetProfile()));
    for (const char* name :
         {prefs::kMediaControlsEnabled, prefs::kMediaDocked}) {
      prefs_.Add(name, base::BindRepeating(&MediaPopover::OnPlacementChanged,
                                           base::Unretained(this)));
    }
  }

  // PopoverContent:
  gfx::Size ContentSize() const override {
    return gfx::Size(kPopoverWidth + 2 * kPopoverPadding,
                     player_->ContentHeight() + 2 * kPopoverPadding);
  }
  void SetTheme(const Theme& theme) override { player_->SetTheme(theme); }

  // views::View:
  void Layout(PassKey) override {
    gfx::Rect inner = GetLocalBounds();
    inner.Inset(kPopoverPadding);
    player_->SetBoundsRect(inner);
  }

 private:
  void OnPlacementChanged() {
    if (prefs_.prefs()->GetBoolean(prefs::kMediaControlsEnabled) &&
        prefs_.prefs()->GetBoolean(prefs::kMediaDocked)) {
      return;
    }
    if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
      if (view->popover_host()->content() == this) {
        view->popover_host()->Close();
      }
    }
  }

  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<MediaPlayerView> player_ = nullptr;
  PrefChangeRegistrar prefs_;
  base::WeakPtrFactory<MediaPopover> weak_factory_{this};
};

BEGIN_METADATA(MediaPopover)
END_METADATA

}  // namespace

MediaCapsule::MediaCapsule(BrowserWindowInterface* browser)
    : views::Button(base::BindRepeating(&MediaCapsule::ShowPlayer,
                                        base::Unretained(this))),
      browser_(browser),
      hub_(MediaHub::Get(browser->GetProfile())) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetAnimationDuration(base::Milliseconds(180));
  SetWindowDraggable(this, false);
  SetVisible(false);
  meter_ = AddChildView(
      std::make_unique<MediaWaveform>(hub_, MediaWaveform::Style::kMeter));
  meter_->SetBar(2, 1.6f);
  prefs_.Init(prefs::PrefsFor(browser->GetProfile()));
  prefs_.Add(
      prefs::kMediaControlsEnabled,
      base::BindRepeating(&MediaCapsule::Update, base::Unretained(this)));
  prefs_.Add(prefs::kMediaDocked,
             base::BindRepeating(&MediaCapsule::OnDockedChanged,
                                 base::Unretained(this)));
  observation_.Observe(hub_);
  Update();
}

MediaCapsule::~MediaCapsule() = default;

void MediaCapsule::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  Update();
}

void MediaCapsule::SetStripActive(bool active) {
  if (strip_active_ == active) {
    return;
  }
  strip_active_ = active;
  Update();
}

void MediaCapsule::SetPresenceChangedCallback(base::RepeatingClosure callback) {
  presence_changed_ = std::move(callback);
}

void MediaCapsule::OnMediaChanged() {
  Update();
}

void MediaCapsule::Update() {
  const MediaEntry* entry = hub_->PrimaryOutsidePictureInPicture();
  has_entry_ = entry != nullptr;
  if (entry) {
    entry_ = *entry;
    GetViewAccessibility().SetName(
        L("media.accessibility",
          {{"title", entry_.title}, {"source", entry_.source}}));
    SetTooltipText(entry_.title);
  }
  count_ = hub_->Entries().size();
  InvalidateLayout();
  media_theme_ = MediaThemeFor(has_entry_ ? &entry_ : nullptr, theme_);
  if (has_entry_) {
    meter_->Set(entry_, media_theme_.accent, media_theme_.accent,
                theme_.OnSurface(0.3f));
  }
  const bool docked =
      prefs::PrefsFor(browser_->GetProfile())->GetBoolean(prefs::kMediaDocked);
  const bool show = has_entry_ && docked && strip_active_ &&
                    prefs::PrefsFor(browser_->GetProfile())
                        ->GetBoolean(prefs::kMediaControlsEnabled);
  if (show != should_show_) {
    should_show_ = show;
    SetVisible(show);
    if (show && !reveal_timer_.IsRunning()) {
      fade_.AnimateOpacityFrom(layer(), 0, 1,
                               MotionCurve::EaseOut(motion::kMediaFade));
    }
    if (presence_changed_) {
      presence_changed_.Run();
    }
  }
  const bool tick = should_show_ && entry_.playing;
  if (tick && !ticker_.IsRunning()) {
    ticker_.Start(FROM_HERE, base::Seconds(1),
                  base::BindRepeating(&views::View::SchedulePaint,
                                      base::Unretained(this)));
  } else if (!tick) {
    ticker_.Stop();
  }
  SchedulePaint();
}

void MediaCapsule::OnDockedChanged() {
  const bool docked =
      prefs::PrefsFor(browser_->GetProfile())->GetBoolean(prefs::kMediaDocked);
  if (docked && has_entry_ && strip_active_ && !motion::ReduceMotion()) {
    // 媒体岛正飞过来：等它快落位时（Reveal）再淡入。
    fade_.SetOpacity(layer(), 0);
    reveal_timer_.Start(
        FROM_HERE, base::Seconds(0.6),
        base::BindOnce(&MediaCapsule::Reveal, base::Unretained(this)));
  }
  Update();
}

void MediaCapsule::Reveal() {
  reveal_timer_.Stop();
  fade_.AnimateOpacity(layer(), 1,
                       MotionCurve::EaseOut(motion::kMediaDockFlightFade));
}

void MediaCapsule::ShowPlayer() {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view || !view->popover_host() || !has_entry_ || !should_show_) {
    return;
  }
  const gfx::Rect anchor =
      views::View::ConvertRectToTarget(parent(), view, bounds());
  view->popover_host()->Show(std::make_unique<MediaPopover>(browser_, hub_),
                             anchor, PopoverHost::Placement::kBelowRight);
}

void MediaCapsule::Layout(PassKey) {
  int right = width() - 10;
  if (count_ > 1) {
    const gfx::FontList& font = UiFont(10, gfx::Font::Weight::SEMIBOLD);
    const int chip_width = std::max(
        16, gfx::GetStringWidth(base::NumberToString16(count_), font) + 8);
    right = width() - 7 - chip_width - 6;
  }
  meter_->SetBounds(right - 14, (height() - 12) / 2, 14, 12);
}

void MediaCapsule::PaintButtonContents(gfx::Canvas* canvas) {
  if (!has_entry_) {
    return;
  }
  const gfx::RectF bounds(GetLocalBounds());
  const float radius = bounds.height() / 2;
  const float hover = static_cast<float>(hover_animation().GetCurrentValue());
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  const SkPoint points[2] = {gfx::PointFToSkPoint(bounds.origin()),
                             gfx::PointFToSkPoint(bounds.top_right())};
  const SkColor4f colors[2] = {
      SkColor4f::FromColor(SkColorSetA(media_theme_.primary_container, 245)),
      SkColor4f::FromColor(SkColorSetA(media_theme_.primary_container, 90))};
  fill.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr, 2,
                                                     SkTileMode::kClamp));
  canvas->DrawRoundRect(bounds, radius, fill);
  if (hover > 0) {
    cc::PaintFlags overlay;
    overlay.setAntiAlias(true);
    overlay.setColor(theme_.OnSurface(0.08f * hover));
    canvas->DrawRoundRect(bounds, radius, overlay);
  }
  cc::PaintFlags edge;
  edge.setAntiAlias(true);
  edge.setStyle(cc::PaintFlags::kStroke_Style);
  edge.setStrokeWidth(1);
  edge.setColor(theme_.OnSurface(0.1f));
  gfx::RectF stroke = bounds;
  stroke.Inset(0.5f);
  canvas->DrawRoundRect(stroke, radius - 0.5f, edge);

  const gfx::PointF center(4 + kArt / 2.0f, bounds.CenterPoint().y());
  PaintArtwork(canvas,
               gfx::RectF(center.x() - kArt / 2.0f, center.y() - kArt / 2.0f,
                          kArt, kArt),
               kArt / 2.0f, entry_, media_theme_);
  if (const std::optional<double> progress = entry_.Progress()) {
    PaintProgressRing(canvas, center, kArt / 2.0f + 2.5f, 1.5f, *progress,
                      theme_.OnSurface(0.14f), media_theme_.accent);
  }
  int right = width() - 10;
  if (count_ > 1) {
    const std::u16string count = base::NumberToString16(count_);
    const gfx::FontList& font = UiFont(10, gfx::Font::Weight::SEMIBOLD);
    const int chip_width = std::max(16, gfx::GetStringWidth(count, font) + 8);
    const gfx::Rect chip(width() - 7 - chip_width, (height() - 16) / 2,
                         chip_width, 16);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme_.OnSurface(0.16f));
    canvas->DrawRoundRect(gfx::RectF(chip), 8, flags);
    canvas->DrawStringRectWithFlags(
        count, font, theme_.on_surface, chip,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    right = chip.x() - 6;
  }
  // 标题后面是三条电平。
  right = meter_->x() - 6;
  const int x = 4 + kArt + 9;
  DrawMediaText(canvas, entry_.title, 12, /*semibold=*/true,
                entry_.playing ? theme_.on_surface : theme_.on_surface_variant,
                gfx::Rect(x, (height() - 17) / 2, right - x, 17));
}

BEGIN_METADATA(MediaCapsule)
END_METADATA

}  // namespace starry
