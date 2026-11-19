// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/swipe/swipe_indicator.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/spring.h"
#include "third_party/skia/include/core/SkMatrix.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 圆泡：直径、静止（上膛）时离网页边多远、藏起来时伸出边外多少（连阴影一起藏住）。
constexpr int kBubble = 44;
constexpr int kBubbleInset = 14;
constexpr float kHiddenOverhang = 8;
// 上膛以后还能再拉出去多远（橡皮筋的极限）、多快接近极限。
constexpr float kRubberLimit = 22;
constexpr float kRubberStiffness = 0.9f;
// 没上膛时的缩放：拖得越远越接近 1。
constexpr float kRestScale = 0.72f;
// 进度环离圆泡边多远、多粗；箭头多大、朝翻页的方向倾出去多少。
constexpr float kRingInset = 4;
constexpr float kRingWidth = 2.5f;
constexpr int kGlyph = 18;
constexpr float kGlyphLean = 2.5f;
// 松手翻页时圆泡再往前冲多远。
constexpr float kLaunchDistance = 18;
// 网页边上的柔光：多宽、多高（最多网页高度的这么多）、最亮时的不透明度。
constexpr int kGlowWidth = 96;
constexpr int kGlowHeight = 440;
constexpr float kGlowHeightFraction = 0.7f;
constexpr float kGlowAlphaDark = 0.2f;
constexpr float kGlowAlphaLight = 0.12f;
// 历史列表：离圆泡多远、内边距、一行多高、宽度范围、离网页边多远、圆角。
constexpr int kListGap = 10;
constexpr int kListPadding = 6;
constexpr int kRowHeight = 34;
constexpr int kListMinWidth = 200;
constexpr int kListMaxWidth = 320;
constexpr int kListMargin = 12;
constexpr float kListRadius = 16;
constexpr float kHighlightRadius = 10;
constexpr int kRowLeading = 10;
constexpr int kRowSpacing = 10;
constexpr int kRowTrailing = 12;
constexpr int kRowFont = 13;
// 列表从圆泡那边放大、滑出来；退了膛时变淡。
constexpr float kListOpenScale = 0.9f;
constexpr float kListOpenShift = 14;
constexpr double kListFadeIn = 0.16;
constexpr float kListDisarmedOpacity = 0.55f;

SkColor WithAlpha(SkColor color, float alpha) {
  return SkColorSetA(
      color, static_cast<U8CPU>(std::lround(std::clamp(alpha, 0.0f, 1.0f) *
                                            SkColorGetA(color))));
}

}  // namespace

// ---- 圆泡上的东西：进度环、上膛时填满的主色、箭头 ----

class SwipeIndicator::BubbleFace : public views::View {
  METADATA_HEADER(BubbleFace, views::View)

 public:
  // 自己一层：进度环每帧都在变，重画时不动下面玻璃的图层。
  BubbleFace() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetTheme(const Theme& theme, bool forward) {
    theme_ = theme;
    forward_ = forward;
    SchedulePaint();
  }

  // revealed：圆泡拉出来了多少（0…1，进度环走多少）；armed：上膛的程度（0…1）。变化小到看不
  // 出来时不重画。
  void SetState(float revealed, float armed) {
    revealed = std::clamp(revealed, 0.0f, 1.0f);
    armed = std::clamp(armed, 0.0f, 1.0f);
    if (std::abs(revealed - revealed_) < 0.002f &&
        std::abs(armed - armed_) < 0.004f &&
        (revealed == 1) == (revealed_ == 1) && (armed == 0) == (armed_ == 0) &&
        (armed == 1) == (armed_ == 1)) {
      return;
    }
    revealed_ = revealed;
    armed_ = armed;
    SchedulePaint();
  }

