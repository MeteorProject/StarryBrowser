// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/toolbar/browser_toolbar.h"

#include <algorithm>
#include <memory>

#include "base/functional/bind.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/sidebar/extension_bar.h"
#include "starry/browser/ui/sidebar/utility_buttons.h"
#include "starry/browser/ui/toolbar/address_pill.h"
#include "starry/browser/ui/toolbar/navigation_buttons.h"
#include "starry/browser/ui/toolbar/toolbar_parts.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"

namespace starry {

namespace {

// 工具栏内各控件组之间的间距。
constexpr int kSpacing = 8;

}  // namespace

BrowserToolbar::BrowserToolbar(BrowserWindowInterface* browser,
                               MatchedGeometry* matched)
    : matched_(matched->GetWeakPtr()),
      transition_(base::BindRepeating(
          [](views::View* view) { view->InvalidateLayout(); },
          base::Unretained(this))) {
  // 自己的图层：整行淡入淡出。空白处可以拖动窗口。
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetWindowDraggable(this, true);

  nav_ = AddChildView(std::make_unique<NavigationButtons>(browser));
  pill_ = AddChildView(
      std::make_unique<AddressPill>(browser, AddressPill::Style::kPill));
  ext_ = AddChildView(
      std::make_unique<ExtensionBar>(browser, ChromePlacement::kToolbar));
  ext_->SetPaintToLayer();
  ext_->layer()->SetFillsBoundsOpaquely(false);
  ext_->SetContentChangedCallback(
      base::BindRepeating([](views::View* view) { view->InvalidateLayout(); },
                          base::Unretained(this)));
  utility_separator_ = AddChildView(std::make_unique<ToolbarSeparator>());
  utility_ = AddChildView(
      std::make_unique<UtilityButtons>(browser, ChromePlacement::kToolbar));
  utility_->SetPaintToLayer();
  utility_->layer()->SetFillsBoundsOpaquely(false);
  utility_->SetContentChangedCallback(
      base::BindRepeating([](views::View* view) { view->InvalidateLayout(); },
                          base::Unretained(this)));
  assistant_separator_ = AddChildView(std::make_unique<ToolbarSeparator>());
  assistant_ = AddChildView(std::make_unique<AssistantButton>(browser));
  for (views::View* child : children()) {
    child->SetVisible(false);
  }
}

BrowserToolbar::~BrowserToolbar() {
  if (!matched_) {
    return;
  }
  for (views::View* child : children()) {
    matched_->Forget(child);
  }
}

void BrowserToolbar::SetTheme(const Theme& theme) {
  nav_->SetTheme(theme);
  pill_->SetTheme(theme);
  ext_->SetTheme(theme);
  utility_separator_->SetTheme(theme);
  utility_->SetTheme(theme);
  assistant_separator_->SetTheme(theme);
  assistant_->SetTheme(theme);
}

void BrowserToolbar::SetChromeLayout(const ChromeLayout& layout,
                                     bool assistant_shown,
                                     bool animated) {
  utility_->SetChromeLayout(layout, ext_->HasContent());
  if (layout_ == layout && assistant_shown_ == assistant_shown) {
    return;
  }
  if (animated) {
    transition_.Begin(motion::ChromeLayout());
  }
  layout_ = layout;
  assistant_shown_ = assistant_shown;
  InvalidateLayout();
}

void BrowserToolbar::Layout(PassKey) {
  if (!matched_) {
    return;
  }
  const int h = height();
  const int button = metrics::kToolButtonSize;
  const int y = (h - button) / 2;
  const bool row = layout_.toolbar();
  const bool nav = row && layout_.toolbar_navigation;
  const bool address = row && layout_.address_in_toolbar;
  const bool ext = row && layout_.extensions_in_toolbar && ext_->HasContent();
  const bool utility =
      row && layout_.toolbar_utilities() && utility_->HasContent();
  const bool assistant = row && assistant_shown_ &&
                         layout_.assistant_placement() ==
                             ChromeLayout::AssistantPlacement::kToolbar;

  // 从右往左：AI 助手、细线、工具按钮、细线、扩展按钮。
  int right = width();
  const gfx::Rect assistant_rect(right - button, y, button, button);
  if (assistant) {
    right -= button + kSpacing;
  }
  const bool assistant_line = assistant && ext;
  const gfx::Rect assistant_line_rect(right - ToolbarSeparator::kWidth, 0,
                                      ToolbarSeparator::kWidth, h);
  if (assistant_line) {
    right -= ToolbarSeparator::kWidth + kSpacing;
  }
  // 工具按钮在自己的区域里靠右排（显示、隐藏按钮时其余的滑过去，整组的右边不动）。
  const gfx::Rect utility_rect(0, y, std::max(0, right), button);
  if (utility) {
    right -= utility_->PreferredWidth() + kSpacing;
  }
  const bool utility_line = utility && ext;
  const gfx::Rect utility_line_rect(right - ToolbarSeparator::kWidth, 0,
                                    ToolbarSeparator::kWidth, h);
  if (utility_line) {
    right -= ToolbarSeparator::kWidth + kSpacing;
  }
  const int ext_width = ext_->PreferredWidth();
  const gfx::Rect ext_rect(right - ext_width, y, ext_width, button);
  if (ext) {
    right -= ext_width + kSpacing;
  }

  const int nav_width = NavigationButtons::Width();
  const gfx::Rect nav_rect(0, y, nav_width, button);
  const int x = nav ? nav_width + kSpacing : 0;
  const int pill_height = metrics::kAddressPillHeight;
  const gfx::Rect pill_rect(x, (h - pill_height) / 2, std::max(0, right - x),
                            pill_height);

  matched_->Place(MatchedGeometry::Id::kNavigation, nav_, nav_rect, nav);
  matched_->Place(MatchedGeometry::Id::kAddress, pill_, pill_rect, address);
  matched_->Place(MatchedGeometry::Id::kAssistant, assistant_, assistant_rect,
                  assistant);
  transition_.Place(ext_, ext_rect, ext);
  transition_.Place(utility_separator_, utility_line_rect, utility_line);
  transition_.Place(utility_, utility_rect, utility);
  transition_.Place(assistant_separator_, assistant_line_rect, assistant_line);
}

void BrowserToolbar::ChildPreferredSizeChanged(views::View* child) {
  InvalidateLayout();
}

BEGIN_METADATA(BrowserToolbar)
END_METADATA

}  // namespace starry
