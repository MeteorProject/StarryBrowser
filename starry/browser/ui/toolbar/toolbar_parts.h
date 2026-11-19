// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TOOLBAR_TOOLBAR_PARTS_H_
#define STARRY_BROWSER_UI_TOOLBAR_TOOLBAR_PARTS_H_

#include "base/memory/raw_ptr.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/view.h"

class BrowserWindowInterface;

namespace starry {

// 横向顶栏（标签行、工具栏、紧凑地址栏）右上角的 AI 助手按钮：点击开关助手浮层，
// 右键菜单与工具按钮共用可见性配置；布局切换时与侧栏底部按钮通过 MatchedGeometry 过渡。
class AssistantButton : public IconButton, public views::ContextMenuController {
  METADATA_HEADER(AssistantButton, IconButton)

 public:
  explicit AssistantButton(BrowserWindowInterface* browser);
  AssistantButton(const AssistantButton&) = delete;
  AssistantButton& operator=(const AssistantButton&) = delete;
  ~AssistantButton() override;

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override;

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
};

// 分隔两组按钮的细线：1 × 16 胶囊，左右各留 2px。
class ToolbarSeparator : public views::View {
  METADATA_HEADER(ToolbarSeparator, views::View)

 public:
  static constexpr int kWidth = 5;

  ToolbarSeparator();
  ToolbarSeparator(const ToolbarSeparator&) = delete;
  ToolbarSeparator& operator=(const ToolbarSeparator&) = delete;
  ~ToolbarSeparator() override;

  void SetTheme(const Theme& theme);

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  Theme theme_ = Theme::DarkBase();
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TOOLBAR_TOOLBAR_PARTS_H_
