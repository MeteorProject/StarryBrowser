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
#include "starry/browser/ui/swipe/haptics.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
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

// 卡片：左右留 10，封面 36（离上边 8），标题从封面右边 9 开始。
constexpr int kInset = 10;
constexpr int kArt = 36;
constexpr int kArtTop = 8;
constexpr int kArtGap = 9;
constexpr float kArtRadius = 7;
// 视频的封面是宽的；卡片不到这么宽时和音乐一样用方的，标题多留一点。
constexpr int kVideoArt = 58;
constexpr int kWideCard = 260;
// 卡片右边的播放暂停。
constexpr int kAccessory = 28;
constexpr int kAccessoryTop = kArtTop + (kArt - kAccessory) / 2;
// 卡片下面的波浪进度条（居中于封面底边 44 与卡片底边 62 之间）。
constexpr int kWaveTop = 48;
constexpr int kWaveHeight = 10;
// 展开后：四周留 14，封面 64，标题从封面右边 12 开始、比封面低 4。
constexpr int kPadding = 14;
constexpr int kFullArt = 64;
constexpr float kFullArtRadius = 12;
constexpr int kFullArtGap = 12;
constexpr int kFullTitleTop = kPadding + 4;
// 展开后右上角的浮出、收起。
constexpr int kCornerButton = 28;
constexpr int kCornerTop = 10;
constexpr int kCornerGap = 2;
// 播放器（从波形开始）在封面下面 12，到底留 12。
constexpr int kPlayerTop = kPadding + kFullArt + 12;
constexpr int kBottomPadding = 12;
// 圆角：卡片 12，展开后 18。
constexpr float kRadius = 12;
constexpr float kFullRadius = 18;
// 弹簧过冲或展开中少了一行时允许的额外范围。
constexpr int kSlack = 64;
// 展开后面板四周投影向外扩散的留白。
constexpr int kShadowMargin = 44;

float Clamp01(double v) {
  return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

// e 在 [from, to] 里走到了哪（0…1）。
float Segment(double e, double from, double to) {
  return Clamp01((e - from) / (to - from));
}

float Lerp(float from, float to, double t) {
  return from + (to - from) * static_cast<float>(t);
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

// 卡片（展开后是最上面封面和标题那一行）：点一下展开，再点一下收回。自己不画东西，封面、
// 标题是另外的几层。
class MediaDock::Row : public views::Button {
  METADATA_HEADER(Row, views::Button)

 public:
  explicit Row(MediaDock* dock)
      : views::Button(base::BindRepeating(
            [](MediaDock* dock) { dock->SetExpanded(!dock->expanded()); },
            base::Unretained(dock))) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }
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
  shadow_ = AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintShadow, base::Unretained(this))));
  shadow_->SetVisible(false);

  clip_ = AddChildView(std::make_unique<views::View>());
  clip_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  clip_->layer()->SetMasksToBounds(true);
  clip_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(kRadius));
  clip_->layer()->SetIsFastRoundedCorner(true);

  sheet_ = clip_->AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintSheet, base::Unretained(this))));
  row_ = clip_->AddChildView(std::make_unique<Row>(this));
  art_ = clip_->AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintArt, base::Unretained(this))));
  card_title_ = clip_->AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintCardTitle, base::Unretained(this))));
  full_title_ = clip_->AddChildView(std::make_unique<Piece>(
      base::BindRepeating(&MediaDock::PaintFullTitle, base::Unretained(this))));
  full_title_->SetVisible(false);
  toggle_ = clip_->AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating(
          [](MediaDock* dock) {
            if (dock->has_entry_) {
              dock->hub_->TogglePlayPause(dock->entry_.id);
            }
          },
          base::Unretained(this)),
      MediaGlyph::kPause, L("media.pause"), kAccessory, 12,
      MediaButton::Style::kTonal));
  wave_ = clip_->AddChildView(
      std::make_unique<MediaWaveform>(hub_, MediaWaveform::Style::kProgress));
  float_ = clip_->AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating(
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
          base::Unretained(this)),
      MediaGlyph::kFloat, L("media.float"), kCornerButton, 14));
  collapse_ = clip_->AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating([](MediaDock* dock) { dock->SetExpanded(false); },
                          base::Unretained(this)),
      MediaGlyph::kCollapse, L("media.collapse"), kCornerButton, 14));
  for (MediaButton* button : {float_.get(), collapse_.get()}) {
    button->layer()->SetOpacity(0);
    button->SetVisible(false);
  }

  MediaPlayerView::Options options;
  options.header = false;
  options.size_changed = base::BindRepeating(&MediaDock::OnPlayerSizeChanged,
                                             base::Unretained(this));
  // 点了其余的媒体里的一行：最上面那一行换成它。
  options.selection_changed =
      base::BindRepeating(&MediaDock::Update, base::Unretained(this));
  player_ =
      clip_->AddChildView(std::make_unique<MediaPlayerView>(hub_, options));
  player_->SetPaintToLayer();
  player_->layer()->SetFillsBoundsOpaquely(false);
  player_->SetVisible(false);
  full_height_ = FullHeight();

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
  const int art_width = CardArtWidth();
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
  for (MediaButton* button : {toggle_.get(), float_.get(), collapse_.get()}) {
    button->SetTheme(media_theme_);
  }
  if (has_entry_) {
    toggle_->SetGlyph(entry_.playing ? MediaGlyph::kPause : MediaGlyph::kPlay,
                      L(entry_.playing ? "media.pause" : "media.play"));
    wave_->Set(entry_, media_theme_.accent, theme_.danger(),
               theme_.OnSurface(0.2f));
    row_->GetViewAccessibility().SetName(
        L("media.accessibility",
          {{"title", entry_.title}, {"source", entry_.source}}));
    // 窄的时候标题放不全：停一下看全名。
    row_->SetTooltipText(entry_.artist.empty()
                             ? entry_.title
                             : entry_.title + u" · " + entry_.artist);
  }
  art_->SchedulePaint();
  card_title_->SchedulePaint();
  full_title_->SchedulePaint();
  if (CardArtWidth() != art_width) {
    // 视频和音乐的封面宽不一样，卡片上的标题跟着挪。
    InvalidateLayout();
  }
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

