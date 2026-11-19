// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/value_motion.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace starry {

namespace {

// 主线程上的动画跟着 gfx::Animation 的定时器走。
constexpr int kFrameRate = 120;

}  // namespace

ValueMotion::ValueMotion(double value, base::RepeatingClosure on_step)
    : value_(value),
      to_(value),
      on_step_(std::move(on_step)),
      animation_(base::Seconds(1), kFrameRate, this) {}

ValueMotion::~ValueMotion() {
  animation_.set_delegate(nullptr);
  animation_.Stop();
}

void ValueMotion::AnimateTo(double target,
                            const MotionCurve& curve,
                            double epsilon) {
  if (target == to_ && (animation_.is_animating() || value_ == target)) {
    return;
  }
  const double velocity = animation_.is_animating() ? VelocityAt(Elapsed()) : 0;
  animation_.Stop();
  from_ = value_;
  to_ = target;
  velocity_ = velocity;
  curve_ = curve;
  const double seconds = motion::ReduceMotion()
                             ? 0
                             : curve.Duration(from_, to_, velocity_, epsilon);
  if (seconds <= 0) {
    value_ = to_;
    if (on_step_) {
      on_step_.Run();
    }
    return;
  }
  start_ = base::TimeTicks::Now();
  animation_.SetDuration(base::Seconds(seconds));
  animation_.Start();
}

void ValueMotion::SetValue(double value) {
  animation_.Stop();
  value_ = value;
  to_ = value;
}

void ValueMotion::AnimationProgressed(const gfx::Animation* animation) {
  value_ = curve_.ValueAt(from_, to_, velocity_, Elapsed());
  if (on_step_) {
    on_step_.Run();
  }
}

void ValueMotion::AnimationEnded(const gfx::Animation* animation) {
  // 关键帧停在最后一帧的值上（不管 to）。
  value_ = curve_.kind() == MotionCurve::Kind::kKeyframes
               ? curve_.ValueAt(from_, to_, velocity_, 1e6)
               : to_;
  if (on_step_) {
    on_step_.Run();
  }
}

void ValueMotion::AnimationCanceled(const gfx::Animation* animation) {}

double ValueMotion::Elapsed() const {
  return (base::TimeTicks::Now() - start_).InSecondsF();
}

double ValueMotion::VelocityAt(double t) const {
  constexpr double kDt = 1.0 / 240;
  return (curve_.ValueAt(from_, to_, velocity_, t + kDt) -
          curve_.ValueAt(from_, to_, velocity_, t)) /
         kDt;
}

}  // namespace starry