  int paints() const { return paints_; }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    ++paints_;
    const gfx::RectF bounds(GetLocalBounds());
    const gfx::PointF center = bounds.CenterPoint();
    const float radius = std::min(bounds.width(), bounds.height()) / 2;
    const float ring = 1 - armed_;
    if (ring > 0) {
      const float r = radius - kRingInset;
      cc::PaintFlags track;
      track.setAntiAlias(true);
      track.setStyle(cc::PaintFlags::kStroke_Style);
      track.setStrokeWidth(kRingWidth);
      track.setColor(theme_.OnSurface((theme_.is_dark ? 0.16f : 0.12f) * ring));
      canvas->DrawCircle(center, r, track);
      if (revealed_ > 0) {
        // 从正上方开始，返回时顺时针、前进时逆时针（左右对称）。
        SkPathBuilder arc;
        arc.addArc(gfx::RectFToSkRect(gfx::RectF(center.x() - r, center.y() - r,
                                                 2 * r, 2 * r)),
                   -90, (forward_ ? -360 : 360) * revealed_);
        cc::PaintFlags stroke = track;
        stroke.setStrokeCap(cc::PaintFlags::kRound_Cap);
        stroke.setColor(WithAlpha(theme_.primary, ring));
        canvas->DrawPath(arc.detach(), stroke);
      }
    }
    if (armed_ > 0) {
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(WithAlpha(theme_.primary, armed_));
      canvas->DrawCircle(center, radius, fill);
    }
    const SkColor color =
        Theme::LerpColor(theme_.on_surface, theme_.on_primary, armed_);
    const float lean = (forward_ ? 1 : -1) * kGlyphLean * revealed_;
    canvas->Save();
    canvas->Translate(
        gfx::Vector2d(std::lround(center.x() - kGlyph / 2.0f + lean),
                      std::lround(center.y() - kGlyph / 2.0f)));
    gfx::PaintVectorIcon(canvas,
                         forward_ ? icons::kChevronRight : icons::kChevronLeft,
                         kGlyph, color);
    canvas->Restore();
  }

 private:
  Theme theme_ = Theme::DarkBase();
  bool forward_ = false;
  float revealed_ = 0;
  float armed_ = 0;
  int paints_ = 0;
};

BEGIN_METADATA(SwipeIndicator, BubbleFace)
END_METADATA

// ---- 网页边上的柔光 ----

// 贴着网页边缘的半个椭圆渐变：颜色只画一次，亮暗用图层的不透明度调（每一帧不重画）。
class SwipeIndicator::EdgeGlow : public views::View {
  METADATA_HEADER(EdgeGlow, views::View)

 public:
  EdgeGlow() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetCanProcessEventsWithinSubtree(false);
  }

  void Set(SkColor color, bool forward) {
    if (color_ == color && forward_ == forward) {
      return;
    }
    color_ = color;
    forward_ = forward;
    SchedulePaint();
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    if (bounds.IsEmpty()) {
      return;
    }
    const SkPoint center =
        SkPoint::Make(forward_ ? bounds.width() : 0, bounds.height() / 2);
    const float radius = bounds.width();
    SkColor4f inner = SkColor4f::FromColor(color_);
    SkColor4f outer = inner;
    outer.fA = 0;
    const SkColor4f colors[] = {inner, outer};
    // 圆形的渐变在竖直方向拉长成椭圆。
    SkMatrix matrix;
    matrix.setScale(1, bounds.height() / (2 * radius), center.x(), center.y());
    cc::PaintFlags flags;
    flags.setShader(cc::PaintShader::MakeRadialGradient(
        center, radius, colors, nullptr, std::size(colors), SkTileMode::kClamp,
        SkGradient::Interpolation(), 0, &matrix));
    canvas->DrawRect(bounds, flags);
  }

 private:
  SkColor color_ = SK_ColorTRANSPARENT;
  bool forward_ = false;
};

BEGIN_METADATA(SwipeIndicator, EdgeGlow)
END_METADATA

// ---- 历史列表 ----

class SwipeIndicator::HistoryList : public views::View {
  METADATA_HEADER(HistoryList, views::View)