void MediaDock::SetWidths(int collapsed_width, int max_width) {
  collapsed_width = std::max(0, collapsed_width);
  max_width = std::max(0, max_width);
  if (collapsed_width_ == collapsed_width && max_width_ == max_width) {
    return;
  }
  if (collapsed_width_ != collapsed_width) {
    stack_->SchedulePaint();
  }
  collapsed_width_ = collapsed_width;
  max_width_ = max_width;
  InvalidateLayout();
}

int MediaDock::ExpandedWidth() const {
  const int width =
      max_width_ > 0 ? std::min(kExpandedWidth, max_width_) : kExpandedWidth;
  return std::max(collapsed_width_, width);
}

int MediaDock::CollapsedWidth() const {
  return collapsed_width_ > 0 ? std::min(collapsed_width_, width()) : width();
}

int MediaDock::CardArtWidth() const {
  return has_entry_ && entry_.has_video && CollapsedWidth() >= kWideCard
             ? kVideoArt
             : kArt;
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
    resize_.SetValue(std::max<double>(resize_.value() + delta * e, -kSlack));
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
  return gfx::Rect(0, height() - kCardHeight, CollapsedWidth(), kCardHeight);
}

gfx::Rect MediaDock::PanelRect() const {
  const int h = height();
  const int full_width = width();
  const int card_width = CollapsedWidth();
  const double e = expand_.value();
  const int travel = std::max(0, h - kCardHeight);
  const int top = std::clamp(
      static_cast<int>(std::lround((1 - e) * travel + resize_.value())),
      -kSlack, travel);
  const int w = std::clamp(
      static_cast<int>(std::lround(card_width + (full_width - card_width) * e)),
      std::min(card_width, full_width), full_width + kSlack);
  return gfx::Rect(0, top, std::max(1, w), std::max(1, h - top));
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
  if (expanded) {
    if (has_entry_) {
      player_->Select(entry_.id);
    }
    // Mac 上触控板轻振一下（手指在触控板上时才感觉得到）。
    PerformHaptic(Haptic::kLevelChange);
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
  if (!clip_ || width() <= 0 || height() <= 0) {
    return;
  }
  const double e = expand_.value();
  if (e == 0 && !expand_.is_animating() && !resize_.is_animating()) {
    resize_.SetValue(0);
  }
  const float shown = Clamp01(e);
  const gfx::Rect panel = PanelRect();
  const int w = panel.width();

  // 框：左下角不动，往上、往右长，圆角 12 → 18。底板跟着大小重画。
  const float radius = Lerp(kRadius, kFullRadius, shown);
  clip_->SetBoundsRect(panel);
  clip_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(radius));
  if (std::abs(radius - sheet_radius_) > 0.01f) {
    sheet_radius_ = radius;
    sheet_->SchedulePaint();
  }
  sheet_->SetBounds(0, 0, w, panel.height());

  // 封面：卡片上的位置 →
  // 展开后的位置，跟着大小重画（视频在宽卡片上从宽的变成方的）。
  const int card_art = CardArtWidth();
  const gfx::RectF art(Lerp(kInset, kPadding, e), Lerp(kArtTop, kPadding, e),
                       Lerp(card_art, kFullArt, e), Lerp(kArt, kFullArt, e));
  const float art_radius = Lerp(kArtRadius, kFullArtRadius, shown);
  if (std::abs(art_radius - art_radius_) > 0.01f) {
    art_radius_ = art_radius;
    art_->SchedulePaint();
  }
  art_->SetBoundsRect(gfx::ToRoundedRect(art));

  // 标题：两份一起从卡片上的位置平移到展开后的位置；卡片上的那份先淡掉，展开后的那份从
  // 13/15 放大到 1、随后淡进来。
  const int card_text_x = kInset + card_art + kArtGap;
  const int full_text_x = kPadding + kFullArt + kFullArtGap;
  const float text_x = Lerp(card_text_x, full_text_x, e);
  const float text_y = Lerp(kArtTop + 1, kFullTitleTop, e);
  gfx::Transform card_transform;
  card_transform.Translate(text_x, text_y);
  card_title_->layer()->SetTransform(card_transform);
  const float card_text = 1 - Segment(e, 0, 0.2);
  card_title_->layer()->SetOpacity(card_text);
  card_title_->SetVisible(card_text > 0);
  gfx::Transform full_transform;
  full_transform.Translate(text_x, text_y);
  const float scale = Lerp(13.0f / 15, 1, shown);
  full_transform.Scale(scale, scale);
  full_title_->layer()->SetTransform(full_transform);
  const float full_text = Segment(e, 0.18, 0.45);
  full_title_->layer()->SetOpacity(full_text);
  full_title_->SetVisible(full_text > 0);

  // 卡片才有的（播放暂停、进度波形、后面那一摞）先淡掉；播放暂停贴着右边缘走。
  const float card = 1 - Segment(e, 0, 0.3);
  toggle_->SetBounds(std::max(kInset, w - kInset - kAccessory), kAccessoryTop,
                     kAccessory, kAccessory);
  toggle_->layer()->SetOpacity(card);
  toggle_->SetVisible(card > 0);
  wave_->layer()->SetOpacity(card);
  wave_->SetVisible(card > 0);
  const float stack = 1 - Segment(e, 0, 0.25);
  stack_->layer()->SetOpacity(stack);
  stack_->SetVisible(count_ > 1 && stack > 0);

  // 右上角的浮出、收起后淡进来。
  const float corner = Segment(e, 0.4, 0.85);
  collapse_->SetBounds(w - kInset - kCornerButton, kCornerTop, kCornerButton,
                       kCornerButton);
  float_->SetBounds(collapse_->x() - kCornerGap - kCornerButton, kCornerTop,
                    kCornerButton, kCornerButton);
  for (MediaButton* button : {float_.get(), collapse_.get()}) {
    button->layer()->SetOpacity(corner);
    button->SetVisible(corner > 0.01f);
  }

  // 下面的波形、进度、控制和其余的媒体跟着上边升起来，从底边露出；中途淡入、上移
  // 8。
  const float body = Segment(e, 0.25, 0.7);
  player_->layer()->SetOpacity(body);
  gfx::Transform lift;
  lift.Translate(0, 8 * (1 - body));
  player_->layer()->SetTransform(lift);
  player_->SetVisible(shown > 0.01f);

  // 投影：按展开后的大小画的，以左下角为锚点缩放到面板的大小；越过侧栏盖到网页上时
  // 把面板和网页分开。
  const float shadow = Clamp01(e * 2);
  shadow_->layer()->SetOpacity(shadow);
  shadow_->SetVisible(shown > 0.01f);
  if (shadow_->GetVisible()) {
    const float sx = static_cast<float>(w) / width();
    const float sy = static_cast<float>(panel.height()) / height();
    gfx::Transform transform;
    transform.Translate(kShadowMargin, kShadowMargin + height());
    transform.Scale(sx, sy);
    transform.Translate(-kShadowMargin, -(kShadowMargin + height()));
    shadow_->layer()->SetTransform(transform);
  }
}

