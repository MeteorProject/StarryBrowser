// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FRAME_CHROME_LAYOUT_H_
#define STARRY_BROWSER_UI_FRAME_CHROME_LAYOUT_H_

#include <optional>

#include "ui/gfx/geometry/point.h"

class PrefService;

namespace starry {

// 标签栏位置与侧栏形态。
enum class TabLayout {
  // 竖排标签的侧栏，贴着窗口左侧。
  kDockedSidebar,
  // 自动隐藏的浮动玻璃侧栏：平时收起，鼠标移到窗口左边缘时盖在网页上滑出。
  kFloatingSidebar,
  // 横排标签在窗口顶部，下面一行是地址栏和工具按钮。
  kTop,
};

// 地址栏、扩展按钮、工具按钮这类控件放在侧栏里，还是内容卡片上方的工具栏（或紧凑地址栏）
// 里。
enum class ChromePlacement { kSidebar, kToolbar };

// 窗口外壳布局：根据标签布局、自动隐藏、地址栏和扩展按钮的位置，计算各区域的显示状态
// 与位置；切换时通过 chromeLayout 动效过渡。
struct ChromeLayout {
  TabLayout tab_layout = TabLayout::kDockedSidebar;
  // 侧栏自动隐藏：与浮动侧栏（kFloatingSidebar）等价。
  bool auto_hide = false;
  // 地址栏在工具栏里（顶部标签布局总是）。
  bool address_in_toolbar = false;
  // 扩展按钮在工具栏里，并且窗口里有可以显示的扩展。
  bool extensions_in_toolbar = false;
  // 导航按钮跟着地址栏进了工具栏（侧栏顶栏就不再放）：顶部标签布局，或浮动侧栏把地址栏
  // 放在顶部时。固定侧栏的顶栏和工具栏连成一行，导航按钮留在红绿灯旁边。
  bool toolbar_navigation = false;
  // 紧凑地址栏：工具栏不单独占一行，里面的东西并进内容卡片顶部的细栏。
  bool compact = false;

  // 读设置。has_extensions：窗口里有带按钮的扩展。
  static ChromeLayout FromPrefs(const PrefService* prefs, bool has_extensions);
  // 只读标签布局。
  static TabLayout TabLayoutFromPrefs(const PrefService* prefs);

  bool is_sidebar() const { return tab_layout != TabLayout::kTop; }
  // 侧栏占一列，内容在它右边。
  bool sidebar_column() const { return is_sidebar() && !auto_hide; }
  // 侧栏自动隐藏：平时收起，滑出时盖在网页上。
  bool overlay_sidebar() const { return is_sidebar() && auto_hide; }
  bool floating() const { return tab_layout == TabLayout::kFloatingSidebar; }
  bool tab_strip() const { return tab_layout == TabLayout::kTop; }
  // 内容卡片上方单独一行的工具栏。
  bool toolbar() const {
    return (address_in_toolbar || extensions_in_toolbar || tab_strip()) &&
           !compact;
  }
  // 内容卡片离上面的距离：紧挨工具栏时不留（工具栏自己有上下的空）；紧凑地址栏配顶部标
  // 签时，标签行下边本来就有空，只留一点。
  int content_top_inset() const;
  // 下载、历史记录、设置这些按钮平时在侧栏底部，顶部标签布局下放在工具栏右端。
  bool toolbar_utilities() const { return tab_strip(); }

  // AI
  // 助手的按钮放在哪：有横向的顶栏就在窗口右上角，标签行优先，其次是地址栏那一行（工
  // 具栏或紧凑地址栏，扩展按钮旁边）；都没有时留在侧栏底部的工具按钮里。
  enum class AssistantPlacement { kTabStrip, kToolbar, kSidebar };
  AssistantPlacement assistant_placement() const;

  // 内容卡片（和工具栏）左边的留白：贴着固定侧栏时不留，侧栏自己有内边距。
  int content_leading() const;
  // 浮动侧栏的面板离窗口顶部有一段距离，工具栏跟着下移，和面板的顶栏对齐。
  int toolbar_top_inset() const;
  // 侧栏顶栏（红绿灯那一行）的高度，panel_inset
  // 是侧栏面板离窗口顶边的距离。用紧凑地址
  // 栏时跟着细栏走，中线对齐：固定侧栏的顶栏比平时高，浮动面板的顶栏和细栏一样高。
  int SidebarHeaderHeight(int panel_inset) const;
  // 红绿灯（关闭按钮左上角）相对窗口左上角的位置，空表示藏起来（自动隐藏的侧栏收起时）。
  // 侧栏里和顶栏的导航按钮同一行，顶部标签布局下在标签行里居中。
  std::optional<gfx::Point> WindowButtonsOrigin(bool peeking) const;

  bool operator==(const ChromeLayout&) const = default;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FRAME_CHROME_LAYOUT_H_
