// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/agent_marks.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "starry/browser/agent/agent_activity.h"
#include "starry/browser/ui/assistant/agent_parts.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/motion/layer_loop.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/text_utils.h"

namespace starry {

namespace {

// 描边：里面一圈宽 9、不透明度 0.14 的光晕，外面一圈 2.5 的渐变线，都画在卡片内侧，
// 和卡片同样的圆角。不用模糊：盖在网页上太费 GPU。
constexpr float kGlowWide = 9;
constexpr float kGlowWideOpacity = 0.14f;
constexpr float kGlowLine = 2.5f;
// 呼吸 0.5 到 1；不呼吸时做事 0.95、等下一步 0.55。
constexpr float kGlowLow = 0.5f;
constexpr float kGlowSteady = 0.95f;
constexpr float kGlowIdle = 0.55f;

// 扫描光带最高 160，不超过这一格的三分之一。
constexpr int kScanBand = 160;

// 高亮框：每边比元素大 4、至少 14，圆角 6；只画网页里看得见的那部分（外面留 12）。
constexpr float kHighlightOutset = 4;
constexpr float kHighlightMin = 14;
constexpr float kHighlightRadius = 6;
constexpr float kHighlightVisibleMargin = 12;

// 光标：箭头 18 × 22，尖在左上角；名字的胶囊在尖右下 (15, 19)，11 号半粗白字，
// 左右 7、上下 3；波纹 40、按住时的圆点 14，都以尖为圆心。
constexpr float kArrowWidth = 18;
constexpr float kArrowHeight = 22;
constexpr int kLabelX = 15;
constexpr int kLabelY = 19;
constexpr int kLabelPaddingX = 7;
constexpr int kLabelPaddingY = 3;
constexpr int kRipple = 40;
constexpr int kHoldDot = 14;
// 光标视图里尖的位置（波纹的半径）；形状外面给阴影、白边留的地方。
constexpr int kTip = kRipple / 2;
constexpr int kShadow = 4;
// 拖动时箭头压低到 0.86，尖上的圆点 0.55（从 0.4 放大）。
constexpr float kHoldScale = 0.86f;
constexpr float kHoldDotOpacity = 0.55f;
constexpr float kHoldDotHidden = 0.4f;
// 第一次出现的地方：底部提示条那里（离这一格的底边 56）。
constexpr int kCursorStartFromBottom = 56;
// 出现时从 0.6 放大（锚点是尖）。
constexpr float kCursorAppearScale = 0.6f;

constexpr double kFade = 0.2;

const gfx::FontList& LabelFont() {
  return UiFont(11, gfx::Font::Weight::SEMIBOLD);
}

// 不是有限数的坐标不画：NaN 传到图层上会出错。
bool Finite(const gfx::RectF& rect) {
  return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
         std::isfinite(rect.width()) && std::isfinite(rect.height());
}

gfx::Rect Lerp(const gfx::Rect& from, const gfx::Rect& to, double t) {
  const auto mix = [t](int a, int b) {
    return static_cast<int>(std::lround(a + (b - a) * t));
  };
  return gfx::Rect(mix(from.x(), to.x()), mix(from.y(), to.y()),
                   std::max(0, mix(from.width(), to.width())),
                   std::max(0, mix(from.height(), to.height())));
}

// 只是一个画着东西的图层（光标的各部分、扫描的光带）。
class PaintedLayerView : public views::View {
  METADATA_HEADER(PaintedLayerView, views::View)

 public:
  using Paint = base::RepeatingCallback<void(gfx::Canvas*, const gfx::Size&)>;
  explicit PaintedLayerView(Paint paint) : paint_(std::move(paint)) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  LayerMotion& motion() { return motion_; }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override { paint_.Run(canvas, size()); }

 private:
  Paint paint_;
  LayerMotion motion_;
};

BEGIN_METADATA(PaintedLayerView)
END_METADATA

}  // namespace

// ---- 描边 ----

class AgentEdgeGlow : public views::View {
  METADATA_HEADER(AgentEdgeGlow, views::View)

