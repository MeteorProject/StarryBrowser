// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_TOOLBAR_H_
#define STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_TOOLBAR_H_

#include "base/memory/raw_ptr.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

class BrowserWindowInterface;

namespace starry {

class AddressPill;
class IconButton;
class NavigationButtons;
class PillButton;

// 小窗口工具栏：高 44，从左到右为红绿灯（macOS）、导航按钮、紧凑地址栏（点击打开命令栏）
// 和「移到浏览器」胶囊按钮（窗口窄于 mini_window::kCompactWidth 时仅保留图标）。空白处可拖动窗口。
class MiniWindowToolbar : public views::View {
  METADATA_HEADER(MiniWindowToolbar, views::View)

 public:
  explicit MiniWindowToolbar(BrowserWindowInterface* browser);
  MiniWindowToolbar(const MiniWindowToolbar&) = delete;
  MiniWindowToolbar& operator=(const MiniWindowToolbar&) = delete;
  ~MiniWindowToolbar() override;

  void SetTheme(const Theme& theme);
  // Windows 上右侧让出系统控制按钮的宽度（避免和「移到浏览器」重叠）。
  void SetTrailingInset(int inset);
  AddressPill* address() { return address_; }
  // 现在显示着的「移到浏览器」（宽的胶囊或窄的图标），调试步骤点它。
  views::View* move_button();

  // views::View:
  void Layout(PassKey) override;
  void ChildPreferredSizeChanged(views::View* child) override;

 private:
  void MoveToBrowser();

  const raw_ptr<BrowserWindowInterface> browser_;
  int trailing_inset_ = 0;
  raw_ptr<NavigationButtons> nav_ = nullptr;
  raw_ptr<AddressPill> address_ = nullptr;
  raw_ptr<PillButton> move_ = nullptr;
  raw_ptr<IconButton> move_icon_ = nullptr;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MINI_WINDOW_MINI_WINDOW_TOOLBAR_H_
