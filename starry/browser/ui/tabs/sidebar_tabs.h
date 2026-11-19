// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_SIDEBAR_TABS_H_
#define STARRY_BROWSER_UI_TABS_SIDEBAR_TABS_H_

#include <optional>

#include "starry/browser/ui/tabs/tabs_view.h"
#include "ui/base/metadata/metadata_header_macros.h"

class BrowserWindowInterface;

namespace starry {

// 侧栏的常用图块和可滚动的标签列表。
class SidebarTabs : public TabsView {
  METADATA_HEADER(SidebarTabs, TabsView)

 public:
  explicit SidebarTabs(BrowserWindowInterface* browser);
  SidebarTabs(const SidebarTabs&) = delete;
  SidebarTabs& operator=(const SidebarTabs&) = delete;
  ~SidebarTabs() override;

  // views::View:
  void Layout(PassKey) override;
  void ChildPreferredSizeChanged(views::View* child) override;

 protected:
  // TabsView:
  std::optional<DropTarget> DropTargetAt(
      const gfx::Point& point) const override;
  bool IsOverContent(const gfx::Point& point) const override;
  std::optional<int> PreviewEdge(views::View* preview) const override;
  void RevealActive() override;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_SIDEBAR_TABS_H_
