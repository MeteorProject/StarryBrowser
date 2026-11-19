// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SIDEBAR_SIDEBAR_PARTS_H_
#define STARRY_BROWSER_UI_SIDEBAR_SIDEBAR_PARTS_H_

#include <optional>

#include "base/functional/callback.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

namespace starry {

// 侧栏右边缘的拖动区：悬停时光标变为左右调整并显示 2px 细线（拖动时加深）；拖动时
// 调整侧栏宽度，靠近默认宽度 6px 以内时吸附，松手写入设置。双击恢复默认宽度。
class SidebarResizeHandle : public views::View {
  METADATA_HEADER(SidebarResizeHandle, views::View)

 public:
  struct Callbacks {
    Callbacks();
    Callbacks(Callbacks&&);
    Callbacks& operator=(Callbacks&&);
    ~Callbacks();

    // 现在的宽度（开始拖时记下）。
    base::RepeatingCallback<int()> current_width;
    // 拖动中：侧栏换成这个宽度（已经吸附、限制过范围）。
    base::RepeatingCallback<void(int)> resize;
    // 松手：最后的宽度，写进设置。
    base::RepeatingCallback<void(int)> commit;
    // 双击：恢复默认宽度。
    base::RepeatingClosure reset;
  };

  explicit SidebarResizeHandle(Callbacks callbacks);
  SidebarResizeHandle(const SidebarResizeHandle&) = delete;
  SidebarResizeHandle& operator=(const SidebarResizeHandle&) = delete;
  ~SidebarResizeHandle() override;

  void SetTheme(const Theme& theme);
  bool is_dragging() const { return start_width_.has_value(); }
  // 细线上下各留出这么多（侧栏的圆角）。
  void SetLineInset(int inset);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  ui::Cursor GetCursor(const ui::MouseEvent& event) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;

 private:
  void UpdateLine();

  Callbacks callbacks_;
  Theme theme_ = Theme::DarkBase();
  int line_inset_ = 12;
  bool hovering_ = false;
  std::optional<int> start_width_;
  int start_x_ = 0;
  int last_width_ = 0;
  // 细线的淡入淡出画在自己的图层上。
  LayerMotion motion_;
};

// 自动隐藏的侧栏收起时，窗口左边缘 8 宽的感应条：鼠标进来就让侧栏滑出来。
class SidebarEdgeTrigger : public views::View {
  METADATA_HEADER(SidebarEdgeTrigger, views::View)

 public:
  explicit SidebarEdgeTrigger(base::RepeatingClosure entered);
  SidebarEdgeTrigger(const SidebarEdgeTrigger&) = delete;
  SidebarEdgeTrigger& operator=(const SidebarEdgeTrigger&) = delete;
  ~SidebarEdgeTrigger() override;

  // views::View:
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;

 private:
  base::RepeatingClosure entered_;
};

// 覆盖在网页上的侧栏面板阴影（深色 45%、浅色 22%，半径 28，向下偏移 10）。
// 视图比面板大一圈，通过 SetPanel 指定面板位置。
class SidebarShadow : public views::View {
  METADATA_HEADER(SidebarShadow, views::View)

 public:
  // 阴影在面板外面最多伸出这么多。
  static constexpr int kOutset = 64;

  SidebarShadow();
  SidebarShadow(const SidebarShadow&) = delete;
  SidebarShadow& operator=(const SidebarShadow&) = delete;
  ~SidebarShadow() override;

  void SetTheme(const Theme& theme);
  // 面板在父视图里的位置；视图摆到它外面一圈。
  void SetPanel(const gfx::Rect& panel, float radius);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  Theme theme_ = Theme::DarkBase();
  float radius_ = 12;
};

// 浮动侧栏玻璃上叠加的轻柔面板底色，保留背景色彩与细节。
class SidebarPanelFill : public views::View {
  METADATA_HEADER(SidebarPanelFill, views::View)

 public:
  SidebarPanelFill();
  SidebarPanelFill(const SidebarPanelFill&) = delete;
  SidebarPanelFill& operator=(const SidebarPanelFill&) = delete;
  ~SidebarPanelFill() override;

  void SetTheme(const Theme& theme);
  void SetOverPage(bool over_page);
  void SetRadius(float radius);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  Theme theme_ = Theme::DarkBase();
  bool over_page_ = false;
  float radius_ = 12;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SIDEBAR_SIDEBAR_PARTS_H_
