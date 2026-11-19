// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SWIPE_OVERSCROLL_DELEGATE_H_
#define STARRY_BROWSER_UI_SWIPE_OVERSCROLL_DELEGATE_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "content/common/content_export.h"

namespace content {
class OverscrollControllerDelegate;
class WebContents;
}  // namespace content

namespace starry {

// 编进 //content/browser（patches/content/browser/BUILD.gn.patch），所有平台都编。
//
// aura（Windows、Linux）上触控板、触摸屏左右滑动翻页走 content 的
// OverscrollController，Chrome 在 GestureNavSimple 里画自己的箭头。这里给控制器一个包在
// GestureNavSimple 外面的 delegate（补丁 web_contents_view_aura.cc）：网页是 Starry 窗口里
// 的标签时，左右翻页交给 //starry/browser 的 SwipeNavigation（同 macOS 的指示器、上下选
// 翻几页），别的（下拉刷新、开发者工具这类不是 Starry 窗口的）还是 GestureNavSimple。
//
// 和控制器配合的两处补丁（overscroll_controller.cc、overscroll_controller_delegate.*）：
// - 上过膛以后左右的模式不因为上下移动变掉（控制器本来在竖的超过横的 1/2.5 时重置）。
// - 翻页进行中控制器看到的每个事件都给 delegate 看一眼，找手指离开：触控板、触摸屏的
//   GestureScrollEnd，甩出去的 GestureFlingStart，惯性开始（手指早就离开了）。翻不翻由
//   SwipeNavigation 按上没上膛、松手的速度决定；控制器自己按屏幕宽 30% 判定的完成、取消
//   在那之后才到，不再管。控制器没说手指离开就自己判定完成了（比如中途动了鼠标）也按
//   松手算；没说手指离开就重置了（按了键、开始了新的滚动）按取消算。
// 翻页（导航）晚一个任务做：导航可能把控制器连同网页的视图一起换掉，不能在它的调用里做。

// 一次左右翻页。位移、速度都是 DIP，位移是控制器给的（去掉了开始的门槛）。析构时还没
// 松手就是取消。
class OverscrollSwipe {
 public:
  virtual ~OverscrollSwipe() = default;
  // dx 朝翻页的方向为正，dy 向下为正（网页跟着手指走的方向）。
  virtual void Update(float dx, float dy) = 0;
  // 手指离开，velocity 是这时朝翻页方向的速度（DIP/秒）。
  virtual void Release(float velocity) = 0;
  // 上过膛：之后上下移动用来选翻几页，控制器要保持左右的模式。
  virtual bool KeepsDirection() const = 0;
  virtual std::string DebugDescription() const = 0;
};

// 开始一次翻页（forward：前进）；不在 Starry 的窗口里、不能往那边走时返回空。
// //starry/browser 启动时登记（overscroll_navigation.h），content 不能反过来依赖它。
using OverscrollSwipeFactory = base::RepeatingCallback<std::unique_ptr<
    OverscrollSwipe>(content::WebContents* contents, bool forward)>;
CONTENT_EXPORT void SetOverscrollSwipeFactory(OverscrollSwipeFactory factory);

// 控制器用的 delegate，挂在 contents 上（跟着它一起销毁）。fallback 是 Chrome 的
// GestureNavSimple；传空的时候沿用之前给的。
CONTENT_EXPORT base::WeakPtr<content::OverscrollControllerDelegate>
OverscrollDelegateFor(content::WebContents* contents,
                      content::OverscrollControllerDelegate* fallback);

// 自动化辅助：在 contents 上通过 OverscrollController 注入合成滚动事件并返回当前状态。
CONTENT_EXPORT std::string SimulateOverscrollForTesting(
    content::WebContents* contents,
    const std::string& command);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SWIPE_OVERSCROLL_DELEGATE_H_