 public:
  // 自己一层：高亮滑动时重画，不动下面玻璃的图层。
  HistoryList() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void SetRows(const std::vector<Row>& rows, int count) {
    rows_.assign(rows.begin(), rows.begin() + count);
    SchedulePaint();
  }

  // 高亮在第几行（0 起，滑动中在两行之间）；armed 是高亮从灰到主色（0…1，退了膛时变灰，
  // 松手就取消）。变化小到看不出来时不重画。
  void SetState(float position, float armed) {
    if (std::abs(position - position_) < 0.003f &&
        std::abs(armed - armed_) < 0.004f) {
      return;
    }
    position_ = position;
    armed_ = armed;
    SchedulePaint();
  }

  int paints() const { return paints_; }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    ++paints_;
    const float position = position_;
    const float armed = armed_;
    const int row_width = width() - 2 * kListPadding;
    cc::PaintFlags highlight;
    highlight.setAntiAlias(true);
    highlight.setColor(
        Theme::LerpColor(theme_.OnSurface(theme_.is_dark ? 0.14f : 0.1f),
                         theme_.primary, armed));
    canvas->DrawRoundRect(
        gfx::RectF(kListPadding, kListPadding + position * kRowHeight,
                   row_width, kRowHeight),
        kHighlightRadius, highlight);

    const int favicon = metrics::kFaviconSize;
    for (size_t i = 0; i < rows_.size(); ++i) {
      // 高亮滑过时文字的颜色跟着变（离高亮越近越接近主色上的颜色）。
      const float near = std::clamp(
          1 - std::abs(position - static_cast<float>(i)), 0.0f, 1.0f);
      const float on_highlight = near * armed;
      const SkColor text =
          Theme::LerpColor(theme_.on_surface, theme_.on_primary, on_highlight);
      const SkColor icon = Theme::LerpColor(theme_.on_surface_variant,
                                            theme_.on_primary, on_highlight);
      const int top = kListPadding + static_cast<int>(i) * kRowHeight;
      const int x = kListPadding + kRowLeading;
      const int icon_y = top + (kRowHeight - favicon) / 2;
      const gfx::ImageSkia image = rows_[i].favicon.IsEmpty()
                                       ? gfx::ImageSkia()
                                       : rows_[i].favicon.AsImageSkia();
      if (!image.isNull()) {
        canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), x,
                             icon_y, favicon, favicon, true);
      } else {
        canvas->Save();
        canvas->Translate(gfx::Vector2d(x, icon_y));
        gfx::PaintVectorIcon(canvas, icons::kGlobe, favicon, icon);
        canvas->Restore();
      }
      const int text_x = x + favicon + kRowSpacing;
      const gfx::Rect text_rect(
          text_x, top,
          std::max(0, kListPadding + row_width - kRowTrailing - text_x),
          kRowHeight);
      canvas->DrawStringRectWithFlags(
          rows_[i].title,
          UiFont(kRowFont, near > 0.5f ? gfx::Font::Weight::MEDIUM
                                       : gfx::Font::Weight::NORMAL),
          text, text_rect, gfx::Canvas::TEXT_ALIGN_LEFT);
    }
  }

 private:
  Theme theme_ = Theme::DarkBase();
  std::vector<Row> rows_;
  float position_ = 0;
  float armed_ = 1;
  int paints_ = 0;
};

BEGIN_METADATA(SwipeIndicator, HistoryList)
END_METADATA

// ---- 跟着帧走的数值 ----

namespace {

// 弹簧的位移小于这么多、速度小于它的 10 倍就算停下。
constexpr double kSettle = 0.001;
// 两帧之间最多按这么久走（主线程卡了一下之后不一步跳过去）。
constexpr double kMaxFrameStep = 1.0 / 20;

}  // namespace

SwipeIndicator::FrameValue::FrameValue(double value)
    : value_(value), target_(value) {}

SwipeIndicator::FrameValue::~FrameValue() = default;