void MediaDock::Layout(PassKey) {
  const int w = width();
  const int h = height();
  const int card_width = CollapsedWidth();
  stack_->SetBounds(0, h - kSlotHeight, card_width, kStackHeight + 24);
  shadow_->SetBounds(-kShadowMargin, -kShadowMargin, w + 2 * kShadowMargin,
                     h + 2 * kShadowMargin);
  // 露出来多少按裁剪框算：收着时是整张卡片，展开后是播放器上面那一块。
  row_->SetBounds(0, 0, w, kPlayerTop - 2);
  const int card_text_x = kInset + CardArtWidth() + kArtGap;
  card_title_->SetBounds(
      0, 0, std::max(0, card_width - card_text_x - kInset - kAccessory - 6),
      40);
  const int full_text_x = kPadding + kFullArt + kFullArtGap;
  full_title_->SetBounds(0, 0,
                         std::max(0, w - full_text_x - kInset -
                                         2 * kCornerButton - kCornerGap - 4),
                         kFullArt);
  wave_->SetBounds(kInset, kWaveTop, std::max(0, card_width - 2 * kInset),
                   kWaveHeight);
  player_->SetBounds(kPadding, kPlayerTop, std::max(0, w - 2 * kPadding),
                     player_->ContentHeight());
  ApplyFrame();
}

