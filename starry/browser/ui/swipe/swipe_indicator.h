// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SWIPE_SWIPE_INDICATOR_H_
#define STARRY_BROWSER_UI_SWIPE_SWIPE_INDICATOR_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/compositor/compositor_animation_observer.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/image/image.h"
#include "ui/views/view.h"

namespace starry {

class GlassView;

// 触控板返回、前进时网页边上的指示器（SwipeNavigation 摆它）。换掉 Chrome 的半圆箭头：
//
// - 一个实时 Liquid Glass 的圆泡从那一格网页的边缘被手指拉出来（裁在网页里，像是从边缘
//   后面滑出来），跟手是 interactiveSpring，过了上膛的距离还能再拉一点（橡皮筋）。圆泡上
//   一圈进度环随拖动走满；上膛时填成主色、箭头反白、鼓一下。网页那条边上同时亮起一道柔光。
// - 上了膛往下选翻几页时，圆泡旁边展开一张玻璃卡片，列出这个方向的历史记录（图标和
//   标题），选中的一行用主色的胶囊标出来，圆泡跟着选中的行上下滑。退了膛时卡片变淡、
//   高亮变灰：松手就取消。
// - 松手翻页：圆泡往前冲一下、整个淡掉；取消：缩回边缘后面。
//
// 这一层铺满窗口，不接鼠标（只显示）；不在手势里时什么都不画。
//
// 跟手的部分跟着合成器的帧走（CompositorAnimationObserver，和屏幕刷新对齐，ProMotion 上是
// 120 帧），不用 ValueMotion 的定时器：定时器和刷新对不齐，有的帧走两步、有的帧不动，看起来
// 帧数低。每帧把所有数值走到这一帧的时刻，只摆一次图层；都停下了就不再要帧。圆泡上的东西
// 和列表的内容各自一层、变了才重画，玻璃的图层本身不重画。
class SwipeIndicator : public views::View,
                       public ui::CompositorAnimationObserver {
  METADATA_HEADER(SwipeIndicator, views::View)

 public:
  struct Row {
    std::u16string title;
    gfx::Image favicon;
  };

  SwipeIndicator();
  SwipeIndicator(const SwipeIndicator&) = delete;
  SwipeIndicator& operator=(const SwipeIndicator&) = delete;
  ~SwipeIndicator() override;

  void SetTheme(const Theme& theme);
  // 一次手势开始。page 是那一格网页（本视图的坐标），forward 时从右边出来；rows 是这个
  // 方向的历史记录（近的在前）。返回列表放得下几行：不到两行（记录不够、网页太小）时
  // 是 0，不能展开。
  int Begin(const gfx::Rect& page, bool forward, std::vector<Row> rows);
  // 手指动了。progress 是拖了多远（1 是上膛的距离）；list_open 时 selected 是选中的一行
  // （0 起）。
  void Update(float progress, bool armed, bool list_open, int selected);
  // 结束：翻了页（往前冲、淡掉），或者取消（缩回边缘）。
  void Finish(bool navigated);
  bool active() const { return active_; }

  base::WeakPtr<SwipeIndicator> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  // 调试：圆泡现在的位置、缩放，上膛的程度，列表在哪里、选中第几行，这次手势走了几帧、
  // 重画了几次。
  std::string DebugDescription() const;

  // views::View:
  void RemovedFromWidget() override;

  // ui::CompositorAnimationObserver:
  void OnAnimationStep(base::TimeTicks timestamp) override;
  void OnCompositingShuttingDown(ui::Compositor* compositor) override;

 private:
  class BubbleFace;
  class EdgeGlow;
  class HistoryList;

  // 跟着合成器的帧走的一个数值。弹簧换终点时从现在的位置和速度接着走（每帧按解析解走
  // 一段，同 ValueMotion）；定长的曲线（关键帧、easeOut）从开始的那一帧算。减弱动态效果时
  // 直接跳到终点。
  class FrameValue {
   public:
    explicit FrameValue(double value);
    FrameValue(const FrameValue&) = delete;
    FrameValue& operator=(const FrameValue&) = delete;
    ~FrameValue();

    double value() const { return value_; }
    double target() const { return target_; }
    bool moving() const { return moving_; }
    // 直接放到 value，停下。
    void Set(double value);
    void SpringTo(double target, const Spring& spring);
    void CurveTo(double target,
                 const MotionCurve& curve,
                 base::TimeTicks start);
    // 走到 now，dt 是离上一帧多久（秒）。
    void Step(base::TimeTicks now, double dt);

   private:
    double value_;
    double target_;
    double velocity_ = 0;
    bool moving_ = false;
    // 定长的曲线（不是弹簧）。
    bool timed_ = false;
    Spring spring_;
    MotionCurve curve_;
    double from_ = 0;
    base::TimeTicks start_;
    double duration_ = 0;
  };

  // 圆泡在网页里的中心（page_ 的坐标），progress 时的横向位置。
  float BubbleX(float progress) const;
  // 有列表时选中的那一行的中心离圆泡静止时的中心多远。
  float LiftFor(int selected) const;
  void LayoutList();
  void OpenList();
  // 有数值在动时开始要帧；都停了、不在窗口里时不要。
  void StartFrames();
  void StopFrames();
  bool AnyMoving() const;
  // 定长曲线从哪一刻算：在要帧时是上一帧，否则是现在。
  base::TimeTicks Clock() const;
  // 把这一帧的数值放到图层上（没变的不动）。
  void Apply();
  void Hide(int generation);

  Theme theme_ = Theme::DarkBase();
  bool active_ = false;
  bool forward_ = false;
  int generation_ = 0;
  gfx::Size page_size_;
  // 圆泡静止（上膛）时的中心（page_ 的坐标）。
  gfx::PointF rest_;
  std::vector<Row> rows_;
  // 这次能显示几行（放不下时少几行）。
  int shown_rows_ = 0;
  bool armed_ = false;
  bool list_open_ = false;
  int selected_ = 0;
  float progress_ = 0;

  // 那一格网页：裁掉伸出网页的部分，圆角同内容卡片。
  raw_ptr<views::View> page_ = nullptr;
  raw_ptr<EdgeGlow> glow_ = nullptr;
  raw_ptr<GlassView> bubble_ = nullptr;
  raw_ptr<BubbleFace> face_ = nullptr;
  raw_ptr<GlassView> list_ = nullptr;
  raw_ptr<HistoryList> list_rows_ = nullptr;
  // 列表的卡片在 page_ 里的位置。
  gfx::Rect list_bounds_;

  // 圆泡相对静止位置的横向、纵向位移，缩放，上膛时鼓一下的倍数，上膛的程度（0…1）。
  FrameValue travel_{0};
  FrameValue lift_{0};
  FrameValue scale_{1};
  FrameValue pop_{1};
  FrameValue arm_{0};
  // 结束时柔光淡出（乘在它的不透明度上）。
  FrameValue glow_fade_{1};
  // 列表的高亮在第几行（可以在两行之间）、高亮是主色还是灰的（0…1）。
  FrameValue list_highlight_{0};
  FrameValue list_armed_{1};
  // 在要帧的合成器（没在要时为空）和上一帧的时刻。
  raw_ptr<ui::Compositor> compositor_ = nullptr;
  base::TimeTicks last_frame_;
  // 上一次放到图层上的圆泡姿态（没变就不再设）。
  std::optional<LayerPose> applied_pose_;
  // 调试：这次手势走了几帧，连着的两帧之间隔了多久（加起来、最长、几次），比当时的
  // 平均长一半以上的有几次（掉帧）。
  int frames_ = 0;
  bool frame_seen_ = false;
  base::TimeTicks previous_frame_;
  double frame_gap_sum_ = 0;
  double frame_gap_max_ = 0;
  int frame_gaps_ = 0;
  int long_frames_ = 0;
  // 圆泡结束时淡出；列表的出现、退膛变淡和淡出。都在玻璃自己的图层上。
  LayerMotion bubble_motion_;
  LayerMotion list_motion_;
  base::WeakPtrFactory<SwipeIndicator> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SWIPE_SWIPE_INDICATOR_H_