 public:
  AgentEdgeGlow() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  // 在做一步时呼吸（减弱动态效果时停在 0.95），等下一步时停在 0.55。
  void SetWorking(bool working) {
    const bool pulse = working && !motion::ReduceMotion();
    if (working_ == working && pulsing_ == pulse) {
      return;
    }
    working_ = working;
    pulsing_ = pulse;
    pulse_.Stop();
    if (!pulse) {
      layer()->SetOpacity(working ? kGlowSteady : kGlowIdle);
      return;
    }
    // 呼吸动画：0.5 和 1 之间来回，每趟 1.1 秒，缓入缓出。描边铺满整个卡片，
    // 每一帧都要重画这一格：用 LayerLoop（每秒 30 帧、不占主线程）。
    layer()->SetOpacity(kGlowLow);
    pulse_.Pulse(layer(), kGlowLow, 1.0f,
                 base::Seconds(motion::kAgentGlowPulse));
  }

  bool working() const { return working_.value_or(false); }
  bool pulsing() const { return pulsing_; }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    const gfx::PointF center = bounds.CenterPoint();
    for (const auto& [width, opacity] :
         {std::pair(kGlowWide, kGlowWideOpacity), std::pair(kGlowLine, 1.0f)}) {
      gfx::RectF rect = bounds;
      rect.Inset(width / 2);
      cc::PaintFlags flags = AgentRingGradient(center, 0.7f);
      flags.setStyle(cc::PaintFlags::kStroke_Style);
      flags.setStrokeWidth(width);
      flags.setAlphaf(opacity);
      canvas->DrawRoundRect(rect, std::max(0.0f, radius::kContent - width / 2),
                            flags);
    }
  }

 private:
  std::optional<bool> working_;
  bool pulsing_ = false;
  LayerLoop pulse_;
};

BEGIN_METADATA(AgentEdgeGlow)
END_METADATA

// ---- 扫描光带 ----

class AgentScanSweep : public views::View {
  METADATA_HEADER(AgentScanSweep, views::View)

 public:
  AgentScanSweep() {
    SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    SetCanProcessEventsWithinSubtree(false);
    band_ = AddChildView(std::make_unique<PaintedLayerView>(
        base::BindRepeating([](gfx::Canvas* canvas, const gfx::Size& size) {
          // 透明、淡天蓝、淡紫、透明。
          SkColor4f sky = SkColor4f::FromColor(kAgentSky);
          sky.fA = 0.16f;
          SkColor4f violet = SkColor4f::FromColor(kAgentViolet);
          violet.fA = 0.2f;
          const SkColor4f clear = SkColor4f::FromColor(SK_ColorTRANSPARENT);
          const SkColor4f colors[] = {clear, sky, violet, clear};
          const SkPoint points[2] = {SkPoint::Make(0, 0),
                                     SkPoint::Make(0, size.height())};
          cc::PaintFlags flags;
          flags.setShader(cc::PaintShader::MakeLinearGradient(
              points, colors, nullptr, std::size(colors), SkTileMode::kClamp));
          canvas->DrawRect(gfx::RectF(gfx::SizeF(size)), flags);
        })));
    band_->layer()->SetOpacity(0);
  }

  // 扫一次：从这一格上面滑到下面，最后四分之一淡出。减弱动态效果时不扫。
  void Sweep() {
    if (motion::ReduceMotion() || height() <= 0 || width() <= 0) {
      return;
    }
    ++sweeps_;
    const int band = std::min(kScanBand, height() / 3);
    band_->SetBounds(0, 0, width(), band);
    LayerPose from;
    from.offset = gfx::Vector2dF(0, -band);
    LayerPose to;
    to.offset = gfx::Vector2dF(0, height());
    band_->motion().AnimateTransformFrom(
        band_->layer(), from, to, MotionCurve::EaseInOut(motion::kAgentScan));
    band_->motion().AnimateOpacityFrom(
        band_->layer(), 1, 0,
        MotionCurve::Keyframes(motion::kAgentScan, {1, 1, 0}, {0, 0.75f, 1}));
  }

  int sweeps() const { return sweeps_; }
  bool sweeping() const { return band_->motion().IsAnimatingTransform(); }

 private:
  raw_ptr<PaintedLayerView> band_ = nullptr;
  int sweeps_ = 0;
};

BEGIN_METADATA(AgentScanSweep)
END_METADATA

// ---- 高亮框 ----

class AgentHighlightBox : public views::View {
  METADATA_HEADER(AgentHighlightBox, views::View)

