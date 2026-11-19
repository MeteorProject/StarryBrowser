// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_ELASTIC_SCROLL_VIEW_H_
#define STARRY_BROWSER_UI_MOTION_ELASTIC_SCROLL_VIEW_H_

#include "base/time/time.h"
#include "base/timer/timer.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/scroll_view.h"

namespace starry {

// 支持越界阻尼拉伸与松手回弹的跨平台滚动视图（补齐 views::ScrollView 的弹性滚动）：
// - 手指在触控板上拖过头：内容跟着手指挪，越拖越慢（StretchFor）；
//   往回拖先收回来，再接着滚。
// - 手指离开：用 Motion.scrollBounce 弹回去。回弹途中手指又放上来
//   就接住。
// - 惯性滚动撞到头：带着当时的速度冲出去一点再弹回来，一次惯性
//   只弹一次。
// - 没有阶段信息的滚轮（普通鼠标）不回弹。
//
// 滚动本身还是 views::ScrollView 的（图层滚动，在合成器上）；
// 拉出去的那一段是内容图层的位移，回弹的弹簧在合成器线程上跑。
// 内容视图的图层变换归它管，别处不要再设。
class ElasticScrollView : public views::ScrollView {
  METADATA_HEADER(ElasticScrollView, views::ScrollView)

 public:
  enum class Axis { kVertical, kHorizontal };
  enum class Elasticity {
    kNone,
    // 仅当内容超出可见区域时允许弹性拉伸。
    kWhenScrollable,
    // 即使内容未超出可见区域也允许弹性拉伸。
    kAlways,
  };

  ElasticScrollView(Axis axis, Elasticity elasticity);
  ElasticScrollView(const ElasticScrollView&) = delete;
  ElasticScrollView& operator=(const ElasticScrollView&) = delete;
  ~ElasticScrollView() override;

  void SetElasticity(Elasticity elasticity);
  Elasticity elasticity() const { return elasticity_; }

  // 内容现在被拉出去多少（屏幕上看到的）：正数是往下、往右挪
  // （拉过了开头），负数是往上、往左（拉过了末尾）。
  float CurrentStretch();

  // 计算越界距离 overscroll 对应的橡皮筋拉伸位移：初始约 0.55 倍阻尼，
  // 随距离递减，渐近上限为 limit（可见区域长度）。
  static double StretchFor(double overscroll, double limit);
  // StretchFor 的反函数：回弹途中接住时，从现在的位置接着拖。
  static double OverscrollFor(double stretch, double limit);

  // views::ScrollView:
  void OnScrollEvent(ui::ScrollEvent* event) override;

 private:
  bool horizontal() const { return axis_ == Axis::kHorizontal; }
  // 这次事件沿滚动方向的位移：正数是往开头滚（滚动位置变小）。
  double AxisDelta(const ui::ScrollEvent& event) const;
  // 可见区域沿滚动方向的长度。
  double Limit() const;
  // 再滚 delta 会超出头多少：正数是超过开头，负数是超过末尾，
  // 0 是没到头。
  double OverscrollAfter(double delta) const;
  // 手指在拖。返回 true 时这次事件全用在拉伸上，不再滚动。
  bool Track(double delta);
  // 手指放上来：回弹途中接住。
  void Catch();
  // 手指离开：弹回去。返回刚才是不是拉着的。
  bool Release();
  // 惯性滚动的一步：撞到头时冲出去再弹回来。
  void Fling(double delta, base::TimeDelta interval);
  void ApplyStretch();
  void OnIdle();

  const Axis axis_;
  Elasticity elasticity_;
  // 手指拖过头的距离（没算阻力），只在手指拖着时不为 0。
  double overscroll_ = 0;
  // 这一次惯性已经弹过了（或者松手时正拉着）：剩下的惯性不再往外冲。
  bool momentum_spent_ = false;
  base::TimeTicks last_event_time_;
  LayerMotion motion_;
  // 有的平台手指离开时没有事件：拉着的时候一会儿没有新的滚动就弹回去。
  base::OneShotTimer idle_timer_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_ELASTIC_SCROLL_VIEW_H_
