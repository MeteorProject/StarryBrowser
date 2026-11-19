// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/toolbar/navigation_buttons.h"

#include <memory>

#include "base/functional/bind.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"

namespace starry {

NavigationButtons::NavigationButtons(BrowserWindowInterface* browser)
    : browser_(browser) {
  // 自己的图层：换布局时飞过去、淡入淡出。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  auto command = [](NavigationButtons* view, int id) {
    return base::BindRepeating(&NavigationButtons::RunCommand,
                               base::Unretained(view), id);
  };
  back_ = AddChildView(std::make_unique<IconButton>(
      command(this, IDC_BACK), icons::kChevronLeft, L("common.back")));
  forward_ = AddChildView(std::make_unique<IconButton>(
      command(this, IDC_FORWARD), icons::kChevronRight, L("common.forward")));
  reload_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&NavigationButtons::ReloadOrStop,
                          base::Unretained(this)),
      icons::kArrowClockwise, L("common.reload")));
  browser_->GetTabStripModel()->AddObserver(this);
  Update();
}

NavigationButtons::~NavigationButtons() {
  browser_->GetTabStripModel()->RemoveObserver(this);
}

// static
int NavigationButtons::Width() {
  return metrics::kToolButtonSize * 3 + kSpacing * 2;
}

void NavigationButtons::SetTheme(const Theme& theme) {
  for (IconButton* button : {back_.get(), forward_.get(), reload_.get()}) {
    button->SetTheme(theme);
  }
}

gfx::Size NavigationButtons::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(Width(), metrics::kToolButtonSize);
}

void NavigationButtons::Layout(PassKey) {
  const int size = metrics::kToolButtonSize;
  const int y = (height() - size) / 2;
  int x = 0;
  for (IconButton* button : {back_.get(), forward_.get(), reload_.get()}) {
    button->SetBounds(x, y, size, size);
    x += size + kSpacing;
  }
}

void NavigationButtons::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  Update();
}

void NavigationButtons::OnTabChangedAt(tabs::TabInterface* tab,
                                       TabChangeType change_type) {
  if (tab == browser_->GetActiveTabInterface()) {
    Update();
  }
}

void NavigationButtons::Update() {
  back_->SetEnabled(chrome::IsCommandEnabled(browser_, IDC_BACK));
  forward_->SetEnabled(chrome::IsCommandEnabled(browser_, IDC_FORWARD));
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  // 载入中时刷新按钮换成停止。
  const bool loading = tab && tab->GetContents()->IsLoading();
  if (loading != showing_stop_) {
    showing_stop_ = loading;
    reload_->SetIcon(loading ? icons::kXmark : icons::kArrowClockwise);
    reload_->SetTooltipText(loading ? L("common.stop") : L("common.reload"));
  }
}

void NavigationButtons::ReloadOrStop() {
  RunCommand(showing_stop_ ? IDC_STOP : IDC_RELOAD);
}

void NavigationButtons::RunCommand(int command) {
  chrome::ExecuteCommand(browser_, command);
}

BEGIN_METADATA(NavigationButtons)
END_METADATA

}  // namespace starry