 public:
  AgentHighlightBox()
      : frame_(0,
               base::BindRepeating(&AgentHighlightBox::Step,
                                   base::Unretained(this))) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
    SetVisible(false);
  }

  bool shown() const { return shown_; }
  const gfx::Rect& target() const { return to_; }

  void SetTyping(bool typing) {
    if (typing_ != typing) {
      typing_ = typing;
      SchedulePaint();
    }
  }

  // 出现：从 1.08 缩回来、淡入。
  void Appear(const gfx::Rect& rect, bool animate) {
    shown_ = true;
    ++generation_;
    frame_.SetValue(1);
    from_ = to_ = rect;
    SetBoundsRect(rect);
    SetVisible(true);
    const MotionCurve curve = MotionCurve::Of(motion::kAgentHighlight);
    const gfx::PointF center(rect.width() / 2.0f, rect.height() / 2.0f);
    if (!animate) {
      motion_.SetTransform(layer(), LayerPose());
      motion_.SetOpacity(layer(), 1);
      return;
    }
    if (!motion::ReduceMotion()) {
      LayerPose from;
      from.scale = 1.08f;
      motion_.AnimateTransformFrom(layer(), from, LayerPose(), curve, center);
    }
    motion_.AnimateOpacityFrom(layer(), motion_.CurrentOpacity(layer()), 1,
                               curve);
  }

  // 换到另一个元素：curve 为空时直接到位（只是这一格挪了），否则滑过去。
  void MoveTo(const gfx::Rect& rect, std::optional<MotionCurve> curve) {
    if (!curve) {
      // 正在滑的接着滑，终点跟着挪。
      const gfx::Vector2d shift = rect.origin() - to_.origin();
      from_.Offset(shift);
      to_ = rect;
      if (!frame_.is_animating()) {
        from_ = rect;
        SetBoundsRect(rect);
      }
      return;
    }
    from_ = bounds();
    to_ = rect;
    frame_.SetValue(0);
    frame_.AnimateTo(1, *curve);
  }

  // 收起：放大到 1.08 淡出，放完藏起来。
  void Hide() {
    if (!shown_) {
      return;
    }
    shown_ = false;
    const int generation = ++generation_;
    const MotionCurve curve = MotionCurve::Of(motion::kAgentHighlight);
    if (!motion::ReduceMotion()) {
      LayerPose to;
      to.scale = 1.08f;
      motion_.AnimateTransform(layer(), to, curve,
                               gfx::PointF(width() / 2.0f, height() / 2.0f));
    }
    motion_.AnimateOpacity(
        layer(), 0, curve,
        base::BindOnce(
            [](base::WeakPtr<AgentHighlightBox> box, int generation) {
              if (box && box->generation_ == generation) {
                box->SetVisible(false);
              }
            },
            weak_factory_.GetWeakPtr(), generation));
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(kAgentViolet, (typing_ ? 0.06f : 0.1f) * 255));
    canvas->DrawRoundRect(bounds, kHighlightRadius, fill);
    const float line = typing_ ? 2.5f : 2.0f;
    gfx::RectF stroke = bounds;
    stroke.Inset(line / 2);
    canvas->DrawRoundRect(stroke, kHighlightRadius - line / 2,
                          AgentGradientStroke(stroke, line));
  }

 private:
  void Step() { SetBoundsRect(Lerp(from_, to_, frame_.value())); }

  ValueMotion frame_;
  LayerMotion motion_;
  gfx::Rect from_;
  gfx::Rect to_;
  bool shown_ = false;
  bool typing_ = false;
  int generation_ = 0;
  base::WeakPtrFactory<AgentHighlightBox> weak_factory_{this};
};

BEGIN_METADATA(AgentHighlightBox)
END_METADATA

// ---- 光标 ----

class AgentCursor : public views::View {
  METADATA_HEADER(AgentCursor, views::View)

