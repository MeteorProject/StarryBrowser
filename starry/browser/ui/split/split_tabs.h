// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SPLIT_SPLIT_TABS_H_
#define STARRY_BROWSER_UI_SPLIT_SPLIT_TABS_H_

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "components/split_tabs/split_tab_id.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/components/split_layout/split_layout.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"

class BrowserWindowInterface;
class TabStripModel;

namespace gfx {
struct VectorIcon;
}

// 最多三格的分屏。标签集合、会话和扩展沿用 Chrome 的 split 机制。
namespace starry::split {

// 两格之间的空隙。
inline constexpr int kGap = 8;
// 拖分隔线时一格的最小宽度、高度；再往里拖有阻力，多拖 kCollapseDistance 松手就把那一格
// 移出分屏。
inline constexpr int kPaneMinWidth = 240;
inline constexpr int kPaneMinHeight = 160;
inline constexpr int kCollapseDistance = 72;

// 把标签拖到一格的哪一边：新的一格放在那一边。
enum class Edge { kLeft, kRight, kTop, kBottom };

// 鼠标在格子里的位置离哪条边最近（按相对距离，相当于用两条对角线把格子分成四块）。
Edge NearestEdge(const gfx::Point& point, const gfx::Rect& rect);
bool IsLeading(Edge edge);
split_tabs::SplitTabLayout LayoutFor(Edge edge);
// 「放在左边」……（落点预览、右键菜单）。
std::u16string PlacementTitle(Edge edge);
const gfx::VectorIcon& PlacementIcon(Edge edge);

// 拖到某一格的某一边以后的布局。
struct Preview {
  // 按阅读顺序（先左后右、先上后下）。
  std::vector<tabs::TabHandle> tabs;
  Layout layout;
  bool operator==(const Preview&) const = default;
};

enum class Refusal { kFull, kOtherWindow };

struct Drop {
  tabs::TabHandle dragged;
  tabs::TabHandle target;
  Edge edge = Edge::kRight;
  std::optional<Preview> preview;
  std::optional<Refusal> refusal;
  bool operator==(const Drop&) const = default;
};

// 把 dragged 拖到 target 那一格的 edge 边上会怎样：
// - target 不在分屏里：两个一起分屏，各占一半。
// - target 在分屏里、dragged 是另一格：换位置（换边、换方向）。
// - target 在分屏里、dragged 不在那一组：插入第三格；已有三格时拒绝。
// 拖到自己身上时 preview、refusal 都为空。
Drop Evaluate(TabStripModel* model,
              tabs::TabInterface* dragged,
              tabs::TabInterface* target,
              Edge edge);

// 按 Evaluate 的结果分屏。dragged 原来在别的分屏里时先从那里拿出来；完了 dragged 成为
// 当前标签。
bool Apply(BrowserWindowInterface* browser, const Drop& drop);

// 和 tab 在同一组的另一格。
tabs::TabInterface* Partner(TabStripModel* model, tabs::TabInterface* tab);

// 把 tab 移出分屏（分隔线拖到头、把手菜单、右键菜单）。只剩一格时整组解散；当前标签被
// 移出时焦点交给另一格。标签只是离开分屏，不关闭。
void RemoveFromSplit(BrowserWindowInterface* browser, tabs::TabInterface* tab);
// 解散 tab 所在的分屏，只留它显示。
void ShowOnly(BrowserWindowInterface* browser, tabs::TabInterface* tab);
// 双击分隔线、把手菜单：每一层平分。
void Equalize(BrowserWindowInterface* browser, split_tabs::SplitTabId split);
// 拖分隔线：start 那一格占的比例。done 时写进会话。
void SetRatio(BrowserWindowInterface* browser,
              split_tabs::SplitTabId split,
              double ratio,
              bool done);

Layout GetLayout(TabStripModel* model, split_tabs::SplitTabId split);
void SetLayout(BrowserWindowInterface* browser,
               split_tabs::SplitTabId split,
               const Layout& layout,
               bool done);
// Native detach path: restore the surviving pair after the model transaction.
void PreserveRemainingPanes(TabStripModel* model, tabs::TabInterface* removing);

}  // namespace starry::split

#endif  // STARRY_BROWSER_UI_SPLIT_SPLIT_TABS_H_