void SwipeIndicator::FrameValue::Set(double value) {
  value_ = value;
  target_ = value;
  velocity_ = 0;
  moving_ = false;
  timed_ = false;
}

void SwipeIndicator::FrameValue::SpringTo(double target, const Spring& spring) {
  if (motion::ReduceMotion()) {
    Set(target);
    return;
  }
  if (timed_) {
    // 定长曲线中途换成弹簧：从现在的位置出发。
    timed_ = false;
    velocity_ = 0;
  }
  target_ = target;
  spring_ = spring;
  moving_ = value_ != target_ || velocity_ != 0;
}

void SwipeIndicator::FrameValue::CurveTo(double target,
                                         const MotionCurve& curve,
                                         base::TimeTicks start) {
  // 关键帧不管起点终点，停在最后一帧的值上。
  const double end = curve.kind() == MotionCurve::Kind::kKeyframes
                         ? curve.ValueAt(value_, target, 0, 1e6)
                         : target;
  if (motion::ReduceMotion()) {
    Set(end);
    return;
  }
  from_ = value_;
  target_ = end;
  velocity_ = 0;
  curve_ = curve;
  start_ = start;
  duration_ = curve.Duration(from_, target, 0, kSettle);
  timed_ = true;
  moving_ = duration_ > 0;
  if (!moving_) {
    Set(end);
  }
}

void SwipeIndicator::FrameValue::Step(base::TimeTicks now, double dt) {
  if (!moving_) {
    return;
  }
  if (timed_) {
    const double t = std::max(0.0, (now - start_).InSecondsF());
    if (t >= duration_) {
      Set(target_);
      return;
    }
    value_ = curve_.ValueAt(from_, target_, 0, t);
    return;
  }
  const SpringState state =
      SolveSpring(spring_, value_ - target_, velocity_, dt);
  value_ = target_ + state.displacement;
  velocity_ = state.velocity;
  if (std::abs(state.displacement) < kSettle &&
      std::abs(velocity_) < 10 * kSettle) {
    Set(target_);
  }
}

// ---- 指示器 ----

SwipeIndicator::SwipeIndicator() {
  SetCanProcessEventsWithinSubtree(false);
  page_ = AddChildView(std::make_unique<views::View>());
  page_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  page_->layer()->SetMasksToBounds(true);
  page_->SetVisible(false);
  glow_ = page_->AddChildView(std::make_unique<EdgeGlow>());
  bubble_ = page_->AddChildView(std::make_unique<GlassView>());
  face_ = bubble_->AddChildView(std::make_unique<BubbleFace>());
  list_ = page_->AddChildView(std::make_unique<GlassView>());
  list_->SetCornerRadius(kListRadius, true);
  list_rows_ = list_->AddChildView(std::make_unique<HistoryList>());
  list_->SetVisible(false);
  scale_.Set(kRestScale);
  SetTheme(theme_);
}

SwipeIndicator::~SwipeIndicator() {
  StopFrames();
}

void SwipeIndicator::SetTheme(const Theme& theme) {
  theme_ = theme;
  GlassStyle style;
  style.dark = theme.is_dark;
  style.luma = theme.is_dark ? 0.1f : 0.9f;
  bubble_->SetStyle(style);
  list_->SetStyle(style);
  glow_->Set(
      WithAlpha(theme.accent, theme.is_dark ? kGlowAlphaDark : kGlowAlphaLight),
      forward_);
  face_->SetTheme(theme, forward_);
  list_rows_->SetTheme(theme);
}

