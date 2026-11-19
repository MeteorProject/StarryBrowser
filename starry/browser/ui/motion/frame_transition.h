// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_FRAME_TRANSITION_H_
#define STARRY_BROWSER_UI_MOTION_FRAME_TRANSITION_H_

#include <map>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/gfx/geometry/rect.h"

namespace views {
class View;
}

namespace starry {

// 容器内多个子视图的布局过渡动画：保留的视图从原位置平滑移动，新出现的视图淡入
// （可选带缩放），消失的视图淡出并在动画结束后隐藏。逐帧在主线程更新布局，视图需
// 具备独立图层以直接控制不透明度和缩放。
//
// 用法：布局变化前调用 Begin 记录当前状态，容器 Layout 中通过 Place 摆放每个视图。
// 未处于动画过渡时 Place 直接定位并按 present 设置可见性。
class FrameTransition {
 public:
  // relayout：过渡的每一帧要重新排（通常是容器的 InvalidateLayout）。
  explicit FrameTransition(base::RepeatingClosure relayout);
  FrameTransition(const FrameTransition&) = delete;
  FrameTransition& operator=(const FrameTransition&) = delete;
  ~FrameTransition();

  void Begin(const MotionCurve& curve);
  // natural 是视图在父视图里该在的位置。appear_scale 小于 1
  // 时出现、消失还带缩放（以中 心）。
  void Place(views::View* view,
             const gfx::Rect& natural,
             bool present,
             float appear_scale = 1);
  // 视图要删掉了：不再记它。
  void Forget(views::View* view);
  bool is_animating() const { return active_; }

 private:
  struct Entry {
    gfx::Rect bounds;
    bool present = false;
    float opacity = 1;
    // Begin 时的样子。
    gfx::Rect from;
    bool was_present = false;
    float from_opacity = 1;
  };

  void Step();

  base::RepeatingClosure relayout_;
  std::map<views::View*, Entry> entries_;
  ValueMotion progress_;
  bool active_ = false;
};

// 跨容器共享元素的几何匹配过渡（如地址栏、导航按钮、助手按钮在侧栏与工具栏之间切换）：
// 新出现的副本从旧副本位置飞入并淡入，旧副本飞向新位置并淡出。位置按 root 坐标系计算，
// 即使两个容器本身也在移动仍能准确对齐。
class MatchedGeometry {
 public:
  enum class Id { kAddress, kNavigation, kAssistant };

  MatchedGeometry(views::View* root, base::RepeatingClosure relayout);
  MatchedGeometry(const MatchedGeometry&) = delete;
  MatchedGeometry& operator=(const MatchedGeometry&) = delete;
  ~MatchedGeometry();

  // 布局要变了：记下每个东西现在在哪、是哪一份。
  void Begin(const MotionCurve& curve);
  // 容器的 Layout 里摆自己的那一份。present：按现在的布局这一份显示着。
  void Place(Id id, views::View* view, const gfx::Rect& natural, bool present);
  void Forget(views::View* view);
  bool is_animating() const { return active_; }
  base::WeakPtr<MatchedGeometry> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  struct State {
    raw_ptr<views::View> owner = nullptr;
    // 现在那一份该在的位置（root 的坐标）。
    gfx::Rect target;
    bool has_target = false;
    // Begin 时显示着的那一份和它的位置。
    raw_ptr<views::View> from_owner = nullptr;
    gfx::Rect from;
  };

  void Step();
  gfx::Rect ToRoot(views::View* view, const gfx::Rect& rect) const;
  gfx::Rect FromRoot(views::View* view, const gfx::Rect& rect) const;

  const raw_ptr<views::View> root_;
  base::RepeatingClosure relayout_;
  std::map<Id, State> states_;
  ValueMotion progress_;
  bool active_ = false;
  base::WeakPtrFactory<MatchedGeometry> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_FRAME_TRANSITION_H_
