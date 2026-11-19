// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_dock.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/media/media_parts.h"
#include "starry/browser/ui/media/media_player_view.h"
#include "starry/browser/ui/media/media_waveform.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 左右留的边。
constexpr int kInset = 10;
// 最上面那一行：封面、标题、艺术家和来源，右边一个按钮。
constexpr int kArt = 36;
constexpr int kArtTop = 8;
constexpr int kArtGap = 9;
// 视频的封面是宽的；卡片不到这么宽时和音乐一样用方的，标题多留一点。
constexpr int kVideoArt = 58;
constexpr int kWideCard = 260;
// 右边的按钮：收着时是播放暂停，展开后在同一个位置换成收起（标题的宽度不跟着变）。
constexpr int kAccessory = 28;
constexpr int kAccessoryTop = kArtTop + (kArt - kAccessory) / 2;
// 卡片下面边播边画的进度波形。
constexpr int kWaveTop = 47;
constexpr int kWaveHeight = 10;
// 播放器从最上面那一行下面开始，到底留这么多。
constexpr int kPlayerTop = 50;
constexpr int kBottomPadding = 10;
constexpr float kRadius = 12;
// 升起来的那一层的底比面板长出这么多，框也往上多出这么多：弹簧冲过头、展开着少了一行
// 时底下不露空，上面的阴影不被裁掉。
constexpr int kSlack = 64;
// 展开后面板上边的阴影往上散这么多。
constexpr int kShadowMargin = 20;
// 底边那一段描边的高度；角刚开始拐的这一小段还是那一层自己的侧边，不重复画。
constexpr int kRim = 12;
constexpr int kRimSkip = 4;

float Clamp01(double v) {
  return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

}  // namespace

// 只管画的一块（有自己的图层，内容不变就不重画），不接鼠标。
class MediaDock::Piece : public views::View {
  METADATA_HEADER(Piece, views::View)

 public:
  using Painter = base::RepeatingCallback<void(gfx::Canvas*, const gfx::Size&)>;

  explicit Piece(Painter painter) : painter_(std::move(painter)) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override { painter_.Run(canvas, size()); }

 private:
  Painter painter_;
};

BEGIN_METADATA(MediaDock, Piece)
END_METADATA

// 最上面那一行（收着时就是整张卡片）：点一下展开，再点一下收回。
class MediaDock::Row : public views::Button {
  METADATA_HEADER(Row, views::Button)

 public:
  explicit Row(MediaDock* dock)
      : views::Button(base::BindRepeating(
            [](MediaDock* dock) { dock->SetExpanded(!dock->expanded()); },
            base::Unretained(dock))),
        dock_(dock) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  // views::Button:
  void PaintButtonContents(gfx::Canvas* canvas) override {
    dock_->PaintRow(canvas, size());
  }

 private:
  const raw_ptr<MediaDock> dock_;
};

BEGIN_METADATA(MediaDock, Row)
END_METADATA