int SwipeIndicator::Begin(const gfx::Rect& page,
                          bool forward,
                          std::vector<Row> rows) {
  ++generation_;
  active_ = true;
  forward_ = forward;
  rows_ = std::move(rows);
  armed_ = false;
  list_open_ = false;
  selected_ = 0;
  progress_ = 0;
  frames_ = 0;
  frame_gap_sum_ = 0;
  frame_gap_max_ = 0;
  frame_gaps_ = 0;
  long_frames_ = 0;
  page_size_ = page.size();
  page_->SetBoundsRect(page);
  page_->SetVisible(true);
  bubble_motion_.SetOpacity(bubble_->layer(), 1);

  // 圆泡静止（上膛）时的位置：网页边上、竖直居中。
  rest_ = gfx::PointF(forward ? page.width() - kBubbleInset - kBubble / 2.0f
                              : kBubbleInset + kBubble / 2.0f,
                      std::round(page.height() / 2.0f));
  bubble_->SetGlassBounds(gfx::Rect(std::lround(rest_.x() - kBubble / 2.0f),
                                    std::lround(rest_.y() - kBubble / 2.0f),
                                    kBubble, kBubble));
  face_->SetBoundsRect(bubble_->glass_rect());
  face_->SetTheme(theme_, forward_);
  glow_->Set(WithAlpha(theme_.accent,
                       theme_.is_dark ? kGlowAlphaDark : kGlowAlphaLight),
             forward_);
  LayoutList();
  list_->SetVisible(false);

  travel_.Set(BubbleX(0) - rest_.x());
  lift_.Set(0);
  scale_.Set(kRestScale);
  pop_.Set(1);
  arm_.Set(0);
  glow_fade_.Set(1);
  list_highlight_.Set(0);
  list_armed_.Set(1);
  applied_pose_.reset();
  Apply();
  return shown_rows_;
}

void SwipeIndicator::Update(float progress,
                            bool armed,
                            bool list_open,
                            int selected) {
  if (!active_) {
    return;
  }
  progress_ = progress;
  const bool arm_changed = armed != armed_;
  armed_ = armed;
  if (arm_changed) {
    arm_.CurveTo(armed ? 1 : 0, MotionCurve::EaseOut(motion::kSwipeArmFade),
                 Clock());
    pop_.CurveTo(1, armed ? motion::SwipeArm() : motion::SwipeDisarm(),
                 Clock());
  }
  if (list_open && !list_open_ && shown_rows_ >= 2) {
    list_open_ = true;
    selected_ = std::clamp(selected, 0, shown_rows_ - 1);
    OpenList();
  } else if (list_open_) {
    selected_ = std::clamp(selected, 0, shown_rows_ - 1);
    list_highlight_.SpringTo(selected_, motion::kSwipeSelection);
    if (arm_changed) {
      const MotionCurve fade = MotionCurve::EaseOut(motion::kSwipeArmFade);
      list_armed_.CurveTo(armed ? 1 : 0, fade, Clock());
      list_motion_.AnimateOpacity(list_->layer(),
                                  armed ? 1 : kListDisarmedOpacity, fade);
    }
  }
  if (list_open_) {
    lift_.SpringTo(LiftFor(selected_), motion::kSwipeSelection);
  }
  travel_.SpringTo(BubbleX(progress) - rest_.x(), motion::kSwipeFollow);
  scale_.SpringTo(
      armed ? 1
            : kRestScale + (1 - kRestScale) * std::clamp(progress, 0.0f, 1.0f),
      motion::kSwipeFollow);
  StartFrames();
}

