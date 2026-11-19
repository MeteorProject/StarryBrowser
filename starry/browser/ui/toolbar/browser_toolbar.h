// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TOOLBAR_BROWSER_TOOLBAR_H_
#define STARRY_BROWSER_UI_TOOLBAR_BROWSER_TOOLBAR_H_

#include "base/memory/raw_ptr.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/frame/chrome_layout.h"
#include "starry/browser/ui/motion/frame_transition.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

class BrowserWindowInterface;

namespace starry {

class AddressPill;
class AssistantButton;
class ExtensionBar;
class NavigationButtons;
class ToolbarSeparator;
class UtilityButtons;

// 内容卡片上方的工具栏，高 metrics::kToolbarHeight。侧栏布局下仅在地址栏或扩展按钮
// 置顶时显示，并与侧栏顶栏（红绿灯行）对齐；顶部标签布局下位于标签行下方。从左到右
// 依次为导航按钮、地址栏（占满剩余宽度）、扩展按钮、工具按钮（顶部标签布局）与 AI
// 助手按钮（侧栏布局下位于最右端），各组之间以细线分隔。空白区域可拖动窗口。
//
// 切换布局时：地址栏、导航按钮、AI 助手按钮通过 MatchedGeometry 过渡，其余控件淡入
// 淡出或滑动补位。
class BrowserToolbar : public views::View {
  METADATA_HEADER(BrowserToolbar, views::View)

 public:
  BrowserToolbar(BrowserWindowInterface* browser, MatchedGeometry* matched);
  BrowserToolbar(const BrowserToolbar&) = delete;
  BrowserToolbar& operator=(const BrowserToolbar&) = delete;
  ~BrowserToolbar() override;

  void SetTheme(const Theme& theme);
  // 布局变了。animated 时里面的东西用 curve 过渡（地址栏这些飞过去由
  // MatchedGeometry 管， 调用之前它已经 Begin 了）。
  void SetChromeLayout(const ChromeLayout& layout,
                       bool assistant_shown,
                       bool animated);

  AddressPill* address_pill() { return pill_; }
  NavigationButtons* navigation() { return nav_; }
  ExtensionBar* extension_bar() { return ext_; }
  UtilityButtons* utility_buttons() { return utility_; }
  AssistantButton* assistant_button() { return assistant_; }

  // views::View:
  void Layout(PassKey) override;
  void ChildPreferredSizeChanged(views::View* child) override;

 private:
  base::WeakPtr<MatchedGeometry> matched_;
  ChromeLayout layout_;
  bool assistant_shown_ = false;
  FrameTransition transition_;

  raw_ptr<NavigationButtons> nav_ = nullptr;
  raw_ptr<AddressPill> pill_ = nullptr;
  raw_ptr<ExtensionBar> ext_ = nullptr;
  raw_ptr<ToolbarSeparator> utility_separator_ = nullptr;
  raw_ptr<UtilityButtons> utility_ = nullptr;
  raw_ptr<ToolbarSeparator> assistant_separator_ = nullptr;
  raw_ptr<AssistantButton> assistant_ = nullptr;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TOOLBAR_BROWSER_TOOLBAR_H_