MediaDock::MediaDock(BrowserWindowInterface* browser)
    : browser_(browser),
      hub_(MediaHub::Get(browser->GetProfile())),
      expand_(0,
              base::BindRepeating(&MediaDock::OnExpandFrame,
                                  base::Unretained(this))),
      resize_(
          0,
          base::BindRepeating(&MediaDock::ApplyFrame, base::Unretained(this))) {
  // 自己不画东西：每一块有自己的图层，展开、收回时只动图层。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetNotifyEnterExitOnChild(true);
  SetWindowDraggable(this, false);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));

  stack_ = AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintStack, base::Unretained(this))));

  clip_ = AddChildView(std::make_unique<views::View>());
  clip_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  clip_->layer()->SetMasksToBounds(true);
  clip_->layer()->SetRoundedCornerRadius(
      gfx::RoundedCornersF(0, 0, kRadius, kRadius));
  clip_->layer()->SetIsFastRoundedCorner(true);

  rider_ = clip_->AddChildView(std::make_unique<views::View>());
  rider_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  shadow_ = rider_->AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintShadow, base::Unretained(this))));
  sheet_ = rider_->AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintSheet, base::Unretained(this))));
  row_ = rider_->AddChildView(std::make_unique<Row>(this));
  toggle_ = rider_->AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating(
          [](MediaDock* dock) {
            if (dock->has_entry_) {
              dock->hub_->TogglePlayPause(dock->entry_.id);
            }
          },
          base::Unretained(this)),
      MediaGlyph::kPause, L("media.pause"), kAccessory, 12,
      MediaButton::Style::kTonal));
  collapse_ = rider_->AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating([](MediaDock* dock) { dock->SetExpanded(false); },
                          base::Unretained(this)),
      MediaGlyph::kCollapse, L("media.collapse"), kAccessory, 14));
  wave_ = rider_->AddChildView(
      std::make_unique<MediaWaveform>(hub_, MediaWaveform::Style::kRecorded));
  wave_->SetBar(2, 1.6f);

  MediaPlayerView::Options options;
  options.compact = true;
  options.placement_glyph = MediaGlyph::kFloat;
  options.placement_tooltip = L("media.float");
  options.placement = base::BindRepeating(
      [](MediaDock* dock) {
        // 浮出去以后这张卡片会收起：不在按钮自己的回调里改。
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(
                           [](base::WeakPtr<MediaDock> dock) {
                             if (dock) {
                               dock->SetExpanded(false);
                               prefs::PrefsFor(dock->browser_->GetProfile())
                                   ->SetBoolean(prefs::kMediaDocked, false);
                             }
                           },
                           dock->weak_factory_.GetWeakPtr()));
      },
      base::Unretained(this));
  options.size_changed = base::BindRepeating(&MediaDock::OnPlayerSizeChanged,
                                             base::Unretained(this));
  // 点了其余的媒体里的一行：最上面那一行换成它。
  options.selection_changed =
      base::BindRepeating(&MediaDock::Update, base::Unretained(this));
  player_ =
      rider_->AddChildView(std::make_unique<MediaPlayerView>(hub_, options));
  player_->SetPaintToLayer();
  player_->layer()->SetFillsBoundsOpaquely(false);
  player_->SetVisible(false);
  full_height_ = FullHeight();

  rim_ = clip_->AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintRim, base::Unretained(this))));

  prefs_.Init(prefs::PrefsFor(browser->GetProfile()));
  prefs_.Add(prefs::kMediaControlsEnabled,
             base::BindRepeating(&MediaDock::Update, base::Unretained(this)));
  prefs_.Add(
      prefs::kMediaDocked,
      base::BindRepeating(&MediaDock::OnDockedChanged, base::Unretained(this)));
  observation_.Observe(hub_);
  Update();
}

MediaDock::~MediaDock() = default;

void MediaDock::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  player_->SetTheme(theme);
  sheet_->SchedulePaint();
  rim_->SchedulePaint();
  shadow_->SchedulePaint();
  Update();
}

void MediaDock::SetSidebarActive(bool active) {
  if (sidebar_active_ == active) {
    return;
  }
  sidebar_active_ = active;
  Update();
}

void MediaDock::SetPresenceChangedCallback(base::RepeatingClosure callback) {
  presence_changed_ = std::move(callback);
}

void MediaDock::SetLayoutChangedCallback(base::RepeatingClosure callback) {
  layout_changed_ = std::move(callback);
}

const MediaEntry* MediaDock::Entry() const {
  if (expanded_ && player_) {
    if (const std::optional<base::UnguessableToken> id = player_->selected()) {
      if (const MediaEntry* entry = hub_->Find(*id)) {
        return entry;
      }
    }
  }
  return hub_->PrimaryOutsidePictureInPicture();
}

void MediaDock::OnMediaChanged() {
  Update();
}