 public:
  AgentCursor() {
    // 外面这层只管挪位置；body_ 管出现、消失；里面每样东西各自一个图层（轻压、波纹）。
    SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    SetCanProcessEventsWithinSubtree(false);
    SetVisible(false);
    body_ = AddChildView(std::make_unique<views::View>());
    body_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    ripple_ = body_->AddChildView(std::make_unique<PaintedLayerView>(
        base::BindRepeating([](gfx::Canvas* canvas, const gfx::Size& size) {
          const gfx::RectF bounds{gfx::SizeF(size)};
          cc::PaintFlags fill;
          fill.setAntiAlias(true);
          fill.setColor(SkColorSetA(kAgentViolet, 0.12f * 255));
          canvas->DrawCircle(bounds.CenterPoint(), size.width() / 2.0f, fill);
          canvas->DrawCircle(bounds.CenterPoint(), size.width() / 2.0f - 1,
                             AgentGradientStroke(bounds, 2));
        })));
    ripple_->layer()->SetOpacity(0);
    dot_ = body_->AddChildView(std::make_unique<PaintedLayerView>(
        base::BindRepeating([](gfx::Canvas* canvas, const gfx::Size& size) {
          const gfx::RectF bounds{gfx::SizeF(size)};
          canvas->DrawCircle(bounds.CenterPoint(), size.width() / 2.0f,
                             AgentGradient(bounds));
        })));
    dot_->layer()->SetOpacity(0);
    arrow_ = body_->AddChildView(std::make_unique<PaintedLayerView>(
        base::BindRepeating(&AgentCursor::PaintArrow)));
    label_ = body_->AddChildView(std::make_unique<PaintedLayerView>(
        base::BindRepeating(&AgentCursor::PaintLabel, base::Unretained(this))));
    Relayout();
  }

  bool shown() const { return shown_; }
  // 尖在父视图里的位置（动画的终点）。
  gfx::Point tip() const { return origin() + gfx::Vector2d(kTip, kTip); }
  // 现在屏幕上尖在哪里（动画中途）。
  gfx::PointF CurrentTip() {
    return gfx::PointF(tip()) + motion_.CurrentPose(layer()).offset;
  }
  bool holding() const { return holding_; }

  void SetClient(const std::u16string& client) {
    if (client_ == client) {
      return;
    }
    client_ = client;
    Relayout();
    label_->SchedulePaint();
  }

  // 出现在 tip（父视图的坐标）：从 0.6 放大、淡入。
  void Appear(const gfx::Point& tip, bool animate) {
    shown_ = true;
    ++generation_;
    SetVisible(true);
    motion_.SetTransform(layer(), LayerPose());
    SetPosition(tip - gfx::Vector2d(kTip, kTip));
    const gfx::PointF anchor(kTip, kTip);
    if (!animate) {
      body_motion_.SetTransform(body_->layer(), LayerPose(), anchor);
      body_motion_.SetOpacity(body_->layer(), 1);
      return;
    }
    const MotionCurve curve = MotionCurve::Of(motion::kAgentHighlight);
    if (!motion::ReduceMotion()) {
      LayerPose from;
      from.scale = kCursorAppearScale;
      body_motion_.AnimateTransformFrom(body_->layer(), from, LayerPose(),
                                        curve, anchor);
    }
    body_motion_.AnimateOpacityFrom(
        body_->layer(), body_motion_.CurrentOpacity(body_->layer()), 1, curve);
  }

  // 挪到 tip：curve 为空时直接到位（这一格挪了，正在走的照样走完）。
  void MoveTo(const gfx::Point& tip, std::optional<MotionCurve> curve) {
    const gfx::Point old = origin();
    SetPosition(tip - gfx::Vector2d(kTip, kTip));
    if (curve && old != origin()) {
      motion_.ShiftAndAnimate(layer(), gfx::Vector2dF(old - origin()),
                              LayerPose(), *curve);
    }
  }

  // 收起：缩到 0.6 淡出，放完藏起来。
  void Disappear() {
    if (!shown_) {
      return;
    }
    shown_ = false;
    const int generation = ++generation_;
    const MotionCurve curve = MotionCurve::Of(motion::kAgentHighlight);
    if (!motion::ReduceMotion()) {
      LayerPose to;
      to.scale = kCursorAppearScale;
      body_motion_.AnimateTransform(body_->layer(), to, curve,
                                    gfx::PointF(kTip, kTip));
    }
    body_motion_.AnimateOpacity(
        body_->layer(), 0, curve,
        base::BindOnce(
            [](base::WeakPtr<AgentCursor> cursor, int generation) {
              if (cursor && cursor->generation_ == generation) {
                cursor->SetVisible(false);
              }
            },
            weak_factory_.GetWeakPtr(), generation));
  }

