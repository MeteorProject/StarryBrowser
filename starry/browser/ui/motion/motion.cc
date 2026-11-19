// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/motion.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/check.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/geometry/cubic_bezier.h"

namespace starry {

MotionCurve::MotionCurve() = default;
MotionCurve::MotionCurve(const MotionCurve&) = default;
MotionCurve& MotionCurve::operator=(const MotionCurve&) = default;
MotionCurve::~MotionCurve() = default;

// static
MotionCurve MotionCurve::Of(const Spring& spring) {
  MotionCurve curve;
  curve.kind_ = Kind::kSpring;
  curve.spring_ = spring;
  return curve;
}

// static
MotionCurve MotionCurve::Timed(double seconds,
                               float x1,
                               float y1,
                               float x2,
                               float y2) {
  MotionCurve curve;
  curve.kind_ = Kind::kTimed;
  curve.seconds_ = std::max(seconds, 0.0);
  curve.x1_ = x1;
  curve.y1_ = y1;
  curve.x2_ = x2;
  curve.y2_ = y2;
  return curve;
}

// static
MotionCurve MotionCurve::EaseIn(double seconds) {
  return Timed(seconds, 0.42f, 0, 1, 1);
}

// static
MotionCurve MotionCurve::EaseOut(double seconds) {
  return Timed(seconds, 0, 0, 0.58f, 1);
}

// static
MotionCurve MotionCurve::EaseInOut(double seconds) {
  return Timed(seconds, 0.42f, 0, 0.58f, 1);
}

// static
MotionCurve MotionCurve::Linear(double seconds) {
  return Timed(seconds, 0, 0, 1, 1);
}

// static
MotionCurve MotionCurve::Keyframes(double seconds,
                                   std::vector<float> values,
                                   std::vector<float> key_times) {
  CHECK_EQ(values.size(), key_times.size());
  CHECK_GE(values.size(), 2u);
  MotionCurve curve;
  curve.kind_ = Kind::kKeyframes;
  curve.seconds_ = std::max(seconds, 0.0);
  curve.values_ = std::move(values);
  curve.key_times_ = std::move(key_times);
  // 每一段都是 easeInEaseOut。
  curve.x1_ = 0.42f;
  curve.y1_ = 0;
  curve.x2_ = 0.58f;
  curve.y2_ = 1;
  return curve;
}

double MotionCurve::ValueAt(double from,
                            double to,
                            double velocity,
                            double t) const {
  switch (kind_) {
    case Kind::kSpring:
      return to + SolveSpring(spring_, from - to, velocity, t).displacement;
    case Kind::kTimed: {
      if (seconds_ <= 0 || t >= seconds_) {
        return to;
      }
      if (t <= 0) {
        return from;
      }
      const double progress =
          gfx::CubicBezier(x1_, y1_, x2_, y2_).Solve(t / seconds_);
      return from + (to - from) * progress;
    }
    case Kind::kKeyframes: {
      if (seconds_ <= 0 || t >= seconds_) {
        return values_.back();
      }
      const double fraction = std::max(t, 0.0) / seconds_;
      size_t segment = 0;
      while (segment + 2 < key_times_.size() &&
             fraction >= key_times_[segment + 1]) {
        ++segment;
      }
      const double start = key_times_[segment];
      const double end = key_times_[segment + 1];
      const double local =
          end > start ? std::clamp((fraction - start) / (end - start), 0.0, 1.0)
                      : 1.0;
      const double progress =
          gfx::CubicBezier(x1_, y1_, x2_, y2_).Solve(local);
      return values_[segment] +
             (values_[segment + 1] - values_[segment]) * progress;
    }
  }
}

double MotionCurve::Duration(double from,
                             double to,
                             double velocity,
                             double epsilon) const {
  if (kind_ == Kind::kSpring) {
    return SpringSettleTime(spring_, from - to, velocity, epsilon);
  }
  return seconds_;
}

namespace motion {

MotionCurve Popover() {
  return MotionCurve::Timed(0.2, 0.16f, 1, 0.3f, 1);
}

MotionCurve Sidebar() {
  return MotionCurve::Timed(0.32, 0.2f, 0.8f, 0.2f, 1);
}

MotionCurve SidebarUnpeek() {
  return MotionCurve::Timed(0.15, 0.25f, 0.1f, 0.25f, 1);
}

MotionCurve Hover() {
  return MotionCurve::EaseOut(0.18);
}

MotionCurve Tab() {
  return MotionCurve::Timed(0.36, 0.16f, 1, 0.3f, 1);
}

MotionCurve Accent() {
  return MotionCurve::EaseInOut(0.8);
}

MotionCurve ChromeLayout() {
  return Sidebar();
}

MotionCurve Progress() {
  return MotionCurve::EaseOut(0.25);
}

MotionCurve AddressSwap() {
  return MotionCurve::EaseOut(0.12);
}

MotionCurve ChromeTint() {
  return Accent();
}

MotionCurve DialogBackdrop() {
  return MotionCurve::EaseOut(0.18);
}

MotionCurve DialogOut() {
  return MotionCurve::EaseOut(0.12);
}

MotionCurve TabPreviewHide() {
  return MotionCurve::EaseOut(0.12);
}

MotionCurve Press() {
  return MotionCurve::EaseOut(0.1);
}

MotionCurve TabClickBounce() {
  return MotionCurve::Keyframes(0.53, {1, 0.8f, 1.06f, 0.985f, 1.004f, 1},
                                {0, 0.15f, 0.42f, 0.64f, 0.84f, 1});
}

MotionCurve AgentDrag() {
  return MotionCurve::EaseInOut(kAgentDragTravel);
}

MotionCurve AgentPress() {
  return MotionCurve::Keyframes(0.38, {1, 0.8f, 1.03f, 0.995f, 1},
                                {0, 0.21f, 0.55f, 0.8f, 1});
}

MotionCurve SwipeArm() {
  return MotionCurve::Keyframes(0.34, {1, 1.14f, 0.98f, 1},
                                {0, 0.32f, 0.7f, 1});
}

MotionCurve SwipeDisarm() {
  return MotionCurve::Keyframes(0.2, {1, 0.92f, 1}, {0, 0.4f, 1});
}

bool ReduceMotion() {
  return gfx::Animation::PrefersReducedMotion();
}

}  // namespace motion

}  // namespace starry
