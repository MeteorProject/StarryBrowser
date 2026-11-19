// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TOOLBAR_COMPACT_TOOLBAR_H_
#define STARRY_BROWSER_UI_TOOLBAR_COMPACT_TOOLBAR_H_

#include <optional>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents_observer.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/frame/chrome_layout.h"
#include "starry/browser/ui/motion/frame_transition.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "starry/browser/ui/toolbar/page_color.h"
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

// 紧凑地址栏：地址栏置顶时不单独占行，而是嵌入内容卡片顶部的细栏（高
// metrics::kCompactToolbarHeight，中线与红绿灯对齐）。从左到右依次为导航按钮（随
// 地址栏置顶时）、地址（网站 / 标题，占满剩余宽度）、扩展按钮和工具按钮（顶部标签
// 布局）或 AI 助手按钮（侧栏布局），按钮尺寸与标准工具栏一致，上下各留 4px。底色
// 取自网页顶边，与页面融为一体，底边带浅分隔线；空白处可拖动窗口。
//
// 分屏时每格各有一条（等高，切换活动格时页面无需重排）：当前格显示完整控件，其余
// 格仅显示地址。
//
// 页面顶色采样由本视图驱动：切标签、加载起止、同页地址变化、theme-color 或背景色
// 变化后分别在 0.15s 与 1s 采样。切标签时快速过渡底色，同页变色时缓慢过渡。
class CompactToolbar : public views::View, public content::WebContentsObserver {
  METADATA_HEADER(CompactToolbar, views::View)

 public:
  CompactToolbar(BrowserWindowInterface* browser, MatchedGeometry* matched);
  CompactToolbar(const CompactToolbar&) = delete;
  CompactToolbar& operator=(const CompactToolbar&) = delete;
  ~CompactToolbar() override;

  // 窗口的主题（不跟网页时的配色，从这里弹出的东西也用它）。
  void SetWindowTheme(const Theme& theme);
  // 底色跟着网页（设置里开着、不是无痕窗口）。
  void SetFollowsPage(bool follows_page);
  void SetChromeLayout(const ChromeLayout& layout,
                       bool assistant_shown,
                       bool animated);
  // 这一条属于哪个标签；focused：当前的一格（导航、扩展和工具按钮只放在这一条上）；
  // split：在分屏里（不和侧栏、工具栏里的控件一起飞）。
  void SetPane(tabs::TabInterface* tab, bool focused, bool split);
  // Windows 紧凑地址栏右端让出系统控制按钮的宽度（含竖分隔线）。
  void SetTrailingInset(int inset);
  void SetStyleChangedCallback(base::RepeatingClosure callback);
  tabs::TabInterface* tab() const { return tab_.Get(); }
  bool focused() const { return focused_; }
  const CompactToolbarStyle& style() const { return shown_; }

  AddressPill* address_pill() { return address_; }
  NavigationButtons* navigation() { return nav_; }
  ExtensionBar* extension_bar() { return ext_; }
  UtilityButtons* utility_buttons() { return utility_; }
  AssistantButton* assistant_button() { return assistant_; }

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  void ChildPreferredSizeChanged(views::View* child) override;
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;

  // content::WebContentsObserver:
  void DidChangeThemeColor() override;
  void OnBackgroundColorChanged() override;
  void DidStartLoading() override;
  void DidStopLoading() override;
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void PrimaryPageChanged(content::Page& page) override;

 private:
  // 配色跟上（换标签时快，同一个网页里慢慢变）。
  void UpdateStyle(bool tab_changed);
  void ApplyStyle();
  void ScheduleSample();
  void Sample();

  const raw_ptr<BrowserWindowInterface> browser_;
  base::WeakPtr<MatchedGeometry> matched_;
  Theme window_theme_ = Theme::DarkBase();
  bool follows_page_ = true;
  ChromeLayout layout_;
  bool assistant_shown_ = false;
  tabs::TabHandle tab_;
  bool focused_ = true;
  bool split_ = false;
  int trailing_inset_ = 0;
  base::RepeatingClosure style_changed_;
  FrameTransition transition_;

  // 正显示着的配色和要变过去的。
  CompactToolbarStyle shown_;
  CompactToolbarStyle from_;
  CompactToolbarStyle target_;
  ValueMotion style_motion_;
  base::OneShotTimer first_sample_;
  base::OneShotTimer second_sample_;

  raw_ptr<NavigationButtons> nav_ = nullptr;
  raw_ptr<AddressPill> address_ = nullptr;
  raw_ptr<ExtensionBar> ext_ = nullptr;
  raw_ptr<ToolbarSeparator> utility_separator_ = nullptr;
  raw_ptr<UtilityButtons> utility_ = nullptr;
  raw_ptr<ToolbarSeparator> assistant_separator_ = nullptr;
  raw_ptr<AssistantButton> assistant_ = nullptr;
  raw_ptr<ToolbarSeparator> controls_separator_ = nullptr;

  base::WeakPtrFactory<CompactToolbar> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TOOLBAR_COMPACT_TOOLBAR_H_
