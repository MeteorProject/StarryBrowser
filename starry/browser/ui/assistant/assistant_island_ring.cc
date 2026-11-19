// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/assistant_island_ring.h"

#include <cmath>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_textured.h"
#include "ui/compositor/paint_recorder.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"

namespace starry {

namespace {

// 智能体的颜色。
constexpr SkColor kViolet = SkColorSetRGB(0x7C, 0x5C, 0xFF);
constexpr SkColor kSky = SkColorSetRGB(0x38, 0xBD, 0xF8);
constexpr float kStroke = 1.5f;
constexpr float kFocusOpacity = 0.75f;
// 一圈 2.8 秒。
constexpr base::TimeDelta kTurn = base::Milliseconds(2800);

gfx::RectF StrokeRect(const gfx::Size& size) {
  gfx::RectF rect{gfx::SizeF(size)};
  rect.Inset(kStroke / 2);
  return rect;
}

}  // namespace

// 画在一个独立图层上的内容（遮罩、转动的渐变）。
class AssistantIslandRing::Painter : public ui::LayerDelegate {
 public:
  using Paint = base::RepeatingCallback<void(gfx::Canvas*, const gfx::Size&)>;
  explicit Painter(Paint paint) : paint_(std::move(paint)) {}

  void SetLayer(ui::Layer* layer) { layer_ = layer; }

  // ui::LayerDelegate:
  void OnPaintLayer(const ui::PaintContext& context) override {
    const gfx::Size size = layer_->size();
    ui::PaintRecorder recorder(context, size);
    paint_.Run(recorder.canvas(), size);
  }
  void OnDeviceScaleFactorChanged(float old_scale, float new_scale) override {}

 private:
  Paint paint_;
  raw_ptr<ui::Layer> layer_ = nullptr;
};

AssistantIslandRing::AssistantIslandRing() {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
  mask_painter_ = std::make_unique<Painter>(base::BindRepeating(
      [](AssistantIslandRing* ring, gfx::Canvas* canvas,
         const gfx::Size& size) {
        cc::PaintFlags flags;
        flags.setAntiAlias(true);
        flags.setStyle(cc::PaintFlags::kStroke_Style);
        flags.setStrokeWidth(kStroke);
        flags.setColor(SK_ColorBLACK);
        canvas->DrawRoundRect(StrokeRect(size), ring->radius_ - kStroke / 2,
                              flags);
      },
      base::Unretained(this)));
  sweep_painter_ = std::make_unique<Painter>(
      base::BindRepeating([](gfx::Canvas* canvas, const gfx::Size& size) {
        // 锥形渐变：一段紫到天蓝，其余透明（色标：0、0.18 透明，0.40 紫，
        // 0.60 天蓝，0.82、1 透明）。
        const SkColor4f clear = SkColor4f::FromColor(SK_ColorTRANSPARENT);
        const SkColor4f violet = SkColor4f::FromColor(kViolet);
        const SkColor4f sky = SkColor4f::FromColor(kSky);
        const SkColor4f colors[] = {clear, clear, violet, sky, clear, clear};
        const SkScalar positions[] = {0, 0.18f, 0.40f, 0.60f, 0.82f, 1};
        cc::PaintFlags flags;
        flags.setAntiAlias(true);
        flags.setShader(cc::PaintShader::MakeSweepGradient(
            size.width() / 2.0f, size.height() / 2.0f, colors, positions,
            std::size(colors), SkTileMode::kClamp, 0, 360));
        canvas->DrawRect(gfx::RectF(gfx::SizeF(size)), flags);
      }));
}

AssistantIslandRing::~AssistantIslandRing() {
  spin_.Stop();
  if (layer() && mask_) {
    layer()->SetMaskLayer(nullptr);
  }
}

void AssistantIslandRing::SetMode(Mode mode) {
  // 减弱动态效果：做事时也是静止的那种渐变。
  if (mode == Mode::kBusy && motion::ReduceMotion()) {
    mode = Mode::kFocus;
  }
  if (mode_ == mode) {
    return;
  }
  mode_ = mode;
  UpdateBusy();
  SchedulePaint();
}

void AssistantIslandRing::SetRadius(float radius) {
  if (radius_ == radius) {
    return;
  }
  radius_ = radius;
  SchedulePaint();
  if (mask_) {
    mask_->SchedulePaint(gfx::Rect(mask_->size()));
  }
}

void AssistantIslandRing::OnPaint(gfx::Canvas* canvas) {
  if (mode_ != Mode::kFocus) {
    return;
  }
  const gfx::RectF rect = StrokeRect(size());
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setStyle(cc::PaintFlags::kStroke_Style);
  flags.setStrokeWidth(kStroke);
  flags.setAlphaf(kFocusOpacity);
  const SkPoint points[2] = {gfx::PointFToSkPoint(rect.bottom_left()),
                             gfx::PointFToSkPoint(rect.top_right())};
  const SkColor4f colors[2] = {SkColor4f::FromColor(kViolet),
                               SkColor4f::FromColor(kSky)};
  flags.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr,
                                                      2, SkTileMode::kClamp));
  canvas->DrawRoundRect(rect, radius_ - kStroke / 2, flags);
}

void AssistantIslandRing::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  if (mask_) {
    LayoutBusy();
  }
}

void AssistantIslandRing::UpdateBusy() {
  const bool busy = mode_ == Mode::kBusy;
  if (busy == static_cast<bool>(mask_)) {
    return;
  }
  if (!busy) {
    spin_.Stop();
    layer()->SetMaskLayer(nullptr);
    layer()->Remove(pivot_.get());
    sweep_.reset();
    pivot_.reset();
    mask_.reset();
    return;
  }
  mask_ = std::make_unique<ui::LayerTextured>();
  mask_->set_delegate(mask_painter_.get());
  mask_painter_->SetLayer(mask_.get());
  mask_->SetFillsBoundsOpaquely(false);
  pivot_ = ui::Layer::Create(ui::LAYER_NOT_DRAWN);
  sweep_ = std::make_unique<ui::LayerTextured>();
  sweep_->set_delegate(sweep_painter_.get());
  sweep_painter_->SetLayer(sweep_.get());
  sweep_->SetFillsBoundsOpaquely(false);
  pivot_->Add(sweep_.get());
  layer()->Add(pivot_.get());
  layer()->SetMaskLayer(mask_.get());
  LayoutBusy();
  // 一直匀速转（合成器上，不占主线程）。
  spin_.Spin(pivot_.get(), kTurn);
}

void AssistantIslandRing::LayoutBusy() {
  const gfx::Size size = this->size();
  mask_->SetBounds(gfx::Rect(size));
  mask_->SchedulePaint(gfx::Rect(size));
  // 方块的边长是对角线：转到哪个角度都盖得住整个圈。
  const int side =
      static_cast<int>(std::ceil(std::hypot(size.width(), size.height())));
  pivot_->SetBounds(gfx::Rect(size.width() / 2, size.height() / 2, 1, 1));
  sweep_->SetBounds(gfx::Rect(-side / 2, -side / 2, side, side));
  sweep_->SchedulePaint(gfx::Rect(side, side));
}

BEGIN_METADATA(AssistantIslandRing)
END_METADATA

}  // namespace starry