  // 点下：箭头轻压回弹（按住时不压），尖上一圈波纹从 0.3 扩散到 1.5、淡出。减弱动态效果
  // 时都没有。
  void Press() {
    ++presses_;
    if (motion::ReduceMotion()) {
      return;
    }
    const MotionCurve ripple = MotionCurve::EaseOut(motion::kAgentRipple);
    const gfx::PointF center(kRipple / 2.0f, kRipple / 2.0f);
    LayerPose small;
    small.scale = 0.3f;
    LayerPose large;
    large.scale = 1.5f;
    ripple_->motion().AnimateTransformFrom(ripple_->layer(), small, large,
                                           ripple, center);
    ripple_->motion().AnimateOpacityFrom(ripple_->layer(), 0.9f, 0, ripple);
    if (!holding_) {
      arrow_->motion().AnimateTransformFrom(arrow_->layer(), LayerPose(),
                                            LayerPose(), motion::AgentPress(),
                                            ArrowAnchor());
    }
  }

  // 按住拖动：箭头压低一点，尖上一个半透明的圆点。
  void SetHolding(bool holding) {
    if (holding_ == holding) {
      return;
    }
    holding_ = holding;
    const MotionCurve curve = MotionCurve::Of(motion::kAgentStatus);
    LayerPose arrow;
    arrow.scale = holding ? kHoldScale : 1;
    arrow_->motion().AnimateTransform(arrow_->layer(), arrow, curve,
                                      ArrowAnchor());
    LayerPose dot;
    dot.scale = holding ? 1 : kHoldDotHidden;
    dot_->motion().AnimateTransform(
        dot_->layer(), dot, curve,
        gfx::PointF(kHoldDot / 2.0f, kHoldDot / 2.0f));
    dot_->motion().AnimateOpacity(dot_->layer(), holding ? kHoldDotOpacity : 0,
                                  curve);
  }

  int presses() const { return presses_; }

 private:
  static gfx::PointF ArrowAnchor() { return gfx::PointF(kShadow, kShadow); }

  gfx::Size LabelSize() const {
    return gfx::Size(
        gfx::GetStringWidth(client_, LabelFont()) + 2 * kLabelPaddingX,
        LabelFont().GetHeight() + 2 * kLabelPaddingY);
  }

  void Relayout() {
    const gfx::Size label = LabelSize();
    SetSize(gfx::Size(kTip + kLabelX + label.width() + kShadow,
                      kTip + kLabelY + label.height() + kShadow));
    body_->SetBoundsRect(GetLocalBounds());
    ripple_->SetBounds(kTip - kRipple / 2, kTip - kRipple / 2, kRipple,
                       kRipple);
    dot_->SetBounds(kTip - kHoldDot / 2, kTip - kHoldDot / 2, kHoldDot,
                    kHoldDot);
    arrow_->SetBounds(kTip - kShadow, kTip - kShadow,
                      static_cast<int>(kArrowWidth) + 2 * kShadow,
                      static_cast<int>(kArrowHeight) + 2 * kShadow);
    label_->SetBounds(kTip + kLabelX - kShadow, kTip + kLabelY - kShadow,
                      label.width() + 2 * kShadow,
                      label.height() + 2 * kShadow);
    // 按住时的圆点一开始是缩小的。
    if (!holding_) {
      LayerPose dot;
      dot.scale = kHoldDotHidden;
      dot_->motion().SetTransform(
          dot_->layer(), dot, gfx::PointF(kHoldDot / 2.0f, kHoldDot / 2.0f));
    }
  }