void MediaDock::Update() {
  if (!player_) {
    // 还在构造里（播放器第一次刷新）。
    return;
  }
  const MediaEntry* entry = Entry();
  has_entry_ = entry != nullptr;
  if (entry) {
    entry_ = *entry;
  }
  const size_t count = hub_->Entries().size();
  const Theme media_theme =
      MediaThemeFor(has_entry_ ? &entry_ : nullptr, theme_);
  // 底的颜色只跟封面的颜色、在不在放有关：没变就不重画那一大块。
  const bool playing = has_entry_ && entry_.playing;
  if (!(media_theme == media_theme_) || playing != sheet_playing_) {
    sheet_->SchedulePaint();
    stack_->SchedulePaint();
  }
  if (count != count_) {
    stack_->SchedulePaint();
  }
  media_theme_ = media_theme;
  sheet_playing_ = playing;
  count_ = count;
  toggle_->SetTheme(media_theme_);
  collapse_->SetTheme(media_theme_);
  if (has_entry_) {
    toggle_->SetGlyph(entry_.playing ? MediaGlyph::kPause : MediaGlyph::kPlay,
                      L(entry_.playing ? "media.pause" : "media.play"));
    wave_->Set(entry_, media_theme_.accent, media_theme_.accent,
               theme_.OnSurface(0.2f));
    row_->GetViewAccessibility().SetName(
        L("media.accessibility",
          {{"title", entry_.title}, {"source", entry_.source}}));
    // 窄的时候标题放不全：停一下看全名。
    row_->SetTooltipText(entry_.artist.empty()
                             ? entry_.title
                             : entry_.title + u" · " + entry_.artist);
  }
  row_->SchedulePaint();
  const bool docked =
      prefs::PrefsFor(browser_->GetProfile())->GetBoolean(prefs::kMediaDocked);
  const bool show = has_entry_ && docked && sidebar_active_ &&
                    prefs::PrefsFor(browser_->GetProfile())
                        ->GetBoolean(prefs::kMediaControlsEnabled);
  if (show != should_show_) {
    should_show_ = show;
    if (!show) {
      SetExpanded(false);
      // 位置合拢的时候跟着淡掉（浮出去时 OnDockedChanged 已经在淡了）。
      if (layer()->GetTargetOpacity() > 0) {
        fade_.AnimateOpacity(layer(), 0,
                             MotionCurve::EaseOut(motion::kMediaFade));
      }
    } else if (!reveal_timer_.IsRunning()) {
      fade_.AnimateOpacityFrom(layer(), 0, 1,
                               MotionCurve::EaseOut(motion::kMediaFade));
    }
    if (presence_changed_) {
      presence_changed_.Run();
    }
  }
  ApplyFrame();
}

void MediaDock::OnDockedChanged() {
  const bool docked =
      prefs::PrefsFor(browser_->GetProfile())->GetBoolean(prefs::kMediaDocked);
  if (docked && has_entry_ && sidebar_active_ && !motion::ReduceMotion()) {
    // 媒体岛正飞过来：位置先让出来，内容等它快落位时（Reveal）再淡入；这个窗口
    // 里没有岛飞过来时等一会儿自己淡入。
    fade_.SetOpacity(layer(), 0);
    reveal_timer_.Start(
        FROM_HERE, base::Seconds(0.6),
        base::BindOnce(&MediaDock::Reveal, base::Unretained(this)));
  } else if (!docked) {
    reveal_timer_.Stop();
    // 浮出去：媒体岛从这里飞走，这里马上淡掉。
    fade_.AnimateOpacity(layer(), 0,
                         MotionCurve::EaseOut(motion::kMediaDockFlightFade));
  }
  Update();
}

void MediaDock::Reveal() {
  reveal_timer_.Stop();
  fade_.AnimateOpacity(layer(), 1,
                       MotionCurve::EaseOut(motion::kMediaDockFlightFade));
}

int MediaDock::FullHeight() const {
  return kPlayerTop + player_->ContentHeight() + kBottomPadding;
}

void MediaDock::SetMaxHeight(int max_height) {
  if (max_height_ == max_height) {
    return;
  }
  max_height_ = max_height;
  int others = MediaPlayerView::kMaxOthers;
  while (others > 0 &&
         kPlayerTop + player_->HeightForOthers(others) + kBottomPadding >
             max_height) {
    --others;
  }
  // 侧栏正在排版：它接着就按新的高度摆，不用再通知。
  base::AutoReset<bool> setting(&setting_max_height_, true);
  player_->SetMaxOthers(others);
}

