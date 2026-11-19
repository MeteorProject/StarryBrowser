// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/layer_motion.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/animation/keyframe_model.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animation_delegate.h"
#include "ui/compositor/layer_animation_element.h"
#include "ui/compositor/layer_animation_sequence.h"
#include "ui/compositor/layer_animator.h"
#include "ui/compositor/layer_threaded_animation_delegate.h"
#include "ui/gfx/animation/keyframe/animation_curve.h"
#include "ui/gfx/geometry/transform_operations.h"

namespace starry {

namespace {

// 弹簧停下的判定：位移差不到 0.05 DIP、缩放差不到 0.0005、不透明度差不到 0.002。
constexpr double kOffsetEpsilon = 0.05;
constexpr double kScaleEpsilon = 0.0005;
constexpr double kOpacityEpsilon = 0.002;
// 用数值微分求速度的步长（秒）。
constexpr double kVelocityStep = 1.0 / 1000;

uintptr_t Id(const ui::Layer* layer) {
  return reinterpret_cast<uintptr_t>(layer);
}

double EpsilonFor(int components, int component) {
  if (components == 1) {
    return kOpacityEpsilon;
  }
  return component == 2 ? kScaleEpsilon : kOffsetEpsilon;
}

void ComputeDuration(MotionSpec& spec) {
  double seconds = 0;
  for (int i = 0; i < spec.components; ++i) {
    if (spec.curve.kind() != MotionCurve::Kind::kKeyframes &&
        spec.from[i] == spec.to[i] && spec.velocity[i] == 0) {
      continue;
    }
    seconds = std::max(
        seconds, spec.curve.Duration(spec.from[i], spec.to[i],
                                     spec.velocity[i],
                                     EpsilonFor(spec.components, i)));
  }
  if (spec.blur_from > 0) {
    seconds =
        std::max(seconds, spec.curve.Duration(spec.blur_from, 0, 0, 0.01));
  }
  spec.duration = base::Seconds(seconds);
}

// 合成器线程上的变换曲线：直接按 MotionSpec 算。
class TransformCurve : public gfx::TransformAnimationCurve {
 public:
  explicit TransformCurve(const MotionSpec& spec) : spec_(spec) {}

  base::TimeDelta Duration() const override { return spec_.duration; }

  std::unique_ptr<gfx::AnimationCurve> Clone() const override {
    return std::make_unique<TransformCurve>(spec_);
  }

  gfx::TransformOperations GetValue(base::TimeDelta t) const override {
    gfx::TransformOperations operations;
    operations.AppendMatrix(spec_.TransformAt(t.InSecondsF()));
    return operations;
  }

  gfx::TransformOperations GetTransformedValue(
      base::TimeDelta t,
      gfx::TimingFunction::LimitDirection) const override {
    return GetValue(t);
  }

  bool PreservesAxisAlignment() const override { return true; }

  bool MaximumScale(float* max_scale) const override {
    // 弹簧会冲过头：按 120 Hz 取样找最大的缩放。
    double scale = std::max(spec_.from[2], spec_.to[2]);
    const double seconds = spec_.duration.InSecondsF();
    for (double t = 0; t <= seconds; t += 1.0 / 120) {
      scale = std::max(scale, spec_.ValueAt(2, t));
    }
    *max_scale = static_cast<float>(scale);
    return scale > 0;
  }

 private:
  const MotionSpec spec_;
};

class OpacityCurve : public gfx::FloatAnimationCurve {
 public:
  explicit OpacityCurve(const MotionSpec& spec) : spec_(spec) {}

  base::TimeDelta Duration() const override { return spec_.duration; }

  std::unique_ptr<gfx::AnimationCurve> Clone() const override {
    return std::make_unique<OpacityCurve>(spec_);
  }

  float GetValue(base::TimeDelta t) const override {
    return std::clamp(static_cast<float>(spec_.ValueAt(0, t.InSecondsF())),
                      0.0f, 1.0f);
  }

  float GetTransformedValue(
      base::TimeDelta t,
      gfx::TimingFunction::LimitDirection) const override {
    return GetValue(t);
  }