  // 箭头：尖在左上角，渐变填充、1.5 的白边（圆角连接），淡淡的阴影。
  static void PaintArrow(gfx::Canvas* canvas, const gfx::Size& size) {
    const float x = kShadow;
    const float y = kShadow;
    const float w = kArrowWidth;
    const float h = kArrowHeight;
    SkPathBuilder builder;
    builder.moveTo(x, y);
    builder.lineTo(x + w, y + h * 0.62f);
    builder.lineTo(x + w * 0.52f, y + h * 0.7f);
    builder.lineTo(x + w * 0.3f, y + h);
    builder.close();
    const SkPath path = builder.detach();
    const gfx::RectF bounds(x, y, w, h);
    cc::PaintFlags fill = AgentGradient(bounds);
    fill.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
        gfx::Vector2d(0, 1), 4, SkColorSetA(SK_ColorBLACK, 0.25f * 255))}));
    canvas->DrawPath(path, fill);
    cc::PaintFlags stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(cc::PaintFlags::kStroke_Style);
    stroke.setStrokeWidth(1.5f);
    stroke.setStrokeJoin(cc::PaintFlags::kRound_Join);
    stroke.setColor(SK_ColorWHITE);
    canvas->DrawPath(path, stroke);
  }

  // 名字的胶囊：渐变的底、白字，淡淡的阴影。
  void PaintLabel(gfx::Canvas* canvas, const gfx::Size& size) {
    gfx::RectF capsule{gfx::SizeF(size)};
    capsule.Inset(kShadow);
    cc::PaintFlags fill = AgentGradient(capsule);
    fill.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
        gfx::Vector2d(0, 1), 4, SkColorSetA(SK_ColorBLACK, 0.2f * 255))}));
    canvas->DrawRoundRect(capsule, capsule.height() / 2, fill);
    canvas->DrawStringRectWithFlags(
        client_, LabelFont(), SK_ColorWHITE, gfx::ToEnclosingRect(capsule),
        gfx::Canvas::NO_SUBPIXEL_RENDERING | gfx::Canvas::TEXT_ALIGN_CENTER);
  }

  std::u16string client_;
  raw_ptr<views::View> body_ = nullptr;
  raw_ptr<PaintedLayerView> ripple_ = nullptr;
  raw_ptr<PaintedLayerView> dot_ = nullptr;
  raw_ptr<PaintedLayerView> arrow_ = nullptr;
  raw_ptr<PaintedLayerView> label_ = nullptr;
  LayerMotion motion_;
  LayerMotion body_motion_;
  bool shown_ = false;
  bool holding_ = false;
  int generation_ = 0;
  int presses_ = 0;
  base::WeakPtrFactory<AgentCursor> weak_factory_{this};
};

BEGIN_METADATA(AgentCursor)
END_METADATA

// ---- 一格 ----

AgentPaneMarks::AgentPaneMarks() {
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetCanProcessEventsWithinSubtree(false);
  // 内容卡片的圆角裁掉外面的（光带、高亮框、光标的名字不画出这一格）。
  layer()->SetMasksToBounds(true);
  layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(radius::kContent));
  layer()->SetIsFastRoundedCorner(true);
  scan_ = AddChildView(std::make_unique<AgentScanSweep>());
  highlight_ = AddChildView(std::make_unique<AgentHighlightBox>());
  cursor_ = AddChildView(std::make_unique<AgentCursor>());
  glow_ = AddChildView(std::make_unique<AgentEdgeGlow>());
}

AgentPaneMarks::~AgentPaneMarks() = default;

gfx::PointF AgentPaneMarks::ToLocal(const gfx::PointF& page) const {
  return gfx::PointF(page_origin_.x() + page.x() * zoom_,
                     page_origin_.y() + page.y() * zoom_);
}

gfx::RectF AgentPaneMarks::ToLocal(const gfx::RectF& page) const {
  return gfx::RectF(ToLocal(page.origin()),
                    gfx::SizeF(page.width() * zoom_, page.height() * zoom_));
}

