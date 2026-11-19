// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TAB_TREE_H_
#define STARRY_BROWSER_UI_TABS_TAB_TREE_H_

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/prefs/pref_change_registrar.h"

class BrowserWindowInterface;
class Profile;
class TabStripModel;

namespace content {
class WebContents;
}

namespace tabs {
class TabInterface;
}

// 树状标签页：普通标签（固定的不算）挂在打开它的标签
// 下面，侧栏的列表里缩进、可以折叠。
//
// 父子和折叠记在每个标签的 WebContents 上（随标签走，跟着会话存），树的样子按
// 模型里的顺序算：每一枝在模型里连续、父标签在前（先序）。插入、挪动、关闭以后
// WindowTree 按规则修正，保持这样；Chrome 按下标做的事（⌘1…8、扩展的
// index、顶部标签行）都不用改。
namespace starry::tab_tree {

// 关掉展开着的父标签时（设置）。
enum class CloseParent {
  // 最外层的由第一个子标签接替，里层的子标签上移一级。
  kSmart = 0,
  // 子标签全部上移一级。
  kOutdent = 1,
  // 连同子标签一起关（只管在侧栏里关、⌘W；网页、扩展关掉的照 kSmart）。
  kCloseAll = 2,
};

// 设置里开着树状标签页（默认开）。
bool Enabled(Profile* profile);
// 侧栏里显示成树：开着，而且不是顶部标签布局（顶部标签行照常平铺）。
bool Shown(Profile* profile);
// 新标签最多挂到第几层（层数，0 是不限）。
int MaxDepth(Profile* profile);
CloseParent CloseParentMode(Profile* profile);
// 切换标签时折起其他组（设置，默认关）：只留当前标签所在的那一组展开。
bool AutoCollapse(Profile* profile);

// 模型里一个标签在树里的样子。固定标签不在树里（depth 0、没有父标签）。
struct Node {
  int depth = 0;
  // 父标签在模型里的下标；最外层是 -1。
  int parent = -1;
  // 直接的子标签几个、所有子孙几个。
  int children = 0;
  int descendants = 0;
  // 折着（有子标签时才算）；hidden：某一层祖先折着，侧栏里不显示。
  bool collapsed = false;
  bool hidden = false;
};
// 按模型现在的顺序算整棵树，下标同模型。
std::vector<Node> Compute(TabStripModel* model);
// 这一枝（index 和它的子孙）在模型里是 [index, BranchEnd)。
int BranchEnd(TabStripModel* model, int index);

// 折叠、展开 index 这个标签下面的；all：连同里面的各层一起。
void SetCollapsed(TabStripModel* model, int index, bool collapsed, bool all);
// 把 index 这一枝整个挪到 to 之前（to 是挪之前的下标，可以是 count）。挪完挂在
// parent（挪之前的下标，-1 是最外层）下面；没给 parent、或者那里挂不了时和下面
// 那一行同级。位置没变时只改挂。
void MoveBranch(TabStripModel* model,
                int index,
                int to,
                std::optional<int> parent = std::nullopt);
// 提升一级：整枝挪到父标签那一枝的后面，和父标签同级。
bool CanOutdent(TabStripModel* model, int index);
void Outdent(TabStripModel* model, int index);
// 作为上一个兄弟的最后一个子标签（位置不变）；到了层数上限不行。
bool CanIndent(TabStripModel* model, int index);
void Indent(TabStripModel* model, int index);
// 折起其他组：这个标签和当前标签所在的那一路以外、和这两路挨着的组都折起来
// （里面更深的不动，展开时还是原来的样子）。
bool CanCollapseOthers(TabStripModel* model, int index);
void CollapseOthers(TabStripModel* model, int index);
// 关掉 index 这一枝：从最后一个往前关，⌘⇧T 先恢复父标签；记下同一个记号，
// 恢复其中第一个时整组一起恢复。
void CloseBranch(TabStripModel* model, int index);
// 关掉 index 的子孙（同 CloseBranch，⌘⇧T 一次恢复），留下它自己。
void CloseChildren(TabStripModel* model, int index);
// 把 index 这一枝移到新窗口，父子和折叠都带过去。
void MoveBranchToNewWindow(BrowserWindowInterface* browser, int index);
// 在 index 这一枝后面开一个空白的新标签页，和它同级，切过去。
void NewTabBelow(BrowserWindowInterface* browser, int index);
// 按着 ⌥ 拖动只拖这一个：它的子标签留在原处、上移一级（挂到它的父标签下）。
// 返回原来的样子，取消、没有挤到别处时用 ReattachChildren 挂回去。
struct Detached {
  std::string id;
  std::vector<std::string> kids;
  bool collapsed = false;
};
Detached DetachChildren(TabStripModel* model, int index);
void ReattachChildren(TabStripModel* model, const Detached& detached);
// 关掉 index 这个标签时要不要整枝一起关：设置是一起关；在侧栏里关（from_sidebar）
// 折着的也是（它在列表里看起来就是一行）。⌘W 关折着的只关它一个，第一个子标签接替。
bool ClosesBranch(TabStripModel* model, int index, bool from_sidebar);

// 会话：记下、恢复父子和折叠（pinned_tabs 的 AddTabExtraData /
// RestoreTabExtraData 钩子里调用）。
void AddExtraData(content::WebContents* contents,
                  std::map<std::string, std::string>* extra_data);
void RestoreExtraData(content::WebContents* contents,
                      bool from_session_restore,
                      const std::map<std::string, std::string>& extra_data);

// 调试用：整棵树，比如 "A, [B: C, [D+: E]], F"（+ 是折着的）。
std::string DescribeForTesting(TabStripModel* model);

// 每个普通窗口一个（StarryBrowserView 建）：插入、挪动、关闭以后修正父子，当前
// 标签藏在折叠里时展开那一路。要在标签列表之前观察模型：列表刷新时树已经改好。
class WindowTree : public TabStripModelObserver {
 public:
  explicit WindowTree(BrowserWindowInterface* browser);
  WindowTree(const WindowTree&) = delete;
  WindowTree& operator=(const WindowTree&) = delete;
  ~WindowTree() override;