void MediaDock::OnPlayerSizeChanged() {
  if (!player_) {
    return;
  }
  const int height = FullHeight();
  if (height == full_height_) {
    return;
  }
  const int delta = height - full_height_;
  full_height_ = height;
  const double e = std::clamp(expand_.value(), 0.0, 1.0);
  if (e > 0 && Compositor()) {
    // 展开着：视图的上边挪了，面板先留在原处再滑到位。多出来的行从底边露出来；少了
    // 一行时上面的内容滑下来补上。
    resize_.SetValue(
        std::max<double>(resize_.value() + delta * e, kShadowMargin - kSlack));
    resize_.AnimateTo(Compositor(), 0, MotionCurve::Of(motion::kMediaDockRow));
  }
  if (!setting_max_height_) {
    InvalidateLayout();
    if (layout_changed_) {
      layout_changed_.Run();
    }
  }
}

gfx::Rect MediaDock::CardRect() const {
  return gfx::Rect(0, height() - kCardHeight, width(), kCardHeight);
}

gfx::Rect MediaDock::PanelRect() const {
  const int travel = std::max(0, height() - kCardHeight);
  const int top =
      std::clamp(static_cast<int>(std::lround((1 - expand_.value()) * travel +
                                              resize_.value())),
                 0, travel);
  return gfx::Rect(0, top, width(), height() - top);
}

ui::Compositor* MediaDock::Compositor() {
  return GetWidget() ? GetWidget()->GetCompositor() : nullptr;
}

void MediaDock::SetExpanded(bool expanded) {
  hover_timer_.Stop();
  leave_timer_.Stop();
  if (expanded && !should_show_) {
    return;
  }
  if (expanded_ == expanded) {
    return;
  }
  expanded_ = expanded;
  if (expanded && has_entry_) {
    player_->Select(entry_.id);
  }
  frames_ = 0;
  frame_gap_sum_ = 0;
  frame_gap_max_ = 0;
  expand_.AnimateTo(Compositor(), expanded ? 1 : 0,
                    MotionCurve::Of(expanded ? motion::kMediaDockExpand
                                             : motion::kMediaDockCollapse));
  // 收回去：最上面那一行回到主要的那个。
  Update();
}

void MediaDock::OnExpandFrame() {
  // 调试步骤看展开、收回时一帧一帧跟不跟得上。
  const base::TimeTicks now = base::TimeTicks::Now();
  if (frames_ > 0) {
    const double gap = (now - last_frame_).InMillisecondsF();
    frame_gap_sum_ += gap;
    frame_gap_max_ = std::max(frame_gap_max_, gap);
  }
  last_frame_ = now;
  ++frames_;
  ApplyFrame();
}

void MediaDock::ApplyFrame() {
  if (!rim_) {
    return;
  }
  const double e = expand_.value();
  if (e == 0 && !expand_.is_animating() && !resize_.is_animating()) {
    resize_.SetValue(0);
  }
  // 升起来的那一层：收着时往下沉到只露出最上面那一行（卡片）。弹簧冲过头时再往上一点，
  // 底下是那一层多出来的底。
  gfx::Transform transform;
  transform.Translate(
      0, static_cast<float>((1 - e) * std::max(0, height() - kCardHeight) +
                            resize_.value()));
  rider_->layer()->SetTransform(transform);

  const float shown = Clamp01(e);
  // 卡片独有的（播放暂停、进度波形、后面那一摞）先淡掉，播放器的再淡进来。
  const float card = 1 - std::min(1.0f, shown * 3);
  const float controls = Clamp01((shown - 0.2) / 0.5);
  toggle_->layer()->SetOpacity(card);
  toggle_->SetVisible(card > 0);
  wave_->layer()->SetOpacity(card);
  wave_->SetVisible(card > 0);
  stack_->layer()->SetOpacity(card);
  stack_->SetVisible(count_ > 1 && card > 0);
  collapse_->layer()->SetOpacity(controls);
  collapse_->SetVisible(shown > 0.01f);
  player_->layer()->SetOpacity(controls);
  player_->SetVisible(shown > 0.01f);
  shadow_->layer()->SetOpacity(Clamp01(shown * 2));
  shadow_->SetVisible(shown > 0.01f);
}