void AgentPaneMarks::Update(const agent::AgentActivity& activity,
                            const gfx::Rect& card,
                            const gfx::PointF& page_origin,
                            float zoom) {
  if (leaving_) {
    return;
  }
  const bool first = !placed_;
  placed_ = true;
  SetBoundsRect(card);
  page_origin_ = page_origin;
  zoom_ = zoom;
  const gfx::Rect local(card.size());
  if (first) {
    // 出现时淡入。
    motion_.AnimateOpacityFrom(layer(), 0, 1, MotionCurve::EaseOut(kFade));
  }
  glow_->SetBoundsRect(local);
  glow_->SetWorking(activity.working());
  scan_->SetBoundsRect(local);
  if (!first && activity.scans != scans_) {
    scan_->Sweep();
  }
  scans_ = activity.scans;

  const bool reduce = motion::ReduceMotion();
  // 拖动开始：光标和高亮框这一趟按拖动的曲线滑到目标。
  const bool drag_started = activity.holding && !holding_;
  const MotionCurve drag = motion::AgentDrag();

  // 高亮框：只框看得见的那部分（特别大的元素不画出几万点高的图层）。
  std::optional<gfx::Rect> box;
  if (activity.target && Finite(*activity.target)) {
    gfx::RectF visible = ToLocal(*activity.target);
    gfx::RectF area(local);
    area.Outset(kHighlightVisibleMargin);
    visible.Intersect(area);
    if (!visible.IsEmpty()) {
      const float w =
          std::max(visible.width() + 2 * kHighlightOutset, kHighlightMin);
      const float h =
          std::max(visible.height() + 2 * kHighlightOutset, kHighlightMin);
      box = gfx::ToEnclosingRect(gfx::RectF(visible.CenterPoint().x() - w / 2,
                                            visible.CenterPoint().y() - h / 2,
                                            w, h));
    }
  }
  if (!box) {
    highlight_->Hide();
  } else {
    highlight_->SetTyping(activity.typing);
    if (!highlight_->shown()) {
      highlight_->Appear(*box, !first);
    } else if (activity.target != target_page_ && !first && !reduce) {
      highlight_->MoveTo(
          *box, drag_started ? drag : MotionCurve::Of(motion::kAgentHighlight));
    } else {
      highlight_->MoveTo(*box, std::nullopt);
    }
  }
  target_page_ = activity.target;

  // 光标：尖不出这一格。
  cursor_->SetClient(activity.client);
  if (!activity.cursor || !std::isfinite(activity.cursor->x()) ||
      !std::isfinite(activity.cursor->y())) {
    cursor_->Disappear();
  } else {
    const gfx::PointF point = ToLocal(*activity.cursor);
    const gfx::Point tip(
        std::clamp(static_cast<int>(std::lround(point.x())), 0, local.width()),
        std::clamp(static_cast<int>(std::lround(point.y())), 0,
                   local.height()));
    if (!cursor_->shown()) {
      if (!first && !reduce) {
        // 第一次出现：从底部提示条那里出发。
        cursor_->Appear(
            gfx::Point(local.width() / 2,
                       std::max(0, local.height() - kCursorStartFromBottom)),
            /*animate=*/true);
        cursor_->MoveTo(tip, MotionCurve::Of(motion::kAgentCursor));
      } else {
        cursor_->Appear(tip, /*animate=*/!first);
      }
    } else if (activity.cursor != cursor_page_ && !first && !reduce) {
      cursor_->MoveTo(
          tip, drag_started ? drag : MotionCurve::Of(motion::kAgentCursor));
    } else {
      cursor_->MoveTo(tip, std::nullopt);
    }
  }
  cursor_page_ = activity.cursor;
  if (!first && activity.clicks != clicks_ && activity.cursor) {
    cursor_->Press();
  }
  clicks_ = activity.clicks;
  cursor_->SetHolding(activity.holding);
  holding_ = activity.holding;
}

void AgentPaneMarks::Leave(base::OnceClosure done) {
  if (leaving_) {
    return;
  }
  leaving_ = true;
  motion_.AnimateOpacity(
      layer(), 0, MotionCurve::EaseOut(kFade),
      base::BindOnce(
          [](base::OnceClosure done) {
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, std::move(done));
          },
          std::move(done)));
}

std::string AgentPaneMarks::DebugDescription() const {
  std::string text =
      base::StrCat({"glow=", glow_->working() ? "working" : "idle",
                    glow_->pulsing() ? "(pulse)" : ""});
  if (cursor_->shown()) {
    const gfx::PointF now = cursor_->CurrentTip();
    base::StrAppend(&text, {" cursor=", cursor_->tip().ToString(),
                            " now=", base::NumberToString(std::lround(now.x())),
                            ",", base::NumberToString(std::lround(now.y())),
                            cursor_->holding() ? " holding" : "", " presses=",
                            base::NumberToString(cursor_->presses())});
  } else {
    text += " cursor=none";
  }
  base::StrAppend(&text, {" highlight=", highlight_->shown()
                                             ? highlight_->target().ToString()
                                             : std::string("none")});
  base::StrAppend(&text, {" scans=", base::NumberToString(scan_->sweeps()),
                          scan_->sweeping() ? "(sweeping)" : ""});
  return text;
}

BEGIN_METADATA(AgentPaneMarks)
END_METADATA

}  // namespace starry