void SwipeIndicator::Finish(bool navigated) {
  if (!active_) {
    return;
  }
  active_ = false;
  const int generation = ++generation_;
  if (motion::ReduceMotion()) {
    Hide(generation);
    return;
  }
  auto hide = base::BindOnce(&SwipeIndicator::Hide, weak_factory_.GetWeakPtr(),
                             generation);
  // 淡出各自的图层，不动外面那一层（page_）的不透明度：它不透明时合成器把整层画到
  // 一张单独的画布上，实时玻璃只读得到这张空画布，留给阴影的边距会画成一个方块。
  if (navigated) {
    const MotionCurve fade = MotionCurve::EaseOut(motion::kSwipeCommitFade);
    const float inward = forward_ ? -1 : 1;
    travel_.SpringTo(travel_.target() + inward * kLaunchDistance,
                     motion::kSwipeLaunch);
    glow_fade_.CurveTo(0, fade, Clock());
    if (list_->GetVisible()) {
      list_motion_.AnimateOpacity(list_->layer(), 0, fade);
    }
    bubble_motion_.AnimateOpacity(bubble_->layer(), 0, fade, std::move(hide));
    StartFrames();
    return;
  }
  if (list_->GetVisible()) {
    list_motion_.AnimateOpacity(list_->layer(), 0, motion::DialogOut());
  }
  const MotionCurve fade = MotionCurve::EaseIn(motion::kSwipeCancelFade);
  arm_.CurveTo(0, MotionCurve::EaseOut(motion::kSwipeArmFade), Clock());
  travel_.SpringTo(BubbleX(0) - rest_.x(), motion::kSwipeRetract);
  scale_.SpringTo(kRestScale, motion::kSwipeRetract);
  glow_fade_.CurveTo(0, fade, Clock());
  bubble_motion_.AnimateOpacity(bubble_->layer(), 0, fade, std::move(hide));
  StartFrames();
}

void SwipeIndicator::Hide(int generation) {
  if (generation != generation_) {
    return;
  }
  page_->SetVisible(false);
  list_->SetVisible(false);
  StopFrames();
}

float SwipeIndicator::BubbleX(float progress) const {
  const float inward = forward_ ? -1 : 1;
  const float edge = forward_ ? page_size_.width() : 0;
  const float hidden = edge - inward * (kBubble / 2.0f + kHiddenOverhang);
  const float p = std::max(0.0f, progress);
  float x = hidden + (rest_.x() - hidden) * std::min(p, 1.0f);
  if (p > 1) {
    // 橡皮筋：越拉越拉不动，最多再出去 kRubberLimit。
    x += inward * kRubberLimit * (1 - 1 / (1 + (p - 1) * kRubberStiffness));
  }
  return x;
}

float SwipeIndicator::LiftFor(int selected) const {
  return list_bounds_.y() + kListPadding + (selected + 0.5f) * kRowHeight -
         rest_.y();
}

void SwipeIndicator::LayoutList() {
  shown_rows_ = 0;
  list_bounds_ = gfx::Rect();
  if (rows_.size() < 2) {
    return;
  }
  const int room = page_size_.height() - 2 * kListMargin - 2 * kListPadding;
  const int rows =
      std::min(static_cast<int>(rows_.size()), std::max(0, room / kRowHeight));
  const int beside = kBubbleInset + kBubble + kListGap;
  const int max_width =
      std::min(kListMaxWidth, page_size_.width() - beside - kListMargin);
  // 太矮、太窄（很小的分屏格子）时不展开，只能翻一页。
  if (rows < 2 || max_width < kListMinWidth * 3 / 4) {
    return;
  }
  shown_rows_ = rows;
  const gfx::FontList& font = UiFont(kRowFont, gfx::Font::Weight::MEDIUM);
  int text = 0;
  for (int i = 0; i < shown_rows_; ++i) {
    text = std::max(text, gfx::GetStringWidth(rows_[i].title, font));
  }
  const int natural = 2 * kListPadding + kRowLeading + metrics::kFaviconSize +
                      kRowSpacing + text + kRowTrailing;
  const int width =
      std::clamp(natural, std::min(kListMinWidth, max_width), max_width);
  const int height = 2 * kListPadding + shown_rows_ * kRowHeight;
  const int x = forward_ ? page_size_.width() - beside - width : beside;
  // 第一行对着圆泡；下面放不下时整张往上挪。
  int y = std::lround(rest_.y()) - kListPadding - kRowHeight / 2;
  y = std::clamp(
      y, kListMargin,
      std::max(kListMargin, page_size_.height() - kListMargin - height));
  list_bounds_ = gfx::Rect(x, y, width, height);
  list_->SetGlassBounds(list_bounds_);
  list_rows_->SetBoundsRect(list_->glass_rect());
  list_rows_->SetRows(rows_, shown_rows_);
}

