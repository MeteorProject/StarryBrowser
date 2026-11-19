// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_TRANSFER_H_
#define STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_TRANSFER_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"

class BrowserWindowInterface;
class SkBitmap;

namespace content {
class WebContents;
}

namespace display {
class Display;
}

namespace views {
class Widget;
}

namespace starry::mini_window {

class TransferView;

// 小窗口网页迁入主浏览器窗口时的飞跃过渡：截取网页画面覆盖在小窗口上方，飞向目标浏览器
// 窗口的内容卡片后淡出，露出真实网页。渲染于不拦截鼠标的透明顶层窗口中，不受小窗口关闭
// 或主窗口会话恢复影响。
//
// 不做的情况：减弱动态效果；网页没有显示在小窗口里。浏览器窗口在别的屏幕上、最小化了或
// 者没显示出来时只淡出。
class Transfer {
 public:
  // 截好图（最多等 0.25 秒）、盖到小窗口的网页上（画到屏幕上了）之后调用 ready；不做过渡
  // 时给的是空的。过渡自己管自己：飞完、Cancel 之后窗口关掉、对象删掉。
  using ReadyCallback = base::OnceCallback<void(base::WeakPtr<Transfer>)>;
  static void Prepare(BrowserWindowInterface* mini, ReadyCallback ready);

  Transfer(const Transfer&) = delete;
  Transfer& operator=(const Transfer&) = delete;
  ~Transfer();

  // 网页已经交给 target：等它放进 target 的内容卡片、知道落点后飞过去。
  void FlyTo(BrowserWindowInterface* target, content::WebContents* contents);
  // 不飞了（网页没有移过去）：淡出收掉。
  void Cancel();

  // 查询正在显示的过渡窗口状态（屏幕坐标与不透明度）。
  static views::Widget* CurrentWidget();
  static std::optional<gfx::RectF> CurrentCard();
  static float CurrentOpacity();

 private:
  Transfer(const SkBitmap& image,
           const gfx::Rect& page,
           SkColor surface,
           const display::Display& display);

  static void OnSnapshot(base::WeakPtr<BrowserWindowInterface> mini,
                         base::WeakPtr<content::WebContents> contents,
                         ReadyCallback ready,
                         const SkBitmap& image);
  // 显示出来，画到屏幕上之后（或者等不到时）调用 ready。
  void Show(ReadyCallback ready);
  void RunReady();
  void PollDestination();
  void Animate(const gfx::Rect& destination);
  void StepFlight();
  // 开始淡出，淡完关掉。
  void Finish();
  void StepFade();
  void Close();

  std::unique_ptr<views::Widget> widget_;
  raw_ptr<TransferView> view_ = nullptr;
  // 过渡窗口铺满的那块屏幕。
  const int64_t display_id_;
  const gfx::Point origin_;
  // 截图的起点、终点（屏幕坐标）。
  const gfx::RectF from_;
  gfx::RectF to_;
  ValueMotion flight_;
  ValueMotion fade_;
  ReadyCallback ready_;
  base::OneShotTimer present_timer_;
  base::WeakPtr<BrowserWindowInterface> target_;
  base::WeakPtr<content::WebContents> contents_;
  int polls_ = 0;
  base::RepeatingTimer poll_timer_;
  // 万一没人来 FlyTo / Cancel 时收掉；飞了以后到时间开始淡出。
  base::OneShotTimer timer_;
  bool finishing_ = false;
  bool closed_ = false;
  base::WeakPtrFactory<Transfer> weak_factory_{this};
};

}  // namespace starry::mini_window

#endif  // STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_TRANSFER_H_
