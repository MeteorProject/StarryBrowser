// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SWIPE_SWIPE_NAVIGATION_H_
#define STARRY_BROWSER_UI_SWIPE_SWIPE_NAVIGATION_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "ui/gfx/image/image.h"

namespace content {
class WebContents;
}

namespace starry {

class SwipeIndicator;

// 触控板两指左右滑动返回、前进的一次手势。平台的识别器
// （macOS 上是 history_swiper_mac.mm）认出这是在翻页以后建一个，把手指走了多远喂进来，
// 手指离开时 Release。这里决定翻不翻、翻几页，摆窗口里的指示器（SwipeIndicator），给
// 触控板的振动反馈。
//
// - 朝翻页的方向拖过 kCommitDistance 就「上膛」：指示器填满、鼓一下、振一下；拖回去不到
//   kDisarmProgress 又退回去（再振一下）。松手时上了膛就翻页；没上膛但快速一甩（过了
//   kFlickProgress、速度够快）也翻。
// - 上了膛以后手指往下移动选翻几页：指示器旁边展开这个方向的历史记录，每往下 kLevelStep
//   多翻一页，选中的一行和圆泡跟着手指走，换一行振一下；往上移回来。斜着拖的那一点
//   上下漂移不算（要以上下为主地动）。停在上膛的地方不动 kListHoldDelay 也会展开，看得到
//   还能往下选。展开以后只选第一行时和不展开一样（同后退按钮：跳过网页自己塞进来的记录）。
class SwipeNavigation {
 public:
  enum class Direction { kBack, kForward };

  // 历史里的一页（列表里的一行）。
  struct Entry {
    Entry();
    Entry(const Entry&);
    Entry& operator=(const Entry&);
    ~Entry();

    // NavigationEntry 的 GetUniqueID（手势当中网页又导航了，下标会变）。
    int unique_id = 0;
    std::u16string title;
    gfx::Image favicon;
  };

  // 拖多远上膛：触控板宽度的几分之几（Chrome 是 0.08，太容易误触；同时有快速一甩）。
  static constexpr float kCommitDistance = 0.1f;

  // contents 是 Starry 窗口里的标签：翻页由这里管（不是时交给 Chrome 自己的）。
  static bool CanHandle(content::WebContents* contents);
  // 开始一次手势。contents 不能往这个方向走、不在 Starry 的窗口里时返回空。
  static std::unique_ptr<SwipeNavigation> Start(content::WebContents* contents,
                                                Direction direction);

  SwipeNavigation(const SwipeNavigation&) = delete;
  SwipeNavigation& operator=(const SwipeNavigation&) = delete;
  // 没 Release、Commit 就析构算取消。
  ~SwipeNavigation();

  // 手指从开始的地方走了多远（触控板宽、高的几分之几）：dx 朝翻页的方向为正，dy 向下
  // 为正（手指的方向，和「自然滚动」无关）。
  void Update(float dx, float dy);
  // 手指离开。velocity 是这时朝翻页方向的速度（每秒几个触控板宽）。返回翻没翻。
  bool Release(float velocity);
  // 不管拖了多远直接翻（妙控鼠标：够不够远系统自己判断）。
  void Commit();
  void Cancel();

  Direction direction() const { return direction_; }
  // 拖了多远：1 是上膛的距离。
  float progress() const { return progress_; }
  bool armed() const { return armed_; }
  // 这次上过膛：之后上下移动是在选翻几页，识别器不该再把它当成滚动。
  bool ever_armed() const { return ever_armed_; }
  bool list_open() const { return list_open_; }
  // 选中翻几页（1 起）。
  int level() const { return level_; }
  const std::vector<Entry>& entries() const { return entries_; }
  // 调试：方向、拖了多远、上膛没有、列表和选中的一行、振了几次。
  std::string DebugDescription() const;

 private:
  SwipeNavigation(content::WebContents* contents,
                  Direction direction,
                  std::vector<Entry> entries,
                  base::WeakPtr<SwipeIndicator> indicator);

  void SetArmed(bool armed);
  // 停在上膛的地方不动：展开列表。
  void OnHold();
  void OpenList();
  void UpdateLevel(float dy);
  void ShowIndicator();
  void Finish(bool navigate);
  void Navigate();
  void Feedback(bool level_change);

  base::WeakPtr<content::WebContents> contents_;
  const Direction direction_;
  std::vector<Entry> entries_;
  base::WeakPtr<SwipeIndicator> indicator_;
  // 指示器的列表放得下几行（不到两行时不展开）。
  int list_rows_ = 0;
  bool finished_ = false;
  float progress_ = 0;
  float last_dx_ = 0;
  float last_dy_ = 0;
  bool armed_ = false;
  bool ever_armed_ = false;
  // 上膛以后以上下为主的那部分移动从哪里算起（斜着拖的漂移会把它带着走）。
  float vertical_anchor_ = 0;
  bool list_open_ = false;
  // 列表展开以后第一行对着的手指位置（越过两头时挪过去，往回动马上换行）。
  float level_origin_ = 0;
  int level_ = 1;
  int haptics_ = 0;
  base::OneShotTimer hold_timer_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SWIPE_SWIPE_NAVIGATION_H_
