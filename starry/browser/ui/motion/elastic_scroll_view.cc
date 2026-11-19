// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/elastic_scroll_view.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "build/build_config.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/vector2d_f.h"

namespace starry {

namespace {

// 越界拉伸阻尼系数。
constexpr double kResistance = 0.55;
// 惯性滚动撞到头：速度的这么多带进回弹，最多冲出去这么远。
constexpr double kMomentumTransfer = 0.4;
constexpr double kMaxMomentumStretch = 72;
// 两次滚动事件隔多久（用来算速度）：限制在合理的范围里，
// 第一次按一帧算。
constexpr base::TimeDelta kMinEventInterval = base::Milliseconds(4);
constexpr base::TimeDelta kMaxEventInterval = base::Milliseconds(50);
constexpr base::TimeDelta kDefaultEventInterval = base::Milliseconds(16);
// 不到这么多就当没拉出去。
constexpr float kStretchEpsilon = 0.5f;
#if !BUILDFLAG(IS_MAC)
// 手指离开没有事件的平台：拉着的时候这么久没有新的滚动就弹回去。
// macOS 上手指离开一定有事件，手指停在触控板上不动时要一直拉着。
constexpr base::TimeDelta kIdleRelease = base::Milliseconds(120);
#endif

gfx::Vector2dF AlongAxis(bool horizontal, double value) {
  return horizontal ? gfx::Vector2dF(value, 0) : gfx::Vector2dF(0, value);
}

}  // namespace

ElasticScrollView::ElasticScrollView(Axis axis, Elasticity elasticity)
    : views::ScrollView(views::ScrollView::ScrollWithLayers::kEnabled),
      axis_(axis),
      elasticity_(elasticity) {}

ElasticScrollView::~ElasticScrollView() = default;

void ElasticScrollView::SetElasticity(Elasticity elasticity) {
  if (elasticity_ == elasticity) {
    return;
  }
  elasticity_ = elasticity;
  if (elasticity_ == Elasticity::kNone) {
    Release();
  }
}

float ElasticScrollView::CurrentStretch() {
  if (!contents() || !contents()->layer()) {
    return 0;
  }
  const gfx::Vector2dF offset = motion_.CurrentPose(contents()->layer()).offset;
  return horizontal() ? offset.x() : offset.y();
}

// static
double ElasticScrollView::StretchFor(double overscroll, double limit) {
  if (overscroll <= 0 || limit <= 0) {
    return 0;
  }
  return limit * (1 - 1 / (overscroll * kResistance / limit + 1));
}

// static
double ElasticScrollView::OverscrollFor(double stretch, double limit) {
  if (stretch <= 0 || limit <= 0) {
    return 0;
  }
  // 拉到 limit 是无穷远：留一点余量。
  stretch = std::min(stretch, limit * 0.98);
  return stretch * limit / (kResistance * (limit - stretch));
}

void ElasticScrollView::OnScrollEvent(ui::ScrollEvent* event) {
  if (elasticity_ == Elasticity::kNone || !contents() || !contents()->layer()) {
    views::ScrollView::OnScrollEvent(event);
    return;
  }
  if (event->type() != ui::EventType::kScroll) {
    // Windows、Linux：手指离开触控板是 kScrollFlingStart，再放上来是
    // kScrollFlingCancel。
    if (event->type() == ui::EventType::kScrollFlingStart) {
      momentum_spent_ = Release();
    } else if (event->type() == ui::EventType::kScrollFlingCancel) {
      momentum_spent_ = false;
      Catch();
    }
    views::ScrollView::OnScrollEvent(event);
    return;
  }

  const double delta = AxisDelta(*event);
  const base::TimeDelta interval =
      last_event_time_.is_null()
          ? kDefaultEventInterval
          : std::clamp(event->time_stamp() - last_event_time_,
                       kMinEventInterval, kMaxEventInterval);
  last_event_time_ = event->time_stamp();

  switch (event->momentum_phase()) {
    case ui::EventMomentumPhase::MAY_BEGIN:
      // 手指放上来（macOS：可能还没动）。
      Catch();
      [[fallthrough]];
    case ui::EventMomentumPhase::NONE:
      // 手指在拖：新的一次手势（有的平台没有「放上来」的事件）。
      momentum_spent_ = false;
      if (Track(delta)) {
        event->SetHandled();
        event->StopPropagation();
        return;
      }
      break;
    case ui::EventMomentumPhase::BEGAN:
    case ui::EventMomentumPhase::INERTIAL_UPDATE:
      // 没等到手指离开的事件就进了惯性：先松手。
      if (Release()) {
        momentum_spent_ = true;
      }
      Fling(delta, interval);
      break;
    case ui::EventMomentumPhase::END:
    case ui::EventMomentumPhase::BLOCKED:
      // 手指离开（之后可能有惯性）、惯性结束。macOS 上没有阶段信息的
      // 滚轮（普通鼠标）也是 END，带着位移：不回弹，照常滚。
      momentum_spent_ = Release();
      break;
  }
  views::ScrollView::OnScrollEvent(event);
}

double ElasticScrollView::AxisDelta(const ui::ScrollEvent& event) const {
  const double x = event.x_offset();
  const double y = event.y_offset();
  if (horizontal()) {
    // 横向的列表竖着滚也算（同 ScrollView 的
    // SetTreatAllScrollEventsAsHorizontal）。
    return std::abs(x) >= std::abs(y) ? x : y;
  }
  // 以左右为主的滑动不拉动竖向的列表。
  return std::abs(x) > std::abs(y) ? 0 : y;
}

double ElasticScrollView::Limit() const {
  const gfx::Rect visible = GetVisibleRect();
  return horizontal() ? visible.width() : visible.height();
}

double ElasticScrollView::OverscrollAfter(double delta) const {
  const gfx::Rect visible = GetVisibleRect();
  const double offset = horizontal() ? visible.x() : visible.y();
  const double max =
      std::max(0, horizontal() ? contents()->width() - visible.width()
                               : contents()->height() - visible.height());
  if (max <= 0 && elasticity_ != Elasticity::kAlways) {
    return 0;
  }
  const double target = offset - delta;
  if (target < 0) {
    return -target;
  }
  if (target > max) {
    return max - target;
  }
  return 0;
}

bool ElasticScrollView::Track(double delta) {
  if (delta == 0) {
    return false;
  }
  if (overscroll_ != 0) {
    // 已经拉出去了：接着拉，或者往回收。收过了头就停在头上，
    // 多出来的一点不要了。
    const double next = overscroll_ + delta;
    overscroll_ = (next > 0) == (overscroll_ > 0) ? next : 0;
    ApplyStretch();
    return true;
  }
  const double over = OverscrollAfter(delta);
  if (over == 0) {
    return false;
  }
  // 这一步里滚到头的那部分还是交给 ScrollView（它自己停在头上），
  // 超出的部分拉出去。
  overscroll_ = over;
  ApplyStretch();
  return false;
}

void ElasticScrollView::Catch() {
  const float stretch = CurrentStretch();
  if (std::abs(stretch) < kStretchEpsilon) {
    return;
  }
  overscroll_ =
      std::copysign(OverscrollFor(std::abs(stretch), Limit()), stretch);
  ApplyStretch();
}

bool ElasticScrollView::Release() {
  idle_timer_.Stop();
  if (overscroll_ == 0) {
    return false;
  }
  overscroll_ = 0;
  if (contents() && contents()->layer()) {
    motion_.AnimateTransform(contents()->layer(), LayerPose(),
                             MotionCurve::Of(motion::kScrollBounce));
  }
  return true;
}

void ElasticScrollView::Fling(double delta, base::TimeDelta interval) {
  if (momentum_spent_ || delta == 0) {
    return;
  }
  if (OverscrollAfter(delta) == 0) {
    return;
  }
  momentum_spent_ = true;
  // 松手后还在弹回：不再往外冲。
  if (std::abs(CurrentStretch()) >= kStretchEpsilon) {
    return;
  }
  // 临界阻尼的弹簧以速度 v 从终点出发，最远到 v / (e · ω)。
  const double omega = 2 * std::numbers::pi / motion::kScrollBounce.response;
  const double max_velocity = kMaxMomentumStretch * std::numbers::e * omega;
  const double velocity =
      std::clamp(delta / interval.InSecondsF() * kMomentumTransfer,
                 -max_velocity, max_velocity);
  motion_.AnimateTransformWithVelocity(contents()->layer(), LayerPose(),
                                       AlongAxis(horizontal(), velocity),
                                       MotionCurve::Of(motion::kScrollBounce));
}

void ElasticScrollView::ApplyStretch() {
  // 取整：停在半个像素上文字会发虚。
  const double stretch = std::copysign(
      std::round(StretchFor(std::abs(overscroll_), Limit())), overscroll_);
  LayerPose pose;
  pose.offset = AlongAxis(horizontal(), stretch);
  motion_.SetTransform(contents()->layer(), pose);
#if !BUILDFLAG(IS_MAC)
  if (overscroll_ != 0) {
    idle_timer_.Start(FROM_HERE, kIdleRelease, this,
                      &ElasticScrollView::OnIdle);
  } else {
    idle_timer_.Stop();
  }
#endif
}

void ElasticScrollView::OnIdle() {
  Release();
}

BEGIN_METADATA(ElasticScrollView)
END_METADATA

}  // namespace starry