void SwipeIndicator::OpenList() {
  list_->SetVisible(true);
  list_highlight_.Set(selected_);
  list_armed_.Set(armed_ ? 1 : 0);
  list_rows_->SetState(selected_, armed_ ? 1 : 0);
  const gfx::Rect rect = list_->glass_rect();
  // 从靠圆泡那一边、第一行的高度放大出来。
  const gfx::PointF origin(forward_ ? rect.right() : rect.x(),
                           rect.y() + kListPadding + kRowHeight / 2.0f);
  const float opacity = armed_ ? 1 : kListDisarmedOpacity;
  if (motion::ReduceMotion()) {
    list_motion_.SetTransform(list_->layer(), LayerPose(), origin);
    list_motion_.SetOpacity(list_->layer(), opacity);
    return;
  }
  LayerPose from;
  from.offset = gfx::Vector2dF((forward_ ? 1 : -1) * kListOpenShift, 0);
  from.scale = kListOpenScale;
  list_motion_.AnimateTransformFrom(list_->layer(), from, LayerPose(),
                                    MotionCurve::Of(motion::kSwipeList),
                                    origin);
  list_motion_.AnimateOpacityFrom(list_->layer(), 0, opacity,
                                  MotionCurve::EaseOut(kListFadeIn));
}

void SwipeIndicator::StartFrames() {
  if (!AnyMoving()) {
    Apply();
    return;
  }
  if (compositor_) {
    return;
  }
  views::Widget* widget = GetWidget();
  ui::Compositor* compositor = widget ? widget->GetCompositor() : nullptr;
  if (!compositor) {
    Apply();
    return;
  }
  compositor_ = compositor;
  last_frame_ = base::TimeTicks::Now();
  frame_seen_ = false;
  // 手指可以停在半路很久：不按「动画太久」检查。
  set_check_active_duration(false);
  compositor_->AddAnimationObserver(this);
}

void SwipeIndicator::StopFrames() {
  if (!compositor_) {
    return;
  }
  compositor_->RemoveAnimationObserver(this);
  compositor_ = nullptr;
}

bool SwipeIndicator::AnyMoving() const {
  for (const FrameValue* value :
       {&travel_, &lift_, &scale_, &pop_, &arm_, &glow_fade_, &list_highlight_,
        &list_armed_}) {
    if (value->moving()) {
      return true;
    }
  }
  return false;
}

base::TimeTicks SwipeIndicator::Clock() const {
  return compositor_ ? last_frame_ : base::TimeTicks::Now();
}

void SwipeIndicator::OnAnimationStep(base::TimeTicks timestamp) {
  // 第一帧的时刻可能比开始要帧时还早一点：那一帧不走。
  const double dt =
      std::clamp((timestamp - last_frame_).InSecondsF(), 0.0, kMaxFrameStep);
  if (frame_seen_) {
    const double gap = (timestamp - previous_frame_).InMillisecondsF();
    if (frame_gaps_ > 0 && gap > 1.5 * frame_gap_sum_ / frame_gaps_) {
      ++long_frames_;
    }
    frame_gap_sum_ += gap;
    frame_gap_max_ = std::max(frame_gap_max_, gap);
    ++frame_gaps_;
  }
  frame_seen_ = true;
  previous_frame_ = timestamp;
  last_frame_ = std::max(last_frame_, timestamp);
  ++frames_;
  for (FrameValue* value : {&travel_, &lift_, &scale_, &pop_, &arm_,
                            &glow_fade_, &list_highlight_, &list_armed_}) {
    value->Step(timestamp, dt);
  }
  Apply();
  if (!AnyMoving()) {
    StopFrames();
  }
}

void SwipeIndicator::OnCompositingShuttingDown(ui::Compositor* compositor) {
  StopFrames();
}

void SwipeIndicator::RemovedFromWidget() {
  StopFrames();
}