 private:
  const MotionSpec spec_;
};

}  // namespace

// LayerPose / MotionSpec ----------------------------------------------------

gfx::Transform LayerPose::ToTransform(const gfx::PointF& origin) const {
  gfx::Transform transform;
  transform.Translate(offset.x() + origin.x(), offset.y() + origin.y());
  transform.Scale(scale, scale);
  transform.Translate(-origin.x(), -origin.y());
  return transform;
}

double MotionSpec::ValueAt(int component, double seconds) const {
  // 关键帧只管缩放（或不透明度），位移停在终点。
  if (curve.kind() == MotionCurve::Kind::kKeyframes && components == 3 &&
      component < 2) {
    return to[component];
  }
  if (seconds >= duration.InSecondsF()) {
    return curve.kind() == MotionCurve::Kind::kKeyframes
               ? curve.ValueAt(from[component], to[component], 0, seconds)
               : to[component];
  }
  return curve.ValueAt(from[component], to[component], velocity[component],
                       seconds);
}

double MotionSpec::VelocityAt(int component, double seconds) const {
  if (seconds >= duration.InSecondsF()) {
    return 0;
  }
  const double before = std::max(seconds - kVelocityStep, 0.0);
  const double after = seconds + kVelocityStep;
  return (ValueAt(component, after) - ValueAt(component, before)) /
         (after - before);
}

LayerPose MotionSpec::PoseAt(double seconds) const {
  LayerPose pose;
  pose.offset = gfx::Vector2dF(static_cast<float>(ValueAt(0, seconds)),
                               static_cast<float>(ValueAt(1, seconds)));
  pose.scale = static_cast<float>(ValueAt(2, seconds));
  return pose;
}

gfx::Transform MotionSpec::TransformAt(double seconds) const {
  return PoseAt(seconds).ToTransform(origin);
}

// LayerMotion::Element ------------------------------------------------------

// 一次动画。照着 ui/compositor 里 ThreadedTransformTransition 的做法：开始时把图层放到
// 起点，曲线交给合成器线程；主线程只看时间到没到，到了把图层放到终点。
class LayerMotion::Element : public ui::LayerAnimationElement {
 public:
  Element(Property property,
          const MotionSpec& spec,
          base::WeakPtr<LayerMotion> owner,
          int generation,
          base::OnceClosure done)
      : ui::LayerAnimationElement(property == Property::kTransform
                                      ? ui::LayerAnimationElement::TRANSFORM
                                      : ui::LayerAnimationElement::OPACITY,
                                  spec.duration),
        property_(property),
        spec_(spec),
        owner_(std::move(owner)),
        generation_(generation),
        done_(std::move(done)) {}

  Element(const Element&) = delete;
  Element& operator=(const Element&) = delete;
  ~Element() override = default;

  bool IsThreaded(ui::LayerAnimationDelegate* delegate) const override {
    return !duration().is_zero();
  }

 protected:
  std::string DebugName() const override { return "StarryLayerMotion"; }

  void OnStart(ui::LayerAnimationDelegate* delegate) override {
    Apply(delegate, 0);
  }

  bool OnProgress(double t, ui::LayerAnimationDelegate* delegate) override {
    if (!reported_start_ && effective_start_time() != base::TimeTicks()) {
      reported_start_ = true;
      if (owner_) {
        owner_->ElementStarted(property_, generation_, effective_start_time());
      }
    }
    if (t < 1.0) {
      // 变换在合成器线程上画；去模糊跟着同一个开始时间和 BeginFrame。
      return ApplyBlur(delegate, t * spec_.duration.InSecondsF());
    }
    if (Started() && IsThreaded(delegate)) {
      delegate->GetThreadedAnimationDelegate()->RemoveThreadedAnimation(
          keyframe_model_id());
    }
    Apply(delegate, spec_.duration.InSecondsF());
    Finish(/*completed=*/true);
    return true;
  }

