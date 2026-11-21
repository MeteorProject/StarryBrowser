// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FRAME_WINDOW_CONTROLS_H_
#define STARRY_BROWSER_UI_FRAME_WINDOW_CONTROLS_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

class PrefService;

namespace starry {

class GlassView;

// 非 macOS 平台（Windows / Linux）自绘窗口控制按钮样式（显示于向导与「设置 › 外观」）：
// - kGlass（默认，悬浮玻璃胶囊模式）：纯侧栏无顶栏时内容卡片顶至 8px，右上角浮一枚
//   Liquid Glass 胶囊，磁吸高亮底在三枚按钮间滑动；切到横栏（顶栏/标签行/紧凑栏）时
//   胶囊淡出融入。
// - kRail（纤细顶檐模式）：纯侧栏无顶栏时保留 30px 纤细顶部拖动檐（不遮挡网页右上角），
//   按钮统一使用 6px 软角微动效；紧凑地址栏下直接并入紧凑栏右端消除双层顶栏。
enum class WindowsControlsStyle {
  kGlass,
  kRail,
};

// 当前平台是否启用 Starry 自绘窗口控制按钮（Windows / Linux 默认启用）。
bool WindowsControlsEnabled();
WindowsControlsStyle WindowsControlsStyleFromPrefs(const PrefService* prefs);
bool WindowsControlsAutoHideFromPrefs(const PrefService* prefs);

// 纤细顶檐模式在纯侧栏无顶栏模式下卡片上方的 30px 纤细顶部拖动檐：空白处拖动窗口、
// 双击最大化，中间柔和显示当前标签页标题。
class WindowRailView : public views::View {
  METADATA_HEADER(WindowRailView, views::View)

 public:
  WindowRailView();
  WindowRailView(const WindowRailView&) = delete;
  WindowRailView& operator=(const WindowRailView&) = delete;
  ~WindowRailView() override;

  void SetTheme(const Theme& theme);
  void SetTitle(const std::u16string& title);
  void SetWindowActive(bool active);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  Theme theme_ = Theme::DarkBase();
  std::u16string title_;
  bool active_ = true;
};

// Windows 窗口控制按钮（最小化、最大化/还原、关闭）：
// - 彻底接管 Chromium 原生 WindowsCaptionButton 绘制，跟随 Starry Theme（暗/亮/网站取
//   色/无痕紫/紧凑栏取色）与 motion.h 弹簧动效。
// - NonClientHitTest 返回 HTMINBUTTON / HTMAXBUTTON / HTCLOSE，完整保留 Windows 11 原生
//   Snap Layouts 贴靠悬停菜单与最大化 Fitts's Law 右上角贴屏点击。
class WindowControlsView : public views::View,
                           public views::ViewTargeterDelegate,
                           public views::WidgetObserver {
  METADATA_HEADER(WindowControlsView, views::View)

 public:
  enum class Presentation {
    // 悬浮玻璃胶囊模式纯侧栏态：右上角悬浮 Liquid Glass 胶囊（100×30，圆角 15）。
    kFloatingGlass,
    // 纤细顶檐模式纯侧栏态：30px 纤细顶檐右端软角按钮（34×24，圆角 6）。
    kSlimRail,
    // 顶部标签行 / 顶部工具栏 / 小窗口：横栏右端软角按钮（36×28，圆角 6）。
    kInlineBar,
    // 紧凑地址栏（侧栏 + 紧凑栏）：并入最右侧紧凑栏右端（30×24，圆角 6）。
    kCompactBar,
  };

  WindowControlsView();
  WindowControlsView(const WindowControlsView&) = delete;
  WindowControlsView& operator=(const WindowControlsView&) = delete;
  ~WindowControlsView() override;

  void SetTheme(const Theme& theme);
  // 悬浮玻璃胶囊是否按深色玻璃绘制（可按网页亮度自动切换）。
  void SetGlassDark(bool dark);
  void SetPresentation(Presentation presentation, bool animated);
  Presentation presentation() const { return presentation_; }
  // 悬浮玻璃胶囊模式隐藏态：开启后在 kFloatingGlass 下平时收起为边框微提示，鼠标移入
  // 右上角边缘弹出液态玻璃胶囊。
  void SetAutoHide(bool auto_hide);
  bool auto_hide() const { return auto_hide_; }
  void SetMaximized(bool maximized);
  // 最大化时在右、上边缘扩展命中热区（视觉按钮保持原圆角与留白，鼠标甩到屏幕最右上角仍
  // 命中关闭按钮）。
  void SetHitPadding(const gfx::Insets& padding);
  const gfx::Insets& hit_padding() const { return hit_padding_; }
  void SetActivationChangedCallback(
      base::RepeatingCallback<void(bool)> callback);

  // 视觉区域尺寸（不含最大化扩展热区）。
  gfx::Size VisualSize() const;
  // 横栏右端需要为控件预留的总宽度（含左侧分隔/呼吸间距）；悬浮胶囊态为 0。
  int ReservedBarWidth() const;

  // views::View:
  void Layout(PassKey) override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // views::WidgetObserver:
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;

 private:
  class BorderHintView;
  class CaptionButton;
  class ControlsSurface;

  bool IsAutoHideActive() const;
  void RevealCapsule();
  void ScheduleRetract();
  void RetractCapsule();
  void PollPointer();
  bool IsPointerInRevealZone() const;
  gfx::Rect SensorRectInLocal() const;
  void UpdateRevealAppearance();
  void UpdateGlassAppearance();
  void UpdateButtonMetrics();
  void UpdateCapsuleHover(bool hovered);
  void OnButtonHoverChanged(int index, bool hovered);
  void OnButtonPressed(int index);
  void ApplyVisualOpacity();
  void SyncMaximizedFromWidget();
  gfx::Rect VisualRectInLocal() const;

  Theme theme_ = Theme::DarkBase();
  bool glass_dark_ = true;
  bool glass_dark_overridden_ = false;
  Presentation presentation_ = Presentation::kFloatingGlass;
  bool auto_hide_ = false;
  bool revealed_ = false;
  bool maximized_ = false;
  bool active_ = true;
  bool any_hovered_ = false;
  int hovered_index_ = -1;
  gfx::Insets hit_padding_;

  ValueMotion glass_presence_;
  ValueMotion reveal_presence_;
  ValueMotion capsule_opacity_;
  ValueMotion active_presence_;
  ValueMotion pill_x_;
  ValueMotion pill_opacity_;
  ValueMotion pill_danger_;

  base::OneShotTimer retract_timer_;
  base::RepeatingTimer pointer_poll_;

  raw_ptr<BorderHintView> hint_ = nullptr;
  raw_ptr<GlassView> glass_ = nullptr;
  raw_ptr<ControlsSurface> surface_ = nullptr;
  raw_ptr<CaptionButton> minimize_ = nullptr;
  raw_ptr<CaptionButton> maximize_ = nullptr;
  raw_ptr<CaptionButton> close_ = nullptr;

  base::RepeatingCallback<void(bool)> activation_changed_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FRAME_WINDOW_CONTROLS_H_