void MediaDock::Layout(PassKey) {
  const int w = width();
  const int h = height();
  stack_->SetBounds(0, h - kSlotHeight, w, kStackHeight + 24);
  clip_->SetBounds(0, -kSlack, w, h + kSlack);
  rider_->SetBounds(0, kSlack, w, h + kSlack);
  shadow_->SetBounds(0, -kShadowMargin, w, kShadowMargin + 2);
  sheet_->SetBounds(0, 0, w, h + kSlack);
  row_->SetBounds(0, 0, w, kCardHeight);
  const gfx::Rect accessory(w - kInset - kAccessory, kAccessoryTop, kAccessory,
                            kAccessory);
  toggle_->SetBoundsRect(accessory);
  collapse_->SetBoundsRect(accessory);
  wave_->SetBounds(kInset, kWaveTop, w - 2 * kInset, kWaveHeight);
  player_->SetBounds(kInset, kPlayerTop, w - 2 * kInset,
                     player_->ContentHeight());
  rim_->SetBounds(0, kSlack + h - kRim, w, kRim);
  ApplyFrame();
}

void MediaDock::PaintSheet(gfx::Canvas* canvas, const gfx::Size& size) {
  // 下面两个角在框外面，看得见的是上面两个角和两条侧边。
  const gfx::RectF panel(0, 0, size.width(), size.height());
  cc::PaintFlags base;
  base.setAntiAlias(true);
  base.setColor(theme_.surface_panel);
  canvas->DrawRoundRect(panel, kRadius, base);
  // 一层封面的颜色，从左往右淡掉。
  cc::PaintFlags wash;
  wash.setAntiAlias(true);
  const SkPoint points[2] = {gfx::PointFToSkPoint(panel.origin()),
                             gfx::PointFToSkPoint(panel.top_right())};
  const SkColor4f colors[2] = {
      SkColor4f::FromColor(SkColorSetA(media_theme_.primary_container,
                                       (sheet_playing_ ? 0.85f : 0.5f) * 255)),
      SkColor4f::FromColor(SkColorSetA(media_theme_.primary_container, 0))};
  wash.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr, 2,
                                                     SkTileMode::kClamp));
  canvas->DrawRoundRect(panel, kRadius, wash);
  cc::PaintFlags edge;
  edge.setAntiAlias(true);
  edge.setStyle(cc::PaintFlags::kStroke_Style);
  edge.setStrokeWidth(1);
  edge.setColor(theme_.OnSurface(0.08f));
  gfx::RectF stroke = panel;
  stroke.Inset(0.5f);
  canvas->DrawRoundRect(stroke, kRadius - 0.5f, edge);
}

void MediaDock::PaintRim(gfx::Canvas* canvas, const gfx::Size& size) {
  canvas->ClipRect(
      gfx::RectF(0, kRimSkip, size.width(), size.height() - kRimSkip));
  cc::PaintFlags edge;
  edge.setAntiAlias(true);
  edge.setStyle(cc::PaintFlags::kStroke_Style);
  edge.setStrokeWidth(1);
  edge.setColor(theme_.OnSurface(0.08f));
  gfx::RectF stroke(0, size.height() - 4 * kRadius, size.width(), 4 * kRadius);
  stroke.Inset(0.5f);
  canvas->DrawRoundRect(stroke, kRadius - 0.5f, edge);
}

void MediaDock::PaintShadow(gfx::Canvas* canvas, const gfx::Size& size) {
  // 只要面板上边线以上散出来的那一截；两头收进去一点，到面板的边上正好淡完。
  canvas->ClipRect(gfx::RectF(0, 0, size.width(), kShadowMargin));
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(SK_ColorBLACK);
  flags.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
      gfx::Vector2d(0, -2), 28,
      SkColorSetA(SK_ColorBLACK, (theme_.is_dark ? 0.42f : 0.22f) * 255))}));
  canvas->DrawRoundRect(
      gfx::RectF(kRadius, kShadowMargin + 4, size.width() - 2 * kRadius, 40),
      kRadius, flags);
}