  void OnGetTarget(TargetValue* target) const override {
    const double end = spec_.duration.InSecondsF();
    if (property_ == Property::kTransform) {
      target->transform = spec_.TransformAt(end);
    } else {
      target->opacity = std::clamp(static_cast<float>(spec_.ValueAt(0, end)),
                                   0.0f, 1.0f);
    }
  }

  void OnAbort(ui::LayerAnimationDelegate* delegate) override {
    if (delegate && Started() && IsThreaded(delegate)) {
      delegate->GetThreadedAnimationDelegate()->RemoveThreadedAnimation(
          keyframe_model_id());
      Apply(delegate,
            last_progressed_fraction() * spec_.duration.InSecondsF());
    }
    Finish(/*completed=*/false);
  }

  void RequestEffectiveStart(ui::LayerAnimationDelegate* delegate) override {
    if (!IsThreaded(delegate)) {
      set_effective_start_time(requested_start_time());
      return;
    }
    set_effective_start_time(base::TimeTicks());
    UpdateKeyframeModelId();
    std::unique_ptr<gfx::AnimationCurve> curve;
    cc::TargetProperty::Type target;
    if (property_ == Property::kTransform) {
      curve = std::make_unique<TransformCurve>(spec_);
      target = cc::TargetProperty::TRANSFORM;
    } else {
      curve = std::make_unique<OpacityCurve>(spec_);
      target = cc::TargetProperty::OPACITY;
    }
    std::unique_ptr<cc::KeyframeModel> model = cc::KeyframeModel::Create(
        std::move(curve), keyframe_model_id(), animation_group_id(),
        cc::KeyframeModel::TargetPropertyId(target));
    model->set_needs_synchronized_start_time(true);
    delegate->GetThreadedAnimationDelegate()->AddThreadedAnimation(
        std::move(model));
  }

 private:
  bool ApplyBlur(ui::LayerAnimationDelegate* delegate, double seconds) {
    if (spec_.blur_from <= 0) {
      return false;
    }
    // Layer 的模糊半径会随自身缩放变换一起缩放，先除以当前缩放比以抵消影响，
    // 保持父坐标系下的视觉模糊半径一致。
    const float scale = std::max(std::abs(spec_.PoseAt(seconds).scale), 0.001f);
    const float sigma =
        seconds >= spec_.duration.InSecondsF()
            ? 0
            : std::max(0.0, spec_.curve.ValueAt(spec_.blur_from, 0, 0, seconds)) /
                  scale;
    if (sigma == last_blur_) {
      return false;
    }
    last_blur_ = sigma;
    delegate->GetLayer()->SetLayerBlur(sigma);
    return true;
  }

  void Apply(ui::LayerAnimationDelegate* delegate, double seconds) {
    ApplyBlur(delegate, seconds);
    if (property_ == Property::kTransform) {
      delegate->SetTransformFromAnimation(
          spec_.TransformAt(seconds),
          ui::PropertyChangeReason::FROM_ANIMATION);
    } else {
      delegate->SetOpacityFromAnimation(
          std::clamp(static_cast<float>(spec_.ValueAt(0, seconds)), 0.0f,
                     1.0f),
          ui::PropertyChangeReason::FROM_ANIMATION);
    }
  }

  void Finish(bool completed) {
    if (finished_) {
      return;
    }
    finished_ = true;
    if (owner_) {
      owner_->ElementEnded(property_, generation_);
    }
    // 在图层动画器的这一步里不能执行别人的代码（可能删掉这个图层）。
    if (completed && done_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, std::move(done_));
    }
  }

  const Property property_;
  const MotionSpec spec_;
  base::WeakPtr<LayerMotion> owner_;
  const int generation_;
  base::OnceClosure done_;
  float last_blur_ = -1;
  bool reported_start_ = false;
  bool finished_ = false;
};

// LayerMotion ---------------------------------------------------------------

LayerMotion::Track::Track() = default;
LayerMotion::Track::Track(const Track&) = default;
LayerMotion::Track& LayerMotion::Track::operator=(const Track&) = default;
LayerMotion::Track::~Track() = default;

LayerMotion::LayerMotion() = default;
LayerMotion::~LayerMotion() = default;

