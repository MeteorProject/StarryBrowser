// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/mini_window/mini_window_toolbar.h"

#include <algorithm>
#include <memory>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/window_controls.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/mini_window/mini_window.h"
#include "starry/browser/ui/toolbar/address_pill.h"
#include "starry/browser/ui/toolbar/navigation_buttons.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace starry {

namespace {

// 控件间距；「移到浏览器」胶囊左侧额外留出 4 DIP。
constexpr int kSpacing = 4;
constexpr int kMoveLeading = 4;
constexpr int kAddressHeight = 28;
constexpr int kMoveHeight = 28;
// 地址栏在右侧给窗口拖动留白：至少保证地址栏有基本可读宽度，多余空间先留出拖动区
// （短标题时按内容收缩，右侧空白全可拖；长标题时仍至少留出 80–120 宽的拖动区）。
constexpr int kMinAddressWidth = 180;
constexpr int kMinDragWidth = 80;
constexpr int kMaxDragWidth = 120;

// 左边：macOS 让开红绿灯（同顶部标签行的第一个标签）；使用 Windows 自绘控件时左边无红绿灯。
int LeadingInset() {
#if BUILDFLAG(IS_MAC)
  return WindowsControlsEnabled() ? metrics::kContentInset
                                  : metrics::kTabStripLeading;
#else
  return metrics::kContentInset;
#endif
}

}  // namespace

MiniWindowToolbar::MiniWindowToolbar(BrowserWindowInterface* browser)
    : browser_(browser) {
  SetWindowDraggable(this, true);
  nav_ = AddChildView(std::make_unique<NavigationButtons>(browser));
  address_ = AddChildView(
      std::make_unique<AddressPill>(browser, AddressPill::Style::kCompact));
  address_->SetDragWindowOnDrag(true);
  const auto move = base::BindRepeating(&MiniWindowToolbar::MoveToBrowser,
                                        base::Unretained(this));
  move_ = AddChildView(std::make_unique<PillButton>(
      move, L("miniWindow.moveToBrowser"), ButtonVariant::kSecondary));
  move_->SetIcon(&icons::kArrowUpLeftAndArrowDownRight);
  move_->SetHeight(kMoveHeight);
  move_->SetTooltipText(L("miniWindow.moveToBrowser.help"));
  move_icon_ = AddChildView(std::make_unique<IconButton>(
      move, icons::kArrowUpLeftAndArrowDownRight,
      L("miniWindow.moveToBrowser.help"), metrics::kToolButtonSize,
      /*icon_size=*/12));
  move_icon_->SetVariant(ButtonVariant::kSecondary);
  move_icon_->GetViewAccessibility().SetName(L("miniWindow.moveToBrowser"));
  move_icon_->SetVisible(false);
}

MiniWindowToolbar::~MiniWindowToolbar() = default;

void MiniWindowToolbar::SetTheme(const Theme& theme) {
  nav_->SetTheme(theme);
  address_->SetTheme(theme);
  move_->SetTheme(theme);
  move_icon_->SetTheme(theme);
}

void MiniWindowToolbar::SetTrailingInset(int inset) {
  inset = std::max(0, inset);
  if (trailing_inset_ == inset) {
    return;
  }
  trailing_inset_ = inset;
  InvalidateLayout();
}

views::View* MiniWindowToolbar::move_button() {
  return move_icon_->GetVisible() ? static_cast<views::View*>(move_icon_)
                                  : static_cast<views::View*>(move_);
}

void MiniWindowToolbar::Layout(PassKey) {
  const int h = height();
  const int button = metrics::kToolButtonSize;
  // 窗口窄的时候「移到浏览器」只留图标，地址不至于挤没。
  const bool compact = width() < mini_window::kCompactWidth;
  move_->SetVisible(!compact);
  move_icon_->SetVisible(compact);

  int right = width() - metrics::kContentInset - trailing_inset_;
  if (compact) {
    move_icon_->SetBounds(right - button, (h - button) / 2, button, button);
    right -= button;
  } else {
    // 胶囊外面有一圈键盘选中的外圈：按钮比胶囊大，排的时候按胶囊对齐。
    const int outset = PillButton::FocusOutset();
    const gfx::Size size = move_->GetPreferredSize();
    move_->SetBounds(right - size.width() + outset, (h - size.height()) / 2,
                     size.width(), size.height());
    right -= size.width() - 2 * outset;
  }
  right -= kMoveLeading + kSpacing;

  const int x = LeadingInset();
  const int nav_width = NavigationButtons::Width();
  nav_->SetBounds(x, (h - button) / 2, nav_width, button);
  const int address_x = x + nav_width + kSpacing;
  const int available = std::max(0, right - address_x);
  const int drag_reserve = std::min(
      std::max(0, available - kMinAddressWidth),
      std::clamp(available / 4, kMinDragWidth, kMaxDragWidth));
  const int max_address = std::max(0, available - drag_reserve);
  const int address_width =
      std::min(address_->GetPreferredSize().width(), max_address);
  address_->SetBounds(address_x, (h - kAddressHeight) / 2, address_width,
                      kAddressHeight);
}

void MiniWindowToolbar::ChildPreferredSizeChanged(views::View* child) {
  InvalidateLayout();
}

void MiniWindowToolbar::MoveToBrowser() {
  // 会把自己所在的窗口关掉：不在按钮的点击回调里做，晚一步。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser) {
                       if (browser) {
                         mini_window::MoveToBrowser(browser.get());
                       }
                     },
                     browser_->GetWeakPtr()));
}

BEGIN_METADATA(MiniWindowToolbar)
END_METADATA

}  // namespace starry
