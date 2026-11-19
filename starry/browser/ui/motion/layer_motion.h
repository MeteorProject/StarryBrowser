// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_LAYER_MOTION_H_
#define STARRY_BROWSER_UI_MOTION_LAYER_MOTION_H_

#include <array>
#include <cstdint>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/geometry/vector2d_f.h"

namespace ui {
class Layer;
}

namespace starry {

// 图层相对它摆好的位置的样子：位移，和以 origin（图层坐标）为中心的缩放。
struct LayerPose {
  gfx::Vector2dF offset;
  float scale = 1;

  gfx::Transform ToTransform(const gfx::PointF& origin) const;
  bool operator==(const LayerPose&) const = default;
};

// 一次图层动画的全部参数：曲线和每个分量的起点、终点、初速度。合成器线程上的曲线拿的是
// 它的拷贝，所以它只能是值。
struct MotionSpec {
  MotionCurve curve;
  // 变换是 3 个分量（位移 x、y，缩放），不透明度是 1 个。
  int components = 1;
  std::array<double, 3> from{};
  std::array<double, 3> to{};
  std::array<double, 3> velocity{};
  gfx::PointF origin;
  base::TimeDelta duration;
  // 可选的入场去模糊：跟变换共用合成器的开始时间和逐帧回调。
  float blur_from = 0;

  double ValueAt(int component, double seconds) const;
  double VelocityAt(int component, double seconds) const;
  LayerPose PoseAt(double seconds) const;
  gfx::Transform TransformAt(double seconds) const;
};

// 按 MotionCurve 驱动图层的变换（位移、缩放）与不透明度。曲线在合成器线程上求值
// （Chromium threaded 图层动画，无需修改上游），主线程繁忙时仍保持流畅。动画进行中
// 变更目标时从当前位置与速度延续运动，保证弹簧打断连贯。
//
// 一个 LayerMotion 管一个图层；视图的图层重建过（换了 ui::Layer）就从图层里的值重新
// 开始。主线程上 ui::Layer::GetTargetTransform() / GetTargetOpacity() 是终点，现在看到的
// 样子用 CurrentPose()、CurrentOpacity()。
class LayerMotion {
 public:
  LayerMotion();
  LayerMotion(const LayerMotion&) = delete;
  LayerMotion& operator=(const LayerMotion&) = delete;
  ~LayerMotion();

  // 从现在的样子动到 to。done 在动画放完时调用（被打断、被别的动画替换时不调用）。
  void AnimateTransform(ui::Layer* layer,
                        const LayerPose& to,
                        const MotionCurve& curve,
                        const gfx::PointF& origin = gfx::PointF(),
                        base::OnceClosure done = base::OnceClosure());
  // 从现在的样子动到 to，出发时再加上 velocity（每秒的位移）：
  // to 就是现在的位置时，弹簧先被甩出去再拉回来（惯性滚动撞到头的
  // 回弹）。不是弹簧的曲线不管 velocity。
  void AnimateTransformWithVelocity(
      ui::Layer* layer,
      const LayerPose& to,
      const gfx::Vector2dF& velocity,
      const MotionCurve& curve,
      const gfx::PointF& origin = gfx::PointF(),
      base::OnceClosure done = base::OnceClosure());
  // 先跳到 from，再动到 to（插入、FLIP 式移动）。blur_from > 0 时同步过渡整层去模糊；
  // ui::Layer 不支持线程化 blur，因此在合成器的主线程逐帧回调中沿用同一时钟更新。
  // 模糊半径按变换后的父坐标系 DIP 指定。
  void AnimateTransformFrom(ui::Layer* layer,
                            const LayerPose& from,
                            const LayerPose& to,
                            const MotionCurve& curve,
                            const gfx::PointF& origin = gfx::PointF(),
                            base::OnceClosure done = base::OnceClosure(),
                            float blur_from = 0);
  // 视图的位置挪了 shift（bounds 变了，图层跟着跳过去）：先补回 shift，屏幕上的样子
  // 不跳，再从那里接着动到 to，原来的速度保留。列表里的行挪位置就是这样（FLIP）。
  void ShiftAndAnimate(ui::Layer* layer,
                       const gfx::Vector2dF& shift,
                       const LayerPose& to,
                       const MotionCurve& curve,
                       const gfx::PointF& origin = gfx::PointF());
  // 直接放到 pose，停掉正在放的变换动画。
  void SetTransform(ui::Layer* layer,
                    const LayerPose& pose,
                    const gfx::PointF& origin = gfx::PointF());

  void AnimateOpacity(ui::Layer* layer,
                      float to,
                      const MotionCurve& curve,
                      base::OnceClosure done = base::OnceClosure());
  void AnimateOpacityFrom(ui::Layer* layer,
                          float from,
                          float to,
                          const MotionCurve& curve,
                          base::OnceClosure done = base::OnceClosure());
  void SetOpacity(ui::Layer* layer, float opacity);

  // 现在屏幕上的样子（按曲线和开始的时间算）。
  LayerPose CurrentPose(ui::Layer* layer) const;
  float CurrentOpacity(ui::Layer* layer) const;
  LayerPose TargetPose(ui::Layer* layer) const;
  bool IsAnimatingTransform() const { return transform_.running; }
  bool IsAnimatingOpacity() const { return opacity_.running; }

 private:
  class Element;
  friend class Element;
  enum class Property { kTransform, kOpacity };

  struct Track {
    Track();
    Track(const Track&);
    Track& operator=(const Track&);
    ~Track();

    MotionSpec spec;
    // 合成器上真正开始的时间；还没开始时为空。
    base::TimeTicks start_time;
    int generation = 0;
    bool running = false;
  };

  void BindLayer(ui::Layer* layer);
  void Start(ui::Layer* layer,
             Property property,
             MotionSpec spec,
             base::OnceClosure done);
  // 现在的值和速度（分量）。
  // 没在动时按 origin 把图层里的变换拆成位移和缩放。
  void Sample(ui::Layer* layer,
              Property property,
              const gfx::PointF& origin,
              std::array<double, 3>& values,
              std::array<double, 3>& velocities) const;
  void ElementStarted(Property property, int generation, base::TimeTicks time);
  void ElementEnded(Property property, int generation);
  Track& track(Property property) {
    return property == Property::kTransform ? transform_ : opacity_;
  }

  // 只用来认是不是同一个图层，不解引用（图层可能已经没了）。
  uintptr_t layer_ = 0;
  Track transform_;
  Track opacity_;
  int next_generation_ = 1;

  base::WeakPtrFactory<LayerMotion> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_LAYER_MOTION_H_