void MediaDock::PaintSheet(gfx::Canvas* canvas, const gfx::Size& size) {
  const gfx::RectF panel(0, 0, size.width(), size.height());
  cc::PaintFlags base;
  base.setAntiAlias(true);
  base.setColor(theme_.surface_panel);
  canvas->DrawRoundRect(panel, sheet_radius_, base);
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
  canvas->DrawRoundRect(panel, sheet_radius_, wash);
  cc::PaintFlags edge;
  edge.setAntiAlias(true);
  edge.setStyle(cc::PaintFlags::kStroke_Style);
  edge.setStrokeWidth(1);
  edge.setColor(theme_.OnSurface(0.08f));
  gfx::RectF stroke = panel;
  stroke.Inset(0.5f);
  canvas->DrawRoundRect(stroke, sheet_radius_ - 0.5f, edge);
}

void MediaDock::PaintShadow(gfx::Canvas* canvas, const gfx::Size& size) {
  gfx::RectF panel(0, 0, size.width(), size.height());
  panel.Inset(kShadowMargin);
  if (panel.IsEmpty()) {
    return;
  }
  const SkRRect rrect =
      SkRRect::MakeRectXY(gfx::RectFToSkRect(panel), kFullRadius, kFullRadius);
  canvas->Save();
  canvas->sk_canvas()->clipRRect(rrect, SkClipOp::kDifference, true);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(SK_ColorBLACK);
  flags.setLooper(gfx::CreateShadowDrawLooper({
      gfx::ShadowValue(
          gfx::Vector2d(0, 6), 36,
          SkColorSetA(SK_ColorBLACK, (theme_.is_dark ? 0.48f : 0.22f) * 255)),
      gfx::ShadowValue(
          gfx::Vector2d(0, 2), 12,
          SkColorSetA(SK_ColorBLACK, (theme_.is_dark ? 0.28f : 0.1f) * 255)),
  }));
  canvas->sk_canvas()->drawRRect(rrect, flags);
  canvas->Restore();
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

void MediaDock::PaintArt(gfx::Canvas* canvas, const gfx::Size& size) {
  if (!has_entry_) {
    return;
  }
  PaintArtwork(canvas, gfx::RectF(0, 0, size.width(), size.height()),
               art_radius_, entry_, media_theme_);
}

void MediaDock::PaintCardTitle(gfx::Canvas* canvas, const gfx::Size& size) {
  if (!has_entry_ || size.width() <= 0) {
    return;
  }
  const std::u16string subtitle = entry_.artist.empty()
                                      ? entry_.source
                                      : entry_.artist + u" · " + entry_.source;
  DrawMediaText(canvas, entry_.title, 13, /*semibold=*/true,
                entry_.playing ? theme_.on_surface : theme_.on_surface_variant,
                gfx::Rect(0, 0, size.width(), 18));
  DrawMediaText(canvas, subtitle, 11, false, theme_.on_surface_variant,
                gfx::Rect(0, 19, size.width(), 15));
}

void MediaDock::PaintFullTitle(gfx::Canvas* canvas, const gfx::Size& size) {
  if (!has_entry_ || size.width() <= 0) {
    return;
  }
  // 同媒体岛展开后封面右边的三行。
  const int w = size.width();
  DrawMediaText(canvas, entry_.title, 15, /*semibold=*/true, theme_.on_surface,
                gfx::Rect(0, 0, w, 20));
  DrawMediaText(canvas, MediaSubtitle(entry_), 12, false,
                theme_.on_surface_variant, gfx::Rect(0, 22, w, 16));
  int source_x = 0;
  if (!entry_.favicon.isNull()) {
    canvas->DrawImageInt(entry_.favicon, 0, 0, entry_.favicon.width(),
                         entry_.favicon.height(), 0, 43, 12, 12, true);
    source_x = 17;
  }
  DrawMediaText(canvas, entry_.source, 11, false, theme_.on_surface_variant,
                gfx::Rect(source_x, 41, w - source_x, 15));
}

bool MediaDock::OnMousePressed(const ui::MouseEvent& event) {
  // 面板上的空白处：收下，不当成拖窗口。
  return true;
}

void MediaDock::OnMouseEntered(const ui::MouseEvent& event) {
  leave_timer_.Stop();
  exited_while_expanding_ = false;
  if (!expanded_ && !hover_timer_.IsRunning()) {
    hover_timer_.Start(
        FROM_HERE, base::Seconds(motion::kMediaIslandHoverDelay),
        base::BindOnce(&MediaDock::HoverExpand, base::Unretained(this)));
  }
}

void MediaDock::OnMouseExited(const ui::MouseEvent& event) {
  hover_timer_.Stop();
  if (expanded_ && !leave_timer_.IsRunning()) {
    exited_while_expanding_ = expand_.is_animating();
    leave_timer_.Start(
        FROM_HERE, base::Seconds(motion::kMediaIslandLeaveDelay),
        base::BindOnce(&MediaDock::LeaveCollapse, base::Unretained(this)));
  }
}

void MediaDock::LeaveCollapse() {
  const bool during_expand = exited_while_expanding_;
  exited_while_expanding_ = false;
  if (!expanded_) {
    return;
  }
  if (during_expand && GetWidget()) {
    // 面板还在长的时候鼠标出了原来的边，又落进了长大的面板：不收。
    gfx::Point point = display::Screen::Get()->GetCursorScreenPoint();
    views::View::ConvertPointFromScreen(this, &point);
    if (PanelRect().Contains(point)) {
      return;
    }
  }
  SetExpanded(false);
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
  if (!should_show_) {
    return false;
  }
  // 视图一直是展开后那么大：只有面板露出来的部分接鼠标，空着的地方留给标签列表和网页。
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
