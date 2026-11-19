// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_POPOVER_POPOVER_HOST_H_
#define STARRY_BROWSER_UI_POPOVER_POPOVER_HOST_H_

#include <memory>
#include <optional>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/view_tracker.h"

namespace ui {
class Event;
}

namespace views {
class EventMonitor;
}

namespace starry {

class FrameMotion;
class PopoverCard;

// 弹出框里的内容（书签、缩放……）。
class PopoverContent : public views::View {
  METADATA_HEADER(PopoverContent, views::View)

 public:
  PopoverContent();
  ~PopoverContent() override;

  // 内容的大小（不含卡片的边框）。
  virtual gfx::Size ContentSize() const = 0;
  virtual void SetTheme(const Theme& theme);
  // 打开时拿到焦点的视图（没有时焦点不动）。
  virtual views::View* InitialFocus();
  // 要关了（点了外面、Esc、换了标签、关窗口）：书签的名字在这里存。
  virtual void WillClose();
  // 里面有自己要处理 Esc 的东西（正在改名的输入框）：这时 Esc 不收起卡片。
  virtual bool HandlesEscape() const;
};

// 窗口里的小弹出框：盖在整个窗口上的一层（StarryBrowserView 的子视图），里面一张实时
// Liquid Glass 的卡片，从挂着的那个角由 0.95 放大、淡入。只有卡片接收鼠标；在外面按下鼠标、
// 按 Esc 时关掉，那一下照常给下面的东西。可以定时自动收起（鼠标停在卡片上时不收）。同一时间
// 一个，再打开时换掉内容。
class PopoverHost : public views::View, public views::ViewTargeterDelegate {
  METADATA_HEADER(PopoverHost, views::View)

 public:
  // 卡片放在 anchor（本视图的坐标）的哪里。
  enum class Placement {
    // 下面，左边对齐（侧栏的地址栏下面）。
    kBelow,
    // 下面，右边对齐（地址栏里的盾牌按钮下面）。
    kBelowRight,
    // 里面的右上角（内容卡片的右上角，同查找栏）。
    kInsideTopRight,
    // 里面的左上角（侧栏收起时挂在内容卡片的左上角）。
    kInsideTopLeft,
  };

  PopoverHost();
  PopoverHost(const PopoverHost&) = delete;
  PopoverHost& operator=(const PopoverHost&) = delete;
  ~PopoverHost() override;

  // 卡片从 from（本视图的坐标，圆角 radius 的一块，比如常用图块里的文件夹）
  // 长出来：玻璃的位置、大小、圆角一起动到卡片的（带一点回弹），内容按最后的
  // 大小排好、裁在玻璃里，后半程淡入；收起时缩回这一块。
  struct Grow {
    gfx::Rect from;
    float radius = 0;
  };

  void Show(std::unique_ptr<PopoverContent> content,
            const gfx::Rect& anchor,
            Placement placement,
            std::optional<Grow> grow = std::nullopt);
  // 关掉（动画淡出）。
  void Close();
  bool is_showing() const { return card_ != nullptr && !closing_; }
  // 正在显示的内容；没有时为空。
  PopoverContent* content();
  void SetTheme(const Theme& theme);
  // 过 delay 自动收起；鼠标停在卡片上时等离开再算。
  void ScheduleAutoClose(base::TimeDelta delay);
  void CancelAutoClose();
  // 内容的大小变了。
  void ContentSizeChanged();
  // 关掉以后调用（每次 Show 一个）。
  void SetClosedCallback(base::OnceClosure closed);
  gfx::Rect CardBoundsForTesting() const;

  // views::View:
  void Layout(PassKey) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  void OnWindowEvent(const ui::Event& event);
  void OnCardHover(bool hovering);

 private:
  class WindowWatcher;

  void PlaceCard();
  void RemoveCard();
  // 长出来、缩回去的一帧（grow_motion_ 的值是进度，0 是那一块，1 是卡片）。
  void StepGrow();

  Theme theme_ = Theme::DarkBase();
  raw_ptr<PopoverCard> card_ = nullptr;
  bool closing_ = false;
  gfx::Rect anchor_;
  Placement placement_ = Placement::kBelow;
  std::unique_ptr<WindowWatcher> watcher_;
  std::unique_ptr<views::EventMonitor> event_monitor_;
  views::ViewTracker previous_focus_;
  base::OneShotTimer auto_close_timer_;
  base::TimeDelta auto_close_delay_;
  bool hovering_ = false;
  base::OnceClosure closed_;
  // 这张卡片从哪一块长出来（收起时缩回去）；没有是普通的出现。
  std::optional<Grow> grow_;
  std::unique_ptr<FrameMotion> grow_motion_;
  // 正在长出来、缩回去：玻璃在那一块和 target_ 之间。
  bool morphing_ = false;
  // 卡片该在的位置（PlaceCard 算的）。
  gfx::Rect target_;
  base::WeakPtrFactory<PopoverHost> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_POPOVER_POPOVER_HOST_H_
