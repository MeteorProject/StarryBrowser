// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/agent_ring.h"

#include <utility>

#include "base/memory/raw_ptr.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/assistant/agent_parts.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_delegate.h"
#include "ui/compositor/layer_textured.h"
#include "ui/compositor/paint_recorder.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/transform.h"

namespace starry {

namespace {

// 线宽 1.6，等下一步时透明度 0.6。
constexpr float kStroke = 1.6f;
constexpr float kIdleOpacity = 0.6f;

}  // namespace

// 画渐变环的图层（挂在转动的 pivot_ 下面）。
class AgentRing::Painter : public ui::LayerDelegate {
 public:
  void SetLayer(ui::Layer* layer) { layer_ = layer; }

  // ui::LayerDelegate:
  void OnPaintLayer(const ui::PaintContext& context) override {
    const gfx::Size size = layer_->size();
    ui::PaintRecorder recorder(context, size);
    const gfx::PointF center(size.width() / 2.0f, size.height() / 2.0f);
    // 环在图标外 3.5（视图每边大 kOutset = 4），线画在这一圈里面。
    const float radius = size.width() / 2.0f - (kOutset - 3.5f) - kStroke / 2;
    cc::PaintFlags flags = AgentRingGradient(center, 0.35f);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(kStroke);
    recorder.canvas()->DrawCircle(center, radius, flags);
  }
  void OnDeviceScaleFactorChanged(float old_scale, float new_scale) override {}

 private:
  raw_ptr<ui::Layer> layer_ = nullptr;
};

// static
gfx::Rect AgentRing::BoundsAround(const gfx::Rect& icon) {
  gfx::Rect bounds = icon;
  bounds.Outset(kOutset);
  return bounds;
}

AgentRing::AgentRing() : painter_(std::make_unique<Painter>()) {
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
  pivot_ = ui::Layer::Create(ui::LAYER_NOT_DRAWN);
  ring_ = std::make_unique<ui::LayerTextured>();
  ring_->set_delegate(painter_.get());
  painter_->SetLayer(ring_.get());
  ring_->SetFillsBoundsOpaquely(false);
  pivot_->Add(ring_.get());
  layer()->Add(pivot_.get());
  SetVisible(false);
}

AgentRing::~AgentRing() {
  spin_.Stop();
  if (layer() && pivot_) {
    layer()->Remove(pivot_.get());
  }
}

void AgentRing::SetWorking(std::optional<bool> working) {
  if (working_ == working) {
    return;
  }
  working_ = working;
  SetVisible(working.has_value());
  if (!working) {
    Spin(false);
    return;
  }
  layer()->SetOpacity(*working ? 1 : kIdleOpacity);
  Spin(*working && !motion::ReduceMotion());
}

void AgentRing::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  LayoutRing();
}

void AgentRing::LayoutRing() {
  const gfx::Size size = this->size();
  pivot_->SetBounds(gfx::Rect(size.width() / 2, size.height() / 2, 1, 1));
  ring_->SetBounds(gfx::Rect(-size.width() / 2, -size.height() / 2,
                             size.width(), size.height()));
  ring_->SchedulePaint(gfx::Rect(size));
}

void AgentRing::Spin(bool spin) {
  if (spinning_ == spin) {
    return;
  }
  spinning_ = spin;
  spin_.Stop();
  pivot_->SetTransform(gfx::Transform());
  if (spin) {
    spin_.Spin(pivot_.get(), base::Seconds(motion::kAgentTabRingSpin));
  }
}

BEGIN_METADATA(AgentRing)
END_METADATA

}  // namespace starry