void LayerMotion::AnimateTransform(ui::Layer* layer,
                                   const LayerPose& to,
                                   const MotionCurve& curve,
                                   const gfx::PointF& origin,
                                   base::OnceClosure done) {
  BindLayer(layer);
  MotionSpec spec;
  spec.curve = curve;
  spec.components = 3;
  spec.origin = origin;
  Sample(layer, Property::kTransform, origin, spec.from, spec.velocity);
  if (!curve.is_spring()) {
    spec.velocity = {};
  }
  spec.to = {to.offset.x(), to.offset.y(), to.scale};
  Start(layer, Property::kTransform, std::move(spec), std::move(done));
}

void LayerMotion::AnimateTransformWithVelocity(ui::Layer* layer,
                                               const LayerPose& to,
                                               const gfx::Vector2dF& velocity,
                                               const MotionCurve& curve,
                                               const gfx::PointF& origin,
                                               base::OnceClosure done) {
  BindLayer(layer);
  MotionSpec spec;
  spec.curve = curve;
  spec.components = 3;
  spec.origin = origin;
  Sample(layer, Property::kTransform, origin, spec.from, spec.velocity);
  if (curve.is_spring()) {
    spec.velocity[0] += velocity.x();
    spec.velocity[1] += velocity.y();
  } else {
    spec.velocity = {};
  }
  spec.to = {to.offset.x(), to.offset.y(), to.scale};
  Start(layer, Property::kTransform, std::move(spec), std::move(done));
}

void LayerMotion::AnimateTransformFrom(ui::Layer* layer,
                                       const LayerPose& from,
                                       const LayerPose& to,
                                       const MotionCurve& curve,
                                       const gfx::PointF& origin,
                                       base::OnceClosure done,
                                       float blur_from) {
  BindLayer(layer);
  MotionSpec spec;
  spec.curve = curve;
  spec.components = 3;
  spec.origin = origin;
  spec.blur_from = motion::ReduceMotion() ? 0 : blur_from;
  spec.from = {from.offset.x(), from.offset.y(), from.scale};
  spec.to = {to.offset.x(), to.offset.y(), to.scale};
  Start(layer, Property::kTransform, std::move(spec), std::move(done));
}

void LayerMotion::ShiftAndAnimate(ui::Layer* layer,
                                  const gfx::Vector2dF& shift,
                                  const LayerPose& to,
                                  const MotionCurve& curve,
                                  const gfx::PointF& origin) {
  BindLayer(layer);
  MotionSpec spec;
  spec.curve = curve;
  spec.components = 3;
  spec.origin = origin;
  Sample(layer, Property::kTransform, origin, spec.from, spec.velocity);
  if (!curve.is_spring()) {
    spec.velocity = {};
  }
  spec.from[0] += shift.x();
  spec.from[1] += shift.y();
  spec.to = {to.offset.x(), to.offset.y(), to.scale};
  Start(layer, Property::kTransform, std::move(spec), base::OnceClosure());
}

void LayerMotion::SetTransform(ui::Layer* layer,
                               const LayerPose& pose,
                               const gfx::PointF& origin) {
  BindLayer(layer);
  AnimateTransformFrom(layer, pose, pose, MotionCurve::Linear(0), origin);
}

void LayerMotion::AnimateOpacity(ui::Layer* layer,
                                 float to,
                                 const MotionCurve& curve,
                                 base::OnceClosure done) {
  BindLayer(layer);
  MotionSpec spec;
  spec.curve = curve;
  spec.components = 1;
  Sample(layer, Property::kOpacity, gfx::PointF(), spec.from, spec.velocity);
  if (!curve.is_spring()) {
    spec.velocity = {};
  }
  spec.to = {to, 0, 0};
  Start(layer, Property::kOpacity, std::move(spec), std::move(done));
}

void LayerMotion::AnimateOpacityFrom(ui::Layer* layer,
                                     float from,
                                     float to,
                                     const MotionCurve& curve,
                                     base::OnceClosure done) {
  BindLayer(layer);
  MotionSpec spec;
  spec.curve = curve;
  spec.components = 1;
  spec.from = {from, 0, 0};
  spec.to = {to, 0, 0};
  Start(layer, Property::kOpacity, std::move(spec), std::move(done));
}

