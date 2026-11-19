// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/frame_transition.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/views/view.h"

namespace starry {

namespace {

int Lerp(int from, int to, double t) {
  return static_cast<int>(std::lround(from + (to - from) * t));
}

gfx::Rect LerpRect(const gfx::Rect& from, const gfx::Rect& to, double t) {
  return gfx::Rect(Lerp(from.x(), to.x(), t), Lerp(from.y(), to.y(), t),
                   std::max(0, Lerp(from.width(), to.width(), t)),
                   std::max(0, Lerp(from.height(), to.height(), t)));
}

float CurrentOpacity(views::View* view) {
  if (!view->GetVisible()) {
    return 0;
  }
  return view->layer() ? view->layer()->opacity() : 1.0f;
}

void SetOpacity(views::View* view, float opacity) {
  if (view->layer()) {
    view->layer()->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
  }
}

// 以视图中心缩放。
void SetScale(views::View* view, float scale) {
  if (!view->layer()) {
    return;
  }
  gfx::Transform transform;
  if (scale != 1) {
    const float cx = view->width() / 2.0f;
    const float cy = view->height() / 2.0f;
    transform.Translate(cx, cy);
    transform.Scale(scale, scale);
    transform.Translate(-cx, -cy);
  }
  if (view->layer()->transform() != transform) {
    view->SetTransform(transform);
  }
}

}  // namespace

FrameTransition::FrameTransition(base::RepeatingClosure relayout)
    : relayout_(std::move(relayout)),
      progress_(
          1,
          base::BindRepeating(&FrameTransition::Step, base::Unretained(this))) {
}

FrameTransition::~FrameTransition() = default;

void FrameTransition::Begin(const MotionCurve& curve) {
  for (auto& [view, entry] : entries_) {
    entry.from = view->bounds();
    entry.was_present = entry.present || (active_ && view->GetVisible());
    entry.from_opacity = CurrentOpacity(view);
  }
  active_ = true;
  progress_.SetValue(0);
  progress_.AnimateTo(1, curve);
  if (!active_) {
    return;
  }
  if (relayout_) {
    relayout_.Run();
  }
}

void FrameTransition::Step() {
  if (!progress_.is_animating()) {
    active_ = false;
  }
  if (relayout_) {
    relayout_.Run();
  }
}

void FrameTransition::Place(views::View* view,
                            const gfx::Rect& natural,
                            bool present,
                            float appear_scale) {
  Entry& entry = entries_[view];
  entry.present = present;
  if (!active_) {
    view->SetBoundsRect(natural);
    view->SetVisible(present);
    SetOpacity(view, 1);
    SetScale(view, 1);
    entry.bounds = natural;
    return;
  }
  const double t = std::clamp(progress_.value(), 0.0, 1.0);
  gfx::Rect bounds = natural;
  float opacity = 1;
  float scale = 1;
  bool visible = present;
  if (present && entry.was_present) {
    bounds = LerpRect(entry.from, natural, t);
    opacity = entry.from_opacity + (1 - entry.from_opacity) * t;
  } else if (present) {
    opacity = entry.from_opacity + (1 - entry.from_opacity) * t;
    scale = appear_scale + (1 - appear_scale) * t;
  } else if (entry.was_present) {
    bounds = entry.from;
    opacity = entry.from_opacity * (1 - t);
    scale = 1 + (appear_scale - 1) * t;
    visible = t < 1;
  }
  view->SetBoundsRect(bounds);
  view->SetVisible(visible);
  SetOpacity(view, opacity);
  SetScale(view, scale);
  entry.bounds = bounds;
}

void FrameTransition::Forget(views::View* view) {
  entries_.erase(view);
}

MatchedGeometry::MatchedGeometry(views::View* root,
                                 base::RepeatingClosure relayout)
    : root_(root),
      relayout_(std::move(relayout)),
      progress_(
          1,
          base::BindRepeating(&MatchedGeometry::Step, base::Unretained(this))) {
}

MatchedGeometry::~MatchedGeometry() = default;

gfx::Rect MatchedGeometry::ToRoot(views::View* view,
                                  const gfx::Rect& rect) const {
  if (!view->parent()) {
    return rect;
  }
  return views::View::ConvertRectToTarget(view->parent(), root_, rect);
}

gfx::Rect MatchedGeometry::FromRoot(views::View* view,
                                    const gfx::Rect& rect) const {
  if (!view->parent()) {
    return rect;
  }
  return views::View::ConvertRectToTarget(root_, view->parent(), rect);
}

void MatchedGeometry::Begin(const MotionCurve& curve) {
  for (auto& [id, state] : states_) {
    views::View* owner = state.owner;
    // 正在飞的时候又换了：从现在显示着的那一份接着飞。
    if (active_ && state.from_owner && state.from_owner->GetVisible() &&
        (!owner || !owner->GetVisible())) {
      owner = state.from_owner;
    }
    state.from_owner = owner;
    state.from = owner && owner->GetVisible() ? ToRoot(owner, owner->bounds())
                                              : gfx::Rect();
    if (!owner || !owner->GetVisible()) {
      state.from_owner = nullptr;
    }
  }
  active_ = true;
  progress_.SetValue(0);
  progress_.AnimateTo(1, curve);
  if (!active_) {
    return;
  }
  if (relayout_) {
    relayout_.Run();
  }
}

void MatchedGeometry::Step() {
  if (!progress_.is_animating()) {
    active_ = false;
    for (auto& [id, state] : states_) {
      state.from_owner = nullptr;
    }
  }
  if (relayout_) {
    relayout_.Run();
  }
}

void MatchedGeometry::Place(Id id,
                            views::View* view,
                            const gfx::Rect& natural,
                            bool present) {
  State& state = states_[id];
  if (present) {
    state.owner = view;
    state.target = ToRoot(view, natural);
    state.has_target = true;
  } else if (state.owner == view) {
    state.owner = nullptr;
    state.has_target = false;
  }
  if (!active_ || (present && view == state.from_owner) ||
      (!present && view != state.from_owner)) {
    // 不在飞，或者还是原来那一份（只跟着容器挪），或者和这次无关。
    view->SetBoundsRect(natural);
    view->SetVisible(present);
    SetOpacity(view, 1);
    return;
  }
  const double t = std::clamp(progress_.value(), 0.0, 1.0);
  if (present) {
    // 新出现的这一份：从原来那一份的位置飞过来，淡入。
    const gfx::Rect bounds =
        state.from_owner ? FromRoot(view, LerpRect(state.from, state.target, t))
                         : natural;
    view->SetBoundsRect(bounds);
    view->SetVisible(true);
    SetOpacity(view, static_cast<float>(t));
    return;
  }
  // 原来那一份：往新的位置飞，淡出，飞完藏起来。
  const gfx::Rect to = state.has_target ? state.target : state.from;
  view->SetBoundsRect(FromRoot(view, LerpRect(state.from, to, t)));
  view->SetVisible(t < 1);
  SetOpacity(view, static_cast<float>(1 - t));
}

void MatchedGeometry::Forget(views::View* view) {
  for (auto& [id, state] : states_) {
    if (state.owner == view) {
      state.owner = nullptr;
      state.has_target = false;
    }
    if (state.from_owner == view) {
      state.from_owner = nullptr;
    }
  }
}

}  // namespace starry
