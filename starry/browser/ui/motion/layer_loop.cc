// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/layer_loop.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "base/check.h"
#include "cc/animation/animation_id_provider.h"
#include "cc/animation/keyframe_model.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animator.h"
#include "ui/compositor/layer_threaded_animation_delegate.h"
#include "ui/gfx/animation/keyframe/animation_curve.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/geometry/transform_operations.h"

namespace starry {

namespace {

constexpr int64_t kMicrosPerSecond = base::Time::kMicrosecondsPerSecond;

base::TimeDelta GridStep() {
  return base::Seconds(1) / LayerLoop::kLoopFps;
}

// 网格上的第几格（向下取整）。
int64_t GridFrames(base::TimeDelta t) {
  const int64_t micros = t.InMicroseconds();
  return micros <= 0 ? 0 : micros * LayerLoop::kLoopFps / kMicrosPerSecond;
}

// 取样的时刻：t 之前最近的一格。
double GridSeconds(base::TimeDelta t) {
  return static_cast<double>(GridFrames(t)) / LayerLoop::kLoopFps;
}

// 所有循环共用的时钟：开始时间放在网格上（不晚于现在），取样的格子就和别的循环对齐。
base::TimeTicks GridStart() {
  const int64_t frames = GridFrames(base::TimeTicks::Now() - base::TimeTicks());
  return base::TimeTicks() +
         base::Microseconds(frames * kMicrosPerSecond / LayerLoop::kLoopFps);
}

// 一圈（一个来回）的时长取成 0.1 秒（3 格）的整数倍：每一圈都从格子上开始。
// base::Seconds(2.8) 这样的浮点数会差 1 微秒，取整后是准的。
base::TimeDelta GridPeriod(base::TimeDelta period) {
  constexpr int64_t kUnit = kMicrosPerSecond / 10;
  const int64_t units =
      std::max<int64_t>(1, std::llround(period.InMicrosecondsF() / kUnit));
  DCHECK_LT((period - base::Microseconds(units * kUnit)).magnitude(),
            base::Milliseconds(1))
      << "LayerLoop period " << period << " is not a multiple of 0.1 s";
  return base::Microseconds(units * kUnit);
}

// 不透明度：from → to → from，每趟 easeInEaseOut。
class PulseCurve : public gfx::FloatAnimationCurve {
 public:
  PulseCurve(float from, float to, base::TimeDelta half)
      : from_(from), to_(to), half_(half) {}

  base::TimeDelta Duration() const override { return half_ * 2; }

  std::unique_ptr<gfx::AnimationCurve> Clone() const override {
    return std::make_unique<PulseCurve>(from_, to_, half_);
  }

  float GetValue(base::TimeDelta t) const override {
    const double half = half_.InSecondsF();
    const double phase = std::fmod(GridSeconds(t), 2 * half);
    const bool back = phase >= half;
    const double progress = gfx::Tween::CalculateValue(
        gfx::Tween::EASE_IN_OUT, (back ? phase - half : phase) / half);
    const float start = back ? to_ : from_;
    const float end = back ? from_ : to_;
    return static_cast<float>(start + (end - start) * progress);
  }

  float GetTransformedValue(
      base::TimeDelta t,
      gfx::TimingFunction::LimitDirection) const override {
    return GetValue(t);
  }

  base::TimeDelta TickInterval() const override { return GridStep(); }

 private:
  const float from_;
  const float to_;
  const base::TimeDelta half_;
};

// 变换：绕原点匀速顺时针转。
class SpinCurve : public gfx::TransformAnimationCurve {
 public:
  explicit SpinCurve(base::TimeDelta period) : period_(period) {}

  base::TimeDelta Duration() const override { return period_; }

  std::unique_ptr<gfx::AnimationCurve> Clone() const override {
    return std::make_unique<SpinCurve>(period_);
  }

  gfx::TransformOperations GetValue(base::TimeDelta t) const override {
    const double period = period_.InSecondsF();
    const double turn = std::fmod(GridSeconds(t), period) / period;
    gfx::TransformOperations operations;
    operations.AppendRotate(0, 0, 1, static_cast<float>(360 * turn));
    return operations;
  }

  gfx::TransformOperations GetTransformedValue(
      base::TimeDelta t,
      gfx::TimingFunction::LimitDirection) const override {
    return GetValue(t);
  }

  bool PreservesAxisAlignment() const override { return false; }

  bool MaximumScale(float* max_scale) const override {
    *max_scale = 1;
    return true;
  }

  base::TimeDelta TickInterval() const override { return GridStep(); }

 private:
  const base::TimeDelta period_;
};

}  // namespace

// static
double LayerLoop::GridPhaseMs(base::TimeTicks origin) {
  const int64_t micros = (origin - base::TimeTicks()).InMicroseconds();
  // 不早于 origin 的第一格（向上取整）。
  const int64_t frames =
      (micros * kLoopFps + kMicrosPerSecond - 1) / kMicrosPerSecond;
  const double boundary =
      static_cast<double>(frames) * kMicrosPerSecond / kLoopFps;
  return (boundary - static_cast<double>(micros)) /
         base::Time::kMicrosecondsPerMillisecond;
}

LayerLoop::LayerLoop() = default;

LayerLoop::~LayerLoop() {
  Stop();
}

void LayerLoop::Pulse(ui::Layer* layer,
                      float from,
                      float to,
                      base::TimeDelta half) {
  Start(layer, std::make_unique<PulseCurve>(from, to, GridPeriod(half * 2) / 2),
        cc::TargetProperty::OPACITY);
}

void LayerLoop::Spin(ui::Layer* layer, base::TimeDelta period) {
  Start(layer, std::make_unique<SpinCurve>(GridPeriod(period)),
        cc::TargetProperty::TRANSFORM);
}

void LayerLoop::Start(ui::Layer* layer,
                      std::unique_ptr<gfx::AnimationCurve> curve,
                      int target_property) {
  Stop();
  animator_ = layer->GetAnimator();
  keyframe_model_id_ = cc::AnimationIdProvider::NextKeyframeModelId();
  std::unique_ptr<cc::KeyframeModel> model = cc::KeyframeModel::Create(
      std::move(curve), keyframe_model_id_,
      cc::AnimationIdProvider::NextGroupId(),
      cc::KeyframeModel::TargetPropertyId(target_property));
  model->set_iterations(std::numeric_limits<double>::infinity());
  // 开始时间自己定（网格上），不等合成器的第一帧。
  model->set_needs_synchronized_start_time(false);
  model->set_start_time(GridStart());
  // 直接加到图层的 cc 动画上：ui::LayerAnimator 不知道它，不会每一帧在主线程上推进。
  static_cast<ui::LayerThreadedAnimationDelegate*>(animator_.get())
      ->AddThreadedAnimation(std::move(model));
}

void LayerLoop::Stop() {
  if (!keyframe_model_id_) {
    return;
  }
  static_cast<ui::LayerThreadedAnimationDelegate*>(animator_.get())
      ->RemoveThreadedAnimation(keyframe_model_id_);
  keyframe_model_id_ = 0;
  animator_ = nullptr;
}

}  // namespace starry
