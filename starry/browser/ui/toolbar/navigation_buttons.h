// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TOOLBAR_NAVIGATION_BUTTONS_H_
#define STARRY_BROWSER_UI_TOOLBAR_NAVIGATION_BUTTONS_H_

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

class BrowserWindowInterface;

namespace starry {

class IconButton;

// 后退、前进、重新载入（载入中切换为停止）按钮组。侧栏顶栏、工具栏与紧凑地址栏各有
// 一份，布局切换时通过 MatchedGeometry 平滑过渡，状态跟随当前标签。
class NavigationButtons : public views::View, public TabStripModelObserver {
  METADATA_HEADER(NavigationButtons, views::View)

 public:
  static constexpr int kSpacing = 2;

  explicit NavigationButtons(BrowserWindowInterface* browser);
  NavigationButtons(const NavigationButtons&) = delete;
  NavigationButtons& operator=(const NavigationButtons&) = delete;
  ~NavigationButtons() override;

  void SetTheme(const Theme& theme);
  // 三个按钮占的宽度。
  static int Width();

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void Layout(PassKey) override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;

 private:
  void Update();
  void RunCommand(int command);
  void ReloadOrStop();

  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<IconButton> back_ = nullptr;
  raw_ptr<IconButton> forward_ = nullptr;
  raw_ptr<IconButton> reload_ = nullptr;
  bool showing_stop_ = false;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TOOLBAR_NAVIGATION_BUTTONS_H_