void LayerMotion::SetOpacity(ui::Layer* layer, float opacity) {
  AnimateOpacityFrom(layer, opacity, opacity, MotionCurve::Linear(0));
}

LayerPose LayerMotion::CurrentPose(ui::Layer* layer) const {
  std::array<double, 3> values{};
  std::array<double, 3> velocities{};
  Sample(layer, Property::kTransform, transform_.spec.origin, values,
         velocities);
  LayerPose pose;
  pose.offset = gfx::Vector2dF(static_cast<float>(values[0]),
                               static_cast<float>(values[1]));
  pose.scale = static_cast<float>(values[2]);
  return pose;
}

float LayerMotion::CurrentOpacity(ui::Layer* layer) const {
  std::array<double, 3> values{};
  std::array<double, 3> velocities{};
  Sample(layer, Property::kOpacity, gfx::PointF(), values, velocities);
  return static_cast<float>(values[0]);
}

LayerPose LayerMotion::TargetPose(ui::Layer* layer) const {
  if (Id(layer) == layer_ && transform_.running) {
    return transform_.spec.PoseAt(transform_.spec.duration.InSecondsF());
  }
  return CurrentPose(layer);
}

void LayerMotion::BindLayer(ui::Layer* layer) {
  if (Id(layer) == layer_) {
    return;
  }
  layer_ = Id(layer);
  transform_ = Track();
  opacity_ = Track();
}

void LayerMotion::Start(ui::Layer* layer,
                        Property property,
                        MotionSpec spec,
                        base::OnceClosure done) {
  ComputeDuration(spec);
  Track& current = track(property);
  current.spec = spec;
  current.start_time = base::TimeTicks();
  current.generation = next_generation_++;
  current.running = true;

  ui::LayerAnimator* animator = layer->GetAnimator();
  // 打断正在放的动画时从它现在的值接着动（我们自己算好了起点和速度）。
  animator->set_preemption_strategy(
      ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
  auto element = std::make_unique<Element>(property, spec,
                                           weak_factory_.GetWeakPtr(),
                                           current.generation, std::move(done));
  animator->StartAnimation(
      new ui::LayerAnimationSequence(std::move(element)));
}

void LayerMotion::Sample(ui::Layer* layer,
                         Property property,
                         const gfx::PointF& origin,
                         std::array<double, 3>& values,
                         std::array<double, 3>& velocities) const {
  values = {};
  velocities = {};
  const Track& current =
      property == Property::kTransform ? transform_ : opacity_;
  if (Id(layer) == layer_ && current.running) {
    double seconds = 0;
    if (current.start_time != base::TimeTicks()) {
      seconds = (base::TimeTicks::Now() - current.start_time).InSecondsF();
    }
    for (int i = 0; i < current.spec.components; ++i) {
      values[i] = current.spec.ValueAt(i, seconds);
      velocities[i] = current.spec.VelocityAt(i, seconds);
    }
    if (property == Property::kOpacity) {
      values[0] = std::clamp(values[0], 0.0, 1.0);
    }
    return;
  }
  // 没在动：图层里的值就是现在的样子。
  if (property == Property::kOpacity) {
    values[0] = layer->GetTargetOpacity();
    return;
  }
  const gfx::Transform transform = layer->GetTargetTransform();
  const double scale = transform.rc(0, 0);
  values[0] = transform.rc(0, 3) - origin.x() * (1 - scale);
  values[1] = transform.rc(1, 3) - origin.y() * (1 - scale);
  values[2] = scale;
}

void LayerMotion::ElementStarted(Property property,
                                 int generation,
                                 base::TimeTicks time) {
  Track& current = track(property);
  if (current.generation == generation) {
    current.start_time = time;
  }
}

void LayerMotion::ElementEnded(Property property, int generation) {
  Track& current = track(property);
  if (current.generation == generation) {
    current.running = false;
  }
}

}  // namespace starry
