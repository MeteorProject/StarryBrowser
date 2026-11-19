// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_FRAME_MOTION_H_
#define STARRY_BROWSER_UI_MOTION_FRAME_MOTION_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/compositor/compositor_animation_observer.h"

namespace ui {
class Compositor;
}

namespace starry {

// 主线程上跟着屏幕刷新走的一个数值动画：合成器要画每一帧之前算一次（不是
// ValueMotion
// 那样自己的定时器，和刷新对不齐时有的帧走两步、有的不走）。每一帧只改图层
// 的变换、不透明度，又要主线程上随时知道现在的值（点击落在哪）时用它；LayerMotion
// 的值在动画放完之前主线程上是起点的。
//
// 曲线是 MotionCurve；中途换终点时从当时的值和速度接着动。每一帧调用
// on_step（值已经 更新），停下的那一帧也调用。
class FrameMotion : public ui::CompositorAnimationObserver {
 public:
  FrameMotion(double value, base::RepeatingClosure on_step);
  FrameMotion(const FrameMotion&) = delete;
  FrameMotion& operator=(const FrameMotion&) = delete;
  ~FrameMotion() override;

  double value() const { return value_; }
  double target() const { return to_; }
  bool is_animating() const { return compositor_ != nullptr; }

  // 动到 target。compositor
  // 是视图所在窗口的（Widget::GetCompositor）；没有、减弱动态
  // 效果、曲线时长为零时直接跳过去（也调用 on_step）。
  void AnimateTo(ui::Compositor* compositor,
                 double target,
                 const MotionCurve& curve,
                 double epsilon = 0.001);
  // 直接设成 value，停掉动画（不调用 on_step）。
  void SetValue(double value);
  // 停在现在的值上（视图离开窗口时）。
  void Stop();

  // ui::CompositorAnimationObserver:
  void OnAnimationStep(base::TimeTicks timestamp) override;
  void OnCompositingShuttingDown(ui::Compositor* compositor) override;

 private:
  double VelocityAt(double seconds) const;

  double value_;
  double from_ = 0;
  double to_ = 0;
  double velocity_ = 0;
  double duration_ = 0;
  MotionCurve curve_;
  base::TimeTicks start_;
  base::RepeatingClosure on_step_;
  raw_ptr<ui::Compositor> compositor_ = nullptr;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_FRAME_MOTION_H_
