// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_VALUE_MOTION_H_
#define STARRY_BROWSER_UI_MOTION_VALUE_MOTION_H_

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/linear_animation.h"

namespace starry {

// 主线程上的一个数值动画：每一帧都要重新排版或者重画的地方用（侧栏宽度、侧栏的布局切换、
// 下载进度圈）。只动图层的变换、不透明度时用 LayerMotion，那是合成器线程上的。
//
// 曲线为 MotionCurve（弹簧或贝塞尔）；中途变更目标时从当前值与速度连贯延续。
// 每一帧调用 on_step（值已经更新）。
class ValueMotion : public gfx::AnimationDelegate {
 public:
  ValueMotion(double value, base::RepeatingClosure on_step);
  ValueMotion(const ValueMotion&) = delete;
  ValueMotion& operator=(const ValueMotion&) = delete;
  ~ValueMotion() override;

  double value() const { return value_; }
  double target() const { return to_; }
  bool is_animating() const { return animation_.is_animating(); }

  // 动到 target。减弱动态效果时、曲线时长为零时直接跳过去（也调用 on_step）。
  // epsilon 是值的收敛阈值；0…1 的进度映射到长距离时应减小，避免提前停下。
  void AnimateTo(double target,
                 const MotionCurve& curve,
                 double epsilon = 0.01);
  // 直接设成 value，停掉动画（不调用 on_step）。
  void SetValue(double value);

  // gfx::AnimationDelegate:
  void AnimationProgressed(const gfx::Animation* animation) override;
  void AnimationEnded(const gfx::Animation* animation) override;
  void AnimationCanceled(const gfx::Animation* animation) override;

 private:
  double VelocityAt(double t) const;
  double Elapsed() const;

  double value_;
  double from_ = 0;
  double to_ = 0;
  double velocity_ = 0;
  MotionCurve curve_;
  base::TimeTicks start_;
  base::RepeatingClosure on_step_;
  gfx::LinearAnimation animation_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_VALUE_MOTION_H_