void SwipeIndicator::Apply() {
  if (!page_->GetVisible() || !bubble_->layer()) {
    return;
  }
  LayerPose pose;
  pose.offset = gfx::Vector2dF(travel_.value(), lift_.value());
  pose.scale = static_cast<float>(scale_.value() * pop_.value());
  if (applied_pose_ != pose) {
    bubble_->layer()->SetTransform(
        pose.ToTransform(gfx::PointF(bubble_->glass_rect().CenterPoint())));
    applied_pose_ = pose;
  }

  // 圆泡拉出来了多少（跟着弹簧，不是手指）：进度环、柔光都按它。
  const float hidden = BubbleX(0);
  const float span = rest_.x() - hidden;
  const float revealed =
      span == 0
          ? 0
          : std::clamp(
                (rest_.x() + static_cast<float>(travel_.value()) - hidden) /
                    span,
                0.0f, 1.0f);
  const float armed = static_cast<float>(arm_.value());
  face_->SetState(revealed, armed);

  const int glow_height = std::min(
      kGlowHeight, static_cast<int>(page_size_.height() * kGlowHeightFraction));
  const int glow_center = std::lround(rest_.y() + lift_.value());
  glow_->SetBounds(forward_ ? page_size_.width() - kGlowWidth : 0,
                   glow_center - glow_height / 2, kGlowWidth, glow_height);
  const float glow = std::clamp(0.5f * revealed + 0.5f * armed, 0.0f, 1.0f) *
                     static_cast<float>(glow_fade_.value());
  if (std::abs(glow_->layer()->opacity() - glow) > 0.002f ||
      (glow == 0) != (glow_->layer()->opacity() == 0)) {
    glow_->layer()->SetOpacity(glow);
  }

  if (list_->GetVisible()) {
    list_rows_->SetState(static_cast<float>(list_highlight_.value()),
                         static_cast<float>(list_armed_.value()));
  }
}

std::string SwipeIndicator::DebugDescription() const {
  const LayerPose pose{gfx::Vector2dF(travel_.value(), lift_.value()),
                       static_cast<float>(scale_.value() * pop_.value())};
  std::string text = base::StrCat(
      {"active=",
       active_ ? "1" : "0",
       " shown=",
       page_->GetVisible() ? "1" : "0",
       " forward=",
       forward_ ? "1" : "0",
       " page=",
       page_->bounds().ToString(),
       " bubble_center=",
       gfx::PointF(rest_.x() + pose.offset.x(), rest_.y() + pose.offset.y())
           .ToString(),
       " scale=",
       base::NumberToString(pose.scale),
       " armed=",
       base::NumberToString(arm_.value()),
       " glow=",
       base::NumberToString(glow_->layer()->opacity()),
       " opacity=",
       base::NumberToString(bubble_->layer()->opacity()),
       " frames=",
       base::NumberToString(frames_),
       " frame_ms_avg=",
       base::NumberToString(frame_gaps_ ? frame_gap_sum_ / frame_gaps_ : 0),
       " frame_ms_max=",
       base::NumberToString(frame_gap_max_),
       " long_frames=",
       base::NumberToString(long_frames_),
       " ticking=",
       compositor_ ? "1" : "0",
       " face_paints=",
       base::NumberToString(face_->paints()),
       " list_paints=",
       base::NumberToString(list_rows_->paints())});
  if (list_->GetVisible()) {
    base::StrAppend(
        &text, {" list=", list_bounds_.ToString(),
                " rows=", base::NumberToString(shown_rows_),
                " selected=", base::NumberToString(selected_),
                " highlight=", base::NumberToString(list_highlight_.value())});
    for (int i = 0; i < shown_rows_; ++i) {
      base::StrAppend(&text, {" [", base::UTF16ToUTF8(rows_[i].title), "]"});
    }
  }
  return text;
}

BEGIN_METADATA(SwipeIndicator)
END_METADATA

}  // namespace starry
