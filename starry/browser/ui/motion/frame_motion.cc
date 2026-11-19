// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/frame_motion.h"

#include <algorithm>
#include <utility>

#include "ui/compositor/compositor.h"

namespace starry {

FrameMotion::FrameMotion(double value, base::RepeatingClosure on_step)
    : value_(value), to_(value), on_step_(std::move(on_step)) {}

FrameMotion::~FrameMotion() {
  Stop();
}

void FrameMotion::AnimateTo(ui::Compositor* compositor,
                            double target,
                            const MotionCurve& curve,
                            double epsilon) {
  if (target == to_ && (compositor_ || value_ == target)) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  const double velocity =
      compositor_ && curve.is_spring()
          ? VelocityAt(std::max(0.0, (now - start_).InSecondsF()))
          : 0;
  Stop();
  from_ = value_;
  to_ = target;
  velocity_ = velocity;
  curve_ = curve;
  duration_ = !compositor || motion::ReduceMotion()
                  ? 0
                  : curve.Duration(from_, to_, velocity_, epsilon);
  if (duration_ <= 0) {
    value_ = to_;
    if (on_step_) {
      on_step_.Run();
    }
    return;
  }
  start_ = now;
  compositor_ = compositor;
  compositor_->AddAnimationObserver(this);
}

void FrameMotion::SetValue(double value) {
  Stop();
  value_ = value;
  to_ = value;
}

void FrameMotion::Stop() {
  if (compositor_) {
    compositor_->RemoveAnimationObserver(this);
    compositor_ = nullptr;
  }
}

void FrameMotion::OnAnimationStep(base::TimeTicks timestamp) {
  // 第一帧的时刻可能比开始时还早一点：那一帧停在起点。
  const double seconds = std::max(0.0, (timestamp - start_).InSecondsF());
  if (seconds >= duration_) {
    Stop();
    value_ = to_;
  } else {
    value_ = curve_.ValueAt(from_, to_, velocity_, seconds);
  }
  if (on_step_) {
    on_step_.Run();
  }
}

void FrameMotion::OnCompositingShuttingDown(ui::Compositor* compositor) {
  Stop();
}

double FrameMotion::VelocityAt(double seconds) const {
  constexpr double kDt = 1.0 / 240;
  return (curve_.ValueAt(from_, to_, velocity_, seconds + kDt) -
          curve_.ValueAt(from_, to_, velocity_, seconds)) /
         kDt;
}

}  // namespace starry