void MediaDock::PaintStack(gfx::Canvas* canvas, const gfx::Size& size) {
  if (count_ < 2) {
    return;
  }
  const int w = size.width();
  const int layers = count_ > 2 ? 2 : 1;
  for (int i = layers; i >= 1; --i) {
    const float inset = w * 0.05f * i;
    const gfx::RectF peek(inset, kStackHeight - 4.0f * i, w - 2 * inset, 24);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(
        SkColorSetA(media_theme_.primary_container, (0.9f - 0.3f * i) * 255));
    canvas->DrawRoundRect(peek, 10, flags);
  }
}

void MediaDock::PaintRow(gfx::Canvas* canvas, const gfx::Size& size) {
  if (!has_entry_) {
    return;
  }
  const int w = size.width();
  const int art_width = entry_.has_video && w >= kWideCard ? kVideoArt : kArt;
  PaintArtwork(canvas, gfx::RectF(kInset, kArtTop, art_width, kArt), 7, entry_,
               media_theme_);
  const int x = kInset + art_width + kArtGap;
  const int right = w - kInset - kAccessory - 6;
  DrawMediaText(canvas, entry_.title, 13, /*semibold=*/true,
                entry_.playing ? theme_.on_surface : theme_.on_surface_variant,
                gfx::Rect(x, kArtTop + 1, right - x, 18));
  const std::u16string subtitle = entry_.artist.empty()
                                      ? entry_.source
                                      : entry_.artist + u" · " + entry_.source;
  DrawMediaText(canvas, subtitle, 11, false, theme_.on_surface_variant,
                gfx::Rect(x, kArtTop + 20, right - x, 15));
}

bool MediaDock::OnMousePressed(const ui::MouseEvent& event) {
  // 面板上的空白处：收下，不当成拖窗口。
  return true;
}

void MediaDock::OnMouseEntered(const ui::MouseEvent& event) {
  leave_timer_.Stop();
  if (!expanded_ && !hover_timer_.IsRunning()) {
    hover_timer_.Start(
        FROM_HERE, base::Seconds(motion::kMediaIslandHoverDelay),
        base::BindOnce(&MediaDock::HoverExpand, base::Unretained(this)));
  }
}

void MediaDock::OnMouseExited(const ui::MouseEvent& event) {
  hover_timer_.Stop();
  if (expanded_ && !leave_timer_.IsRunning()) {
    leave_timer_.Start(
        FROM_HERE, base::Seconds(motion::kMediaIslandLeaveDelay),
        base::BindOnce(&MediaDock::SetExpanded, base::Unretained(this), false));
  }
}

void MediaDock::HoverExpand() {
  if (toggle_->GetState() == views::Button::STATE_HOVERED ||
      toggle_->GetState() == views::Button::STATE_PRESSED) {
    // 正要点播放暂停：先不展开，挪开了再说。
    hover_timer_.Start(
        FROM_HERE, base::Seconds(motion::kMediaIslandHoverDelay),
        base::BindOnce(&MediaDock::HoverExpand, base::Unretained(this)));
    return;
  }
  SetExpanded(true);
}

void MediaDock::RemovedFromWidget() {
  hover_timer_.Stop();
  leave_timer_.Stop();
  expand_.SetValue(expanded_ ? 1 : 0);
  resize_.SetValue(0);
}

bool MediaDock::DoesIntersectRect(const views::View* target,
                                  const gfx::Rect& rect) const {
  // 视图一直是展开后那么大：只有面板露出来的部分接鼠标，上面空着的地方留给标签列表。
  return PanelRect().Intersects(rect);
}

std::string MediaDock::DebugState() const {
  return base::StrCat(
      {"e=", base::NumberToString(expand_.value()), " panel=",
       PanelRect().ToString(), " full=", base::NumberToString(full_height_),
       " max=", base::NumberToString(max_height_),
       " animating=", expand_.is_animating() ? "1" : "0",
       " frames=", base::NumberToString(frames_), " frame_ms_avg=",
       base::NumberToString(frames_ > 1 ? frame_gap_sum_ / (frames_ - 1) : 0),
       " frame_ms_max=", base::NumberToString(frame_gap_max_),
       " wave_fps=", base::NumberToString(player_->WavePaintRate())});
}

BEGIN_METADATA(MediaDock)
END_METADATA

}  // namespace starry