  static WindowTree* From(TabStripModel* model);

  // 树变了、设置变了（不是模型的改动带来的）：列表要刷新。
  base::CallbackListSubscription AddChangedCallback(
      base::RepeatingClosure callback);
  void NotifyChanged();

  // 挪一整枝的过程中不修正（MoveBranch 挪完自己修）。
  void set_moving(bool moving) { moving_ = moving; }
  // 接下来插进来的一个标签挂到 parent（ID，空的是最外层）下面（NewTabBelow）；
  // 放不下时同平常的插入。
  void ExpectParent(std::optional<std::string> parent) {
    expected_parent_ = std::move(parent);
  }

  // 聚焦一组（只看这一组）：侧栏的列表里只有 index 的子孙，从第 0 层排，顶上是
  // 面包屑。它没了、没有子标签了、固定了、当前标签换到这一组外面、不再显示成树时
  // 回到整棵树。只在这个窗口里，不存。
  bool CanHoist(int index) const;
  void Hoist(int index);
  void Unhoist();
  // 聚焦着的那个标签在模型里的下标；没有是 -1。
  int HoistedIndex() const;
  // 上一次模型的通知里折起了别的组（切换标签时折起其他组）：列表用开合的动画。
  bool TakeFolded() { return std::exchange(folded_, false); }

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) override;

 private:
  void Inserted(content::WebContents* contents, int index);
  void Moved(content::WebContents* contents, int to);
  // 当前标签在折起来的一组里：展开那一路。
  void RevealActive();

  const raw_ptr<TabStripModel> model_;
  PrefChangeRegistrar prefs_;
  base::RepeatingClosureList changed_;
  bool moving_ = false;
  std::optional<std::string> expected_parent_;
  // 聚焦着的标签的 ID；空的是没有。
  std::string hoisted_;
  bool folded_ = false;
};

}  // namespace starry::tab_tree

#endif  // STARRY_BROWSER_UI_TABS_TAB_TREE_H_
