// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_LAYER_LOOP_H_
#define STARRY_BROWSER_UI_MOTION_LAYER_LOOP_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"

namespace gfx {
class AnimationCurve;
}  // namespace gfx

namespace ui {
class Layer;
class LayerAnimator;
}  // namespace ui

namespace starry {

// 一直重复、直到叫停的装饰动画（智能体的描边呼吸、星光跳动、转圈的环）。整个在合成器里
// 跑：给图层的 cc 动画加一个无限循环的 KeyframeModel，不经过 ui::LayerAnimator 的序列。
//
// 不用 LayerAnimationSequence::set_is_repeating 的原因：序列在放的时候，ui::Compositor
// 每一帧都要在主线程上做一次 BeginMainFrame 和提交（浏览器的合成器是单线程的），智能体
// 做一步的十几秒里浏览器进程一直占着 CPU。这里主线程只在开始、停下时动一下。
//
// 值按全窗口统一的 kLoopFps 网格取样：合成器在值没变的帧上不出新帧，几个循环同时在动
// 也落在同一个 vsync 上，120 Hz 屏幕上同样按每秒 kLoopFps 帧提交。macOS 上存在实时
// 背景滤镜（如玻璃提示条、媒体岛）时每帧都需要整窗重绘，限制帧率可降低功耗。周期需为
// 0.1 秒的整数倍以与网格对齐。
//
// 一个 LayerLoop 管一个图层的一个动画；图层的同一个属性不要再用 ui::LayerAnimator 动。
class LayerLoop {
 public:
  // 装饰动画的帧率。
  static constexpr int kLoopFps = 30;

  // origin 之后网格的第一格离 origin 多少毫秒（0 到一格）。给岛里的网页用：网页的
  // 时间线从导航开始算，它的循环动画（CSS 的 steps）的开始时间放在「这个值 + 整数格」
  // 上，就和浏览器这边的循环在同一个 vsync 上变，不会两边错开、帧数翻倍。
  static double GridPhaseMs(base::TimeTicks origin);

  LayerLoop();
  LayerLoop(const LayerLoop&) = delete;
  LayerLoop& operator=(const LayerLoop&) = delete;
  ~LayerLoop();

  // 不透明度在 from 与 to 之间往复呼吸，单程时长 half（easeInEaseOut），无限循环。
  void Pulse(ui::Layer* layer, float from, float to, base::TimeDelta half);
  // 绕图层的原点（父图层坐标里的 0, 0）顺时针匀速转，period 一圈。
  void Spin(ui::Layer* layer, base::TimeDelta period);
  // 停下：图层回到主线程上设的值。
  void Stop();

  bool running() const { return keyframe_model_id_ != 0; }

 private:
  // target_property 是 cc::TargetProperty::Type。
  void Start(ui::Layer* layer,
             std::unique_ptr<gfx::AnimationCurve> curve,
             int target_property);

  // 拿着动画器（引用计数）而不是图层：图层先没了也能安全地停。
  scoped_refptr<ui::LayerAnimator> animator_;
  int keyframe_model_id_ = 0;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_LAYER_LOOP_H_
