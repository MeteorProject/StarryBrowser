// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/sidebar_tabs.h"

#include <algorithm>

#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/controls/scroll_view.h"

namespace starry {

namespace {

constexpr int kPinnedBottomGap = 10;

}  // namespace

SidebarTabs::SidebarTabs(BrowserWindowInterface* browser)
    : TabsView(browser, TabListView::Style::kGrid, TabListView::Style::kList) {
  Sync(TabListView::Change::kNone);
}

SidebarTabs::~SidebarTabs() = default;

void SidebarTabs::Layout(PassKey) {
  const int pinned_height = is_dragging_tab() && !pinned()->slot_count()
                                ? metrics::kPinnedTileHeight
                                : pinned()->GetPreferredSize().height();
  pinned()->SetBounds(0, 0, width(), pinned_height);
  const int y = pinned_height > 0 ? pinned_height + kPinnedBottomGap : 0;
  scroll()->SetBounds(0, y, width(), std::max(0, height() - y));
  UpdateSticky();
}

void SidebarTabs::ChildPreferredSizeChanged(views::View* child) {
  InvalidateLayout();
}

std::optional<TabsView::DropTarget> SidebarTabs::DropTargetAt(
    const gfx::Point& point) const {
  if (std::optional<DropTarget> in_card; CardDropTargetAt(point, &in_card)) {
    return in_card;
  }
  // 拖到侧栏外面（网页上）：那是分屏。
  if (point.x() > width() + 24) {
    return std::nullopt;
  }
  if (pinned()->height() > 0) {
    gfx::Rect zone = pinned()->bounds();
    zone.Inset(gfx::Insets::TLBR(-8, -24, -kPinnedBottomGap / 2, -24));
    if (zone.Contains(point)) {
      gfx::Point local = point;
      views::View::ConvertPointToTarget(this, pinned(), &local);
      return GridDropTarget(local);
    }
  }
  // 图块文件夹只能在常用图块里排序。
  if (is_dragging_tile()) {
    return std::nullopt;
  }
  // 列表上面（图块和列表之间）算列表里看得见的最上面。
  gfx::Point local = point;
  views::View::ConvertPointToTarget(this, list(), &local);
  local.set_y(std::max(local.y(), scroll()->GetVisibleRect().y()));
  return ListDropTarget(local);
}

bool SidebarTabs::IsOverContent(const gfx::Point& point) const {
  if (card() && CardBounds().Contains(point)) {
    return false;
  }
  return point.x() > width();
}

std::optional<int> SidebarTabs::PreviewEdge(views::View* preview) const {
  // 侧栏面板的右边缘（浮动的侧栏比视图小一圈）。
  StarryBrowserView* view = StarryBrowserView::From(browser());
  if (!view || !view->sidebar()) {
    return std::nullopt;
  }
  SidebarView* sidebar = view->sidebar();
  return views::View::ConvertRectToTarget(sidebar, preview,
                                          sidebar->panel_rect())
      .right();
}

void SidebarTabs::RevealActive() {
  // 选中的标签不在视野里时滚过去（只滚到刚好看见）。
  if (TabCell* cell = list()->CellFor(
          model()->GetTabAtIndex(model()->active_index())->GetHandle())) {
    cell->ScrollViewToVisible();
  }
}

BEGIN_METADATA(SidebarTabs)
END_METADATA

}  // namespace starry
