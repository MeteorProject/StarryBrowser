// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tab_tree.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/sessions/core/tab_restore_types.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/starry_hooks.h"
#include "url/gurl.h"

namespace starry::tab_tree {

namespace {

// 会话里每个标签的 extra_data 的键。
constexpr char kIdKey[] = "starry.tree.id";
constexpr char kParentKey[] = "starry.tree.parent";
constexpr char kCollapsedKey[] = "starry.tree.collapsed";
// 一起关掉的一组：⌘⇧T 恢复其中第一个时，后面同一个记号的接着恢复。
constexpr char kBranchKey[] = "starry.tree.branch";

std::string NewId() {
  return base::NumberToString(base::RandUint64());
}

// 一个标签在树里的数据，挂在 WebContents 上。
class TreeState : public content::WebContentsUserData<TreeState> {
 public:
  ~TreeState() override = default;

  // 这个标签的 ID（会话里存着，子标签用它找父标签）。
  std::string id = NewId();
  // 父标签的 ID；空的是最外层。
  std::string parent;
  bool collapsed = false;
  // 网页里的链接打开它的时候，打开它的那个标签的 ID（插入时用，插完清掉）。
  std::string source;
  // 一起关掉的一组的记号。
  std::string branch;
  // 整枝关掉的过程中：子标签先关掉了也不清掉「折着」，⌘⇧T 恢复时还折着。
  bool closing = false;

 private:
  explicit TreeState(content::WebContents* contents)
      : content::WebContentsUserData<TreeState>(*contents) {}
  friend class content::WebContentsUserData<TreeState>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(TreeState);

// 离开树的标签那一刻的样子（关掉、挪走、固定时按它改挂子标签）。
struct Info {
  std::string id;
  std::string parent;
  bool collapsed = false;
};

TreeState* State(content::WebContents* contents) {
  return TreeState::GetOrCreateForWebContents(contents);
}

TreeState* StateAt(TabStripModel* model, int index) {
  return State(model->GetWebContentsAt(index));
}

Info InfoOf(const TreeState* state) {
  return {state->id, state->parent, state->collapsed};
}

std::map<TabStripModel*, WindowTree*>& Registry() {
  static base::NoDestructor<std::map<TabStripModel*, WindowTree*>> registry;
  return *registry;
}

PrefService* Prefs(Profile* profile) {
  return profile ? prefs::PrefsFor(profile) : nullptr;
}

// 正在整枝移到别的窗口的标签（MoveBranchToNewWindow）：Chrome 一个一个拿走，先拿
// 走的父标签不把还没拿走的子标签改挂。
std::set<content::WebContents*>& Departing() {
  static base::NoDestructor<std::set<content::WebContents*>> departing;
  return *departing;
}

// 写进会话（无痕窗口没有会话，不写）。重写会话文件时 AddExtraData 也会记。
void SaveInSession(content::WebContents* contents) {
  SessionService* service = SessionServiceFactory::GetForProfileIfExisting(
      Profile::FromBrowserContext(contents->GetBrowserContext()));
  if (!service) {
    return;
  }
  const SessionID window_id =
      sessions::SessionTabHelper::IdForWindowContainingTab(contents);
  const SessionID tab_id = sessions::SessionTabHelper::IdForTab(contents);
  if (!window_id.is_valid() || !tab_id.is_valid()) {
    return;
  }
  const TreeState* state = State(contents);
  service->AddTabExtraData(window_id, tab_id, kIdKey, state->id);
  service->AddTabExtraData(window_id, tab_id, kParentKey, state->parent);
  service->AddTabExtraData(window_id, tab_id, kCollapsedKey,
                           state->collapsed ? "1" : "");
}

void SetParent(content::WebContents* contents, const std::string& parent) {
  TreeState* state = State(contents);
  if (state->parent != parent) {
    state->parent = parent;
    SaveInSession(contents);
  }
}

void SetCollapsedState(content::WebContents* contents, bool collapsed) {
  TreeState* state = State(contents);
  if (state->collapsed != collapsed) {
    state->collapsed = collapsed;
    SaveInSession(contents);
  }
}

// 模型里 ID 是 id 的普通标签；没有是 -1。
int IndexOfId(TabStripModel* model, const std::string& id) {
  if (id.empty()) {
    return -1;
  }
  for (int i = model->IndexOfFirstNonPinnedTab(); i < model->count(); ++i) {
    if (StateAt(model, i)->id == id) {
      return i;
    }
  }
  return -1;
}

// 按模型的顺序算树，跳过 [skip_begin, skip_end) 这几个（插入、挪动的标签还没
// 定父标签时，看它不在的时候树是什么样）。父标签不是前面某一行的祖先（顺序被
// 打乱了）的当成最外层。
std::vector<Node> ComputeSkipping(TabStripModel* model,
                                  int skip_begin,
                                  int skip_end) {
  const int count = model->count();
  std::vector<Node> nodes(count);
  std::vector<std::string> ids(count);
  std::vector<int> stack;
  const int first = model->IndexOfFirstNonPinnedTab();
  auto skipped = [&](int i) { return i >= skip_begin && i < skip_end; };
  for (int i = first; i < count; ++i) {
    if (skipped(i)) {
      continue;
    }
    const TreeState* state = StateAt(model, i);
    ids[i] = state->id;
    while (!stack.empty() && ids[stack.back()] != state->parent) {
      stack.pop_back();
    }
    nodes[i].parent = stack.empty() ? -1 : stack.back();
    nodes[i].depth = static_cast<int>(stack.size());
    nodes[i].collapsed = state->collapsed;
    stack.push_back(i);
  }
  for (int i = first; i < count; ++i) {
    if (skipped(i)) {
      continue;
    }
    if (nodes[i].parent >= 0) {
      ++nodes[nodes[i].parent].children;
    }
    for (int p = nodes[i].parent; p >= 0; p = nodes[p].parent) {
      ++nodes[p].descendants;
    }
  }
  // 父标签在前：按顺序走一遍就能从上往下传「藏着」。
  for (int i = first; i < count; ++i) {
    Node& node = nodes[i];
    node.collapsed = node.collapsed && node.children > 0;
    const int p = node.parent;
    node.hidden = p >= 0 && (nodes[p].collapsed || nodes[p].hidden);
  }
  return nodes;
}

int BranchEndIn(const std::vector<Node>& nodes, int index) {
  int end = index + 1;
  while (end < static_cast<int>(nodes.size()) &&
         nodes[end].depth > nodes[index].depth) {
    ++end;
  }
  return end;
}

// [begin, end) 这一枝（已经在模型里）挂到哪。wanted 是想要的父标签（空的是最外
// 层），放得下就照它；没有 wanted 或者放不下时和下面那一行同级（下面没有了就是
// 最外层）。
std::string ParentFor(TabStripModel* model,
                      int begin,
                      int end,
                      std::optional<std::string> wanted) {
  const std::vector<Node> nodes = ComputeSkipping(model, begin, end);
  const int first = model->IndexOfFirstNonPinnedTab();
  const int above = begin - 1 >= first ? begin - 1 : -1;
  const int below = end < model->count() ? end : -1;
  const int below_depth = below >= 0 ? nodes[below].depth : 0;
  const std::string below_parent = below >= 0 && nodes[below].parent >= 0
                                       ? StateAt(model, nodes[below].parent)->id
                                       : std::string();
  if (!wanted) {
    return below_parent;
  }
  if (wanted->empty()) {
    // 放在最外层：下面那一行也得是最外层，不然把它那一组拦腰截断了。
    return below_depth == 0 ? std::string() : below_parent;
  }
  // wanted 得是上面那一行自己或者它的祖先，而且不比下面那一行的父标签浅。
  for (int p = above; p >= 0; p = nodes[p].parent) {
    if (StateAt(model, p)->id == *wanted) {
      return below_depth - 1 <= nodes[p].depth ? *wanted : below_parent;
    }
  }
  return below_parent;
}

// 没有子标签的标签不算折着（不然以后在后台挂上来的子标签一出来就藏着）。
void ClearIfChildless(TabStripModel* model, const std::string& id) {
  const int index = IndexOfId(model, id);
  if (index < 0 || StateAt(model, index)->closing) {
    return;
  }
  for (int i = model->IndexOfFirstNonPinnedTab(); i < model->count(); ++i) {
    if (StateAt(model, i)->parent == id) {
      return;
    }
  }
  SetCollapsedState(model->GetWebContentsAt(index), false);
}

// 一个标签离开了树（关掉、挪到别的窗口）：它的子标签按设置改挂。在最外层时
// 第一个子标签接替（折着的接着折着），不然子标签上移一级。
void PromoteChildren(TabStripModel* model, const Info& gone) {
  std::vector<content::WebContents*> kids;
  for (int i = model->IndexOfFirstNonPinnedTab(); i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    if (State(contents)->id != gone.id && State(contents)->parent == gone.id &&
        !Departing().contains(contents)) {
      kids.push_back(contents);
    }
  }
  if (kids.empty()) {
    ClearIfChildless(model, gone.parent);
    return;
  }
  const bool root = gone.parent.empty() || IndexOfId(model, gone.parent) < 0;
  if (root && CloseParentMode(model->profile()) != CloseParent::kOutdent) {
    SetParent(kids[0], std::string());
    for (size_t k = 1; k < kids.size(); ++k) {
      SetParent(kids[k], State(kids[0])->id);
    }
    if (gone.collapsed && kids.size() > 1) {
      SetCollapsedState(kids[0], true);
    }
    return;
  }
  for (content::WebContents* kid : kids) {
    SetParent(kid, root ? std::string() : gone.parent);
  }
}

// ⌘⇧T 恢复一起关掉的一组：这一组恢复了哪几个（第一个是父标签）、关掉时折没折着
// （恢复一个切过去一个，路上会展开，恢复完再折回去）。
struct BranchRestore {
  std::vector<std::pair<base::WeakPtr<content::WebContents>, bool>> tabs;
};

std::map<std::string, BranchRestore>& Restoring() {
  static base::NoDestructor<std::map<std::string, BranchRestore>> restoring;
  return *restoring;
}

// 恢复了一组里的一个（last）：最近关掉的下一个还是这一组的就接着恢复；恢复完
// 切回这一组的第一个。
void ContinueBranchRestore(std::string token,
                           base::WeakPtr<content::WebContents> last) {
  const BranchRestore restore = Restoring()[token];
  base::WeakPtr<content::WebContents> first =
      restore.tabs.empty() ? nullptr : restore.tabs.front().first;
  content::WebContents* anchor = last ? last.get() : first.get();
  tabs::TabInterface* tab =
      anchor ? tabs::TabInterface::MaybeGetFromContents(anchor) : nullptr;
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser) {
    Restoring().erase(token);
    return;
  }
  sessions::TabRestoreService* service =
      TabRestoreServiceFactory::GetForProfile(browser->GetProfile());
  if (service && !service->entries().empty()) {
    const sessions::tab_restore::Entry& entry = *service->entries().front();
    auto it = entry.extra_data.find(kBranchKey);
    if (entry.type == sessions::tab_restore::Type::TAB &&
        it != entry.extra_data.end() && it->second == token) {
      chrome::RestoreTab(browser);
      return;
    }
  }
  Restoring().erase(token);
  TabStripModel* model = browser->GetTabStripModel();
  if (first) {
    const int index = model->GetIndexOfWebContents(first.get());
    if (index != TabStripModel::kNoTab) {
      model->ActivateTabAt(index);
    }
  }
  for (const auto& [contents, collapsed] : restore.tabs) {
    if (contents && collapsed) {
      SetCollapsedState(contents.get(), true);
    }
  }
  if (WindowTree* tree = WindowTree::From(model)) {
    tree->NotifyChanged();
  }
}

// 当前标签藏在折叠里：展开那一路。
void RevealActiveIn(TabStripModel* model) {
  const int active = model->active_index();
  if (active < model->IndexOfFirstNonPinnedTab() || active >= model->count()) {
    return;
  }
  const std::vector<Node> nodes = ComputeSkipping(model, -1, -1);
  if (!nodes[active].hidden) {
    return;
  }
  for (int p = nodes[active].parent; p >= 0; p = nodes[p].parent) {
    if (nodes[p].collapsed) {
      SetCollapsedState(model->GetWebContentsAt(p), false);
    }
  }
}

// index 是树里的一个标签（开着、不是固定的）。
bool InTree(TabStripModel* model, int index) {
  return Enabled(model->profile()) &&
         index >= model->IndexOfFirstNonPinnedTab() && index < model->count();
}

// 上一个兄弟（同一个父标签、排在前面最近的）；没有是 -1。
int PreviousSibling(const std::vector<Node>& nodes, int index, int first) {
  for (int i = index - 1; i >= first && nodes[i].depth >= nodes[index].depth;
       --i) {
    if (nodes[i].parent == nodes[index].parent) {
      return i;
    }
  }
  return -1;
}

// 折起其他组时要折起来的：index 和当前标签所在的两路（祖先和它自己）以外，
// 父标签在这两路上（或者是最外层）、展开着的组。这两个标签自己那一枝里面的不动。
std::vector<int> OtherGroups(TabStripModel* model, int index) {
  const std::vector<Node> nodes = ComputeSkipping(model, -1, -1);
  const int first = model->IndexOfFirstNonPinnedTab();
  const int count = model->count();
  std::vector<bool> path(count, false);
  std::vector<bool> inside(count, false);
  for (const int start : {index, model->active_index()}) {
    if (start < first || start >= count) {
      continue;
    }
    for (int p = start; p >= 0; p = nodes[p].parent) {
      path[p] = true;
    }
    for (int i = start + 1; i < BranchEndIn(nodes, start); ++i) {
      inside[i] = true;
    }
  }
  std::vector<int> groups;
  for (int i = first; i < count; ++i) {
    const Node& node = nodes[i];
    if (node.children > 0 && !node.collapsed && !path[i] && !inside[i] &&
        (node.parent < 0 || path[node.parent])) {
      groups.push_back(i);
    }
  }
  return groups;
}

// 从最后一个往前关掉 [begin, end)，记下同一个记号（⌘⇧T 一次整组恢复）。
void CloseRange(TabStripModel* model, int begin, int end) {
  const std::string token = NewId();
  std::vector<base::WeakPtr<content::WebContents>> branch;
  for (int i = begin; i < end; ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    State(contents)->branch = token;
    State(contents)->closing = true;
    branch.push_back(contents->GetWeakPtr());
  }
  // 从最后一个往前关：⌘⇧T 先恢复父标签，子标签恢复时父标签已经在了。
  for (auto it = branch.rbegin(); it != branch.rend(); ++it) {
    if (!*it) {
      continue;
    }
    const int i = model->GetIndexOfWebContents(it->get());
    if (i != TabStripModel::kNoTab) {
      model->CloseWebContentsAt(i,
                                TabCloseTypes::CLOSE_USER_GESTURE |
                                    TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
    }
  }
  // 网页问了「确定离开」、用户选了留下的：不再算在一组里关。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::vector<base::WeakPtr<content::WebContents>> branch) {
            for (const auto& contents : branch) {
              if (contents) {
                State(contents.get())->closing = false;
              }
            }
          },
          std::move(branch)));
}

void NotifyChangedIn(TabStripModel* model) {
  if (WindowTree* tree = WindowTree::From(model)) {
    tree->NotifyChanged();
  }
}

std::string ShortTitle(content::WebContents* contents) {
  std::string title = base::UTF16ToUTF8(contents->GetTitle());
  if (const size_t space = title.find(' '); space != std::string::npos) {
    title.resize(space);
  }
  return title.empty() ? "?" : title;
}

}  // namespace

bool Enabled(Profile* profile) {
  PrefService* prefs = Prefs(profile);
  return prefs && prefs->GetBoolean(prefs::kTreeTabs);
}

bool Shown(Profile* profile) {
  PrefService* prefs = Prefs(profile);
  return Enabled(profile) &&
         prefs->GetString(prefs::kSidebarLayout) != prefs::kSidebarLayoutTop;
}

int MaxDepth(Profile* profile) {
  PrefService* prefs = Prefs(profile);
  return prefs ? std::max(0, prefs->GetInteger(prefs::kTreeTabsMaxDepth)) : 0;
}

CloseParent CloseParentMode(Profile* profile) {
  PrefService* prefs = Prefs(profile);
  const int value =
      prefs ? std::clamp(prefs->GetInteger(prefs::kTreeTabsCloseParent), 0, 2)
            : 0;
  return static_cast<CloseParent>(value);
}

bool AutoCollapse(Profile* profile) {
  PrefService* prefs = Prefs(profile);
  return prefs && prefs->GetBoolean(prefs::kTreeTabsAutoCollapse);
}

std::vector<Node> Compute(TabStripModel* model) {
  return ComputeSkipping(model, -1, -1);
}

int BranchEnd(TabStripModel* model, int index) {
  if (index < model->IndexOfFirstNonPinnedTab()) {
    return index + 1;
  }
  return BranchEndIn(Compute(model), index);
}

void SetCollapsed(TabStripModel* model, int index, bool collapsed, bool all) {
  if (index < model->IndexOfFirstNonPinnedTab() || index >= model->count()) {
    return;
  }
  const std::vector<Node> nodes = Compute(model);
  if (nodes[index].children == 0) {
    return;
  }
  const int end = BranchEndIn(nodes, index);
  SetCollapsedState(model->GetWebContentsAt(index), collapsed);
  if (all) {
    for (int i = index + 1; i < end; ++i) {
      if (nodes[i].children > 0) {
        SetCollapsedState(model->GetWebContentsAt(i), collapsed);
      }
    }
  }
  // 折叠操作覆盖到当前活动标签时，激活该父标签。
  const int active = model->active_index();
  if (collapsed && active > index && active < end) {
    model->ActivateTabAt(index);
  }
  NotifyChangedIn(model);
}

void MoveBranch(TabStripModel* model,
                int index,
                int to,
                std::optional<int> parent) {
  const int first = model->IndexOfFirstNonPinnedTab();
  if (index < first || index >= model->count()) {
    return;
  }
  const int end = BranchEnd(model, index);
  // 挂到自己这一枝里面：不行。
  if (parent && *parent >= index && *parent < end) {
    return;
  }
  std::optional<std::string> wanted;
  if (parent) {
    wanted = *parent >= first && *parent < model->count()
                 ? StateAt(model, *parent)->id
                 : std::string();
  }
  const bool in_place = to >= index && to <= end;
  if (in_place && !wanted) {
    return;
  }
  std::vector<content::WebContents*> branch;
  for (int i = index; i < end; ++i) {
    // 分屏得整组挪，这里不管：只挪这一个，它的子标签按挪动的规则改挂。
    if (!in_place && model->GetTabAtIndex(i)->GetSplit()) {
      model->MoveWebContentsAt(index, to > index ? to - 1 : to,
                               /*select_after_move=*/false);
      return;
    }
    branch.push_back(model->GetWebContentsAt(i));
  }
  WindowTree* tree = WindowTree::From(model);
  const std::string old_parent = State(branch[0])->parent;
  if (!in_place) {
    content::WebContents* anchor =
        to < model->count() ? model->GetWebContentsAt(to) : nullptr;
    if (tree) {
      tree->set_moving(true);
    }
    for (content::WebContents* contents : branch) {
      const int from = model->GetIndexOfWebContents(contents);
      int dest = anchor ? model->GetIndexOfWebContents(anchor) : model->count();
      if (from < dest) {
        --dest;
      }
      if (from != dest) {
        model->MoveWebContentsAt(from, dest, /*select_after_move=*/false);
      }
    }
    if (tree) {
      tree->set_moving(false);
    }
  }
  // 挂到想要的父标签下；没有、挂不了时挪到哪就认哪：和下面那一行同级。
  const int begin = model->GetIndexOfWebContents(branch[0]);
  SetParent(
      branch[0],
      ParentFor(model, begin, begin + static_cast<int>(branch.size()), wanted));
  ClearIfChildless(model, old_parent);
  // 挂进了折着的一组：当前标签在里面时展开那一路。
  RevealActiveIn(model);
  // 挪的时候列表已经按中间的样子刷新过了：父标签定下来以后再刷一次。
  if (tree) {
    tree->NotifyChanged();
  }
}

bool CanOutdent(TabStripModel* model, int index) {
  return InTree(model, index) && Compute(model)[index].parent >= 0;
}

void Outdent(TabStripModel* model, int index) {
  if (!CanOutdent(model, index)) {
    return;
  }
  const std::vector<Node> nodes = Compute(model);
  const int parent = nodes[index].parent;
  MoveBranch(model, index, BranchEndIn(nodes, parent), nodes[parent].parent);
}

bool CanIndent(TabStripModel* model, int index) {
  if (!InTree(model, index)) {
    return false;
  }
  const std::vector<Node> nodes = Compute(model);
  const int limit = MaxDepth(model->profile());
  return PreviousSibling(nodes, index, model->IndexOfFirstNonPinnedTab()) >=
             0 &&
         (limit == 0 || nodes[index].depth + 2 <= limit);
}

void Indent(TabStripModel* model, int index) {
  if (!CanIndent(model, index)) {
    return;
  }
  // 上一个兄弟那一枝正好在它前面结束：不用挪，改挂就是它的最后一个子标签。
  const int sibling =
      PreviousSibling(Compute(model), index, model->IndexOfFirstNonPinnedTab());
  content::WebContents* up = model->GetWebContentsAt(sibling);
  SetParent(model->GetWebContentsAt(index), State(up)->id);
  // 挂进折着的一组：展开，看得见它去了哪。
  SetCollapsedState(up, false);
  NotifyChangedIn(model);
}

bool CanCollapseOthers(TabStripModel* model, int index) {
  return InTree(model, index) && !OtherGroups(model, index).empty();
}

void CollapseOthers(TabStripModel* model, int index) {
  if (!InTree(model, index)) {
    return;
  }
  for (const int group : OtherGroups(model, index)) {
    SetCollapsedState(model->GetWebContentsAt(group), true);
  }
  NotifyChangedIn(model);
}

bool ClosesBranch(TabStripModel* model, int index, bool from_sidebar) {
  Profile* profile = model->profile();
  if (!Enabled(profile) || index < model->IndexOfFirstNonPinnedTab() ||
      index >= model->count()) {
    return false;
  }
  const std::vector<Node> nodes = Compute(model);
  return nodes[index].children > 0 &&
         ((from_sidebar && nodes[index].collapsed) ||
          CloseParentMode(profile) == CloseParent::kCloseAll);
}

void CloseBranch(TabStripModel* model, int index) {
  CloseRange(model, index, BranchEnd(model, index));
}

void CloseChildren(TabStripModel* model, int index) {
  if (!InTree(model, index)) {
    return;
  }
  const int end = BranchEnd(model, index);
  if (end <= index + 1) {
    return;
  }
  // 当前标签在里面：先切到它（不然一个一个关的时候会轮着选中还没关的子标签）。
  content::WebContents* contents = model->GetWebContentsAt(index);
  const int active = model->active_index();
  if (active > index && active < end) {
    model->ActivateTabAt(index);
  }
  CloseRange(model, index + 1, end);
  // ⌘⇧T 恢复的子标签展开着。
  SetCollapsedState(contents, false);
  NotifyChangedIn(model);
}

void MoveBranchToNewWindow(BrowserWindowInterface* browser, int index) {
  TabStripModel* model = browser->GetTabStripModel();
  if (!InTree(model, index)) {
    return;
  }
  std::vector<int> indices;
  for (int i = index; i < BranchEnd(model, index); ++i) {
    indices.push_back(i);
  }
  if (!chrome::CanMoveTabsToNewWindow(browser, indices)) {
    return;
  }
  std::set<content::WebContents*>& departing = Departing();
  for (const int i : indices) {
    departing.insert(model->GetWebContentsAt(i));
  }
  chrome::MoveTabsToNewWindow(browser, indices);
  departing.clear();
}

void NewTabBelow(BrowserWindowInterface* browser, int index) {
  TabStripModel* model = browser->GetTabStripModel();
  if (!InTree(model, index)) {
    return;
  }
  WindowTree* tree = WindowTree::From(model);
  if (tree) {
    tree->ExpectParent(StateAt(model, index)->parent);
  }
  chrome::AddTabAt(browser, GURL(), BranchEnd(model, index),
                   /*foreground=*/true);
  if (tree) {
    tree->ExpectParent(std::nullopt);
  }
}

Detached DetachChildren(TabStripModel* model, int index) {
  Detached detached;
  if (!InTree(model, index)) {
    return detached;
  }
  const TreeState* state = StateAt(model, index);
  detached.id = state->id;
  detached.collapsed = state->collapsed;
  for (int i = index + 1; i < model->count(); ++i) {
    content::WebContents* kid = model->GetWebContentsAt(i);
    if (State(kid)->parent == state->id) {
      detached.kids.push_back(State(kid)->id);
      SetParent(kid, state->parent);
    }
  }
  if (!detached.kids.empty()) {
    SetCollapsedState(model->GetWebContentsAt(index), false);
    NotifyChangedIn(model);
  }
  return detached;
}

void ReattachChildren(TabStripModel* model, const Detached& detached) {
  const int index = IndexOfId(model, detached.id);
  if (index < 0 || detached.kids.empty()) {
    return;
  }
  // 它没挤地方：子标签还紧跟在它后面，挂回去。
  const std::string parent = StateAt(model, index)->parent;
  bool any = false;
  for (const std::string& id : detached.kids) {
    const int kid = IndexOfId(model, id);
    if (kid > index && StateAt(model, kid)->parent == parent) {
      SetParent(model->GetWebContentsAt(kid), detached.id);
      any = true;
    }
  }
  if (any) {
    SetCollapsedState(model->GetWebContentsAt(index), detached.collapsed);
  }
  NotifyChangedIn(model);
}

void AddExtraData(content::WebContents* contents,
                  std::map<std::string, std::string>* extra_data) {
  const TreeState* state = TreeState::FromWebContents(contents);
  if (!state) {
    return;
  }
  (*extra_data)[kIdKey] = state->id;
  (*extra_data)[kParentKey] = state->parent;
  (*extra_data)[kCollapsedKey] = state->collapsed ? "1" : "";
  if (!state->branch.empty()) {
    (*extra_data)[kBranchKey] = state->branch;
  }
}

void RestoreExtraData(content::WebContents* contents,
                      bool from_session_restore,
                      const std::map<std::string, std::string>& extra_data) {
  auto id = extra_data.find(kIdKey);
  if (id == extra_data.end() || id->second.empty()) {
    return;
  }
  TreeState* state = State(contents);
  state->id = id->second;
  auto parent = extra_data.find(kParentKey);
  state->parent = parent == extra_data.end() ? std::string() : parent->second;
  auto collapsed = extra_data.find(kCollapsedKey);
  state->collapsed = collapsed != extra_data.end() && collapsed->second == "1";
  state->branch.clear();
  auto branch = extra_data.find(kBranchKey);
  if (from_session_restore || branch == extra_data.end() ||
      branch->second.empty()) {
    return;
  }
  // ⌘⇧T 恢复了一起关掉的一组里的一个：插进模型以后接着恢复同一组的。
  const std::string token = branch->second;
  Restoring()[token].tabs.emplace_back(contents->GetWeakPtr(),
                                       state->collapsed);
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&ContinueBranchRestore, token, contents->GetWeakPtr()));
}

std::string DescribeForTesting(TabStripModel* model) {
  const std::vector<Node> nodes = Compute(model);
  std::function<std::string(int)> describe = [&](int index) {
    std::string out = ShortTitle(model->GetWebContentsAt(index));
    if (nodes[index].collapsed) {
      out += "+";
    }
    if (nodes[index].children == 0) {
      return out;
    }
    out = "[" + out + ":";
    const int end = BranchEndIn(nodes, index);
    for (int i = index + 1; i < end; ++i) {
      if (nodes[i].parent == index) {
        out += " " + describe(i) + ",";
      }
    }
    out.back() = ']';
    return out;
  };
  std::string result;
  for (int i = model->IndexOfFirstNonPinnedTab(); i < model->count(); ++i) {
    if (nodes[i].parent < 0) {
      result += (result.empty() ? "" : ", ") + describe(i);
    }
  }
  return result.empty() ? "(empty)" : result;
}

// ---- 上游的钩子（starry_hooks.h） ----

int InsertionIndex(TabStripModel* model,
                   tabs::TabInterface* tab,
                   int chrome_index) {
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents || !Enabled(model->profile())) {
    return chrome_index;
  }
  TreeState* state = State(contents);
  // 打开它的标签：导航时记下的来源，没有就是当前标签（同 Chrome）。
  int source = IndexOfId(model, state->source);
  state->source.clear();
  if (source < 0) {
    source = model->active_index();
  }
  if (source < 0 || source >= model->count() || model->IsTabPinned(source)) {
    // 固定标签打开的：Chrome 放在普通标签的最前面，最外层。
    state->parent.clear();
    return chrome_index;
  }
  const std::vector<Node> nodes = Compute(model);
  // 层数到上限了：往上找，挂在放得下的那一层（成了兄弟）。
  const int limit = MaxDepth(model->profile());
  int parent = source;
  while (limit > 0 && nodes[parent].parent >= 0 &&
         nodes[parent].depth + 1 >= limit) {
    parent = nodes[parent].parent;
  }
  if (limit == 1) {
    // 只有一层：排在它那一枝后面，最外层。
    state->parent.clear();
    return BranchEndIn(nodes, parent);
  }
  state->parent = StateAt(model, parent)->id;
  return BranchEndIn(nodes, parent);
}

std::optional<int> SelectionAfterClose(TabStripModel* model,
                                       tabs::TabInterface* tab,
                                       std::optional<int> chrome_choice) {
  if (!Enabled(model->profile())) {
    return chrome_choice;
  }
  const int index = model->GetIndexOfTab(tab);
  const int first = model->IndexOfFirstNonPinnedTab();
  if (index == TabStripModel::kNoTab || index != model->active_index() ||
      index < first || model->count() <= 1) {
    return chrome_choice;
  }
  const std::vector<Node> nodes = Compute(model);
  const Node& node = nodes[index];
  // 不在树里（最外层、没有子标签）：照 Chrome 的，⌘T 开的标签马上关掉还回到原来的。
  if (node.parent < 0 && node.children == 0) {
    return chrome_choice;
  }
  // 关掉以后的下标。
  auto after = [index](int i) { return i > index ? i - 1 : i; };
  // 有子标签：接替它的第一个子标签。
  if (node.children > 0) {
    return after(index + 1);
  }
  // 下一个兄弟。
  if (index + 1 < model->count() && nodes[index + 1].parent == node.parent) {
    return after(index + 1);
  }
  // 上一个兄弟。
  for (int i = index - 1; i >= first && nodes[i].depth >= node.depth; --i) {
    if (nodes[i].parent == node.parent) {
      return after(i);
    }
  }
  if (node.parent >= 0) {
    return after(node.parent);
  }
  return chrome_choice;
}

void NoteSource(content::WebContents* source, content::WebContents* contents) {
  if (!source || !contents || source == contents) {
    return;
  }
  State(contents)->source = State(source)->id;
}

bool IsHidden(TabStripModel* model, int index) {
  if (!WindowTree::From(model) || !Shown(model->profile()) ||
      index < model->IndexOfFirstNonPinnedTab() || index >= model->count()) {
    return false;
  }
  return Compute(model)[index].hidden;
}

int DuplicateIndex(TabStripModel* model,
                   int index,
                   int chrome_index,
                   content::WebContents* duplicate) {
  // 分屏整组复制（DuplicateSplit）照 Chrome 的。
  if (!duplicate || !WindowTree::From(model) || !InTree(model, index) ||
      chrome_index != index + 1 || model->GetTabAtIndex(index)->GetSplit()) {
    return chrome_index;
  }
  State(duplicate)->parent = StateAt(model, index)->parent;
  return BranchEnd(model, index);
}

// ---- WindowTree ----

WindowTree::WindowTree(BrowserWindowInterface* browser)
    : model_(browser->GetTabStripModel()) {
  Registry()[model_] = this;
  model_->AddObserver(this);
  prefs_.Init(Prefs(browser->GetProfile()));
  prefs_.Add(prefs::kTreeTabs, base::BindRepeating(&WindowTree::NotifyChanged,
                                                   base::Unretained(this)));
}

WindowTree::~WindowTree() {
  model_->RemoveObserver(this);
  Registry().erase(model_);
}

// static
WindowTree* WindowTree::From(TabStripModel* model) {
  auto it = Registry().find(model);
  return it == Registry().end() ? nullptr : it->second;
}

base::CallbackListSubscription WindowTree::AddChangedCallback(
    base::RepeatingClosure callback) {
  return changed_.Add(std::move(callback));
}

void WindowTree::NotifyChanged() {
  // 聚焦着的一组没有子标签了（拖走了、关掉了）、不再显示成树：回到整棵树。
  if (!hoisted_.empty() && HoistedIndex() < 0) {
    hoisted_.clear();
  }
  changed_.Notify();
}

void WindowTree::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  switch (change.type()) {
    case TabStripModelChange::kInserted:
      for (const auto& inserted : change.GetInsert()->contents) {
        Inserted(inserted.contents, inserted.index);
      }
      break;
    case TabStripModelChange::kMoved: {
      const auto* move = change.GetMove();
      Moved(move->contents, move->to_index);
      break;
    }
    case TabStripModelChange::kRemoved:
      for (const auto& removed : change.GetRemove()->contents) {
        if (const TreeState* state =
                TreeState::FromWebContents(removed.contents)) {
          PromoteChildren(model_, InfoOf(state));
        }
      }
      break;
    case TabStripModelChange::kReplaced: {
      // 换了 WebContents（休眠的标签醒过来等）：树的数据跟过去。
      const auto* replace = change.GetReplace();
      const TreeState* old = TreeState::FromWebContents(replace->old_contents);
      if (old && !TreeState::FromWebContents(replace->new_contents)) {
        TreeState* state = State(replace->new_contents);
        state->id = old->id;
        state->parent = old->parent;
        state->collapsed = old->collapsed;
      }
      break;
    }
    case TabStripModelChange::kSelectionOnly:
      break;
  }
  if (selection.active_tab_changed() ||
      change.type() == TabStripModelChange::kInserted) {
    RevealActive();
  }
  // 聚焦着的一组没了，或者切到了这一组外面：回到整棵树。整枝挪的过程中树是乱的
  // （子标签可能暂时在父标签前面），挪完在 NotifyChanged 里再看。
  if (!hoisted_.empty() && !moving_) {
    const int hoisted = HoistedIndex();
    const int active = model_->active_index();
    if (hoisted < 0 ||
        (selection.active_tab_changed() &&
         (active < hoisted || active >= BranchEnd(model_, hoisted)))) {
      hoisted_.clear();
    }
  }
  Profile* profile = model_->profile();
  if (selection.active_tab_changed() && !moving_ && AutoCollapse(profile) &&
      Shown(profile) && InTree(model_, model_->active_index())) {
    for (const int group : OtherGroups(model_, model_->active_index())) {
      SetCollapsedState(model_->GetWebContentsAt(group), true);
      folded_ = true;
    }
  }
}

bool WindowTree::CanHoist(int index) const {
  return Shown(model_->profile()) && InTree(model_, index) &&
         Compute(model_)[index].children > 0 && HoistedIndex() != index;
}

void WindowTree::Hoist(int index) {
  if (!CanHoist(index)) {
    return;
  }
  hoisted_ = StateAt(model_, index)->id;
  // 这一组和它的祖先展开着：⌃Tab、⌘1–8 跳过的和列表里看到的一样。
  const std::vector<Node> nodes = Compute(model_);
  for (int p = index; p >= 0; p = nodes[p].parent) {
    SetCollapsedState(model_->GetWebContentsAt(p), false);
  }
  NotifyChanged();
}

void WindowTree::Unhoist() {
  if (!hoisted_.empty()) {
    hoisted_.clear();
    NotifyChanged();
  }
}

int WindowTree::HoistedIndex() const {
  if (hoisted_.empty() || !Shown(model_->profile())) {
    return -1;
  }
  const int index = IndexOfId(model_, hoisted_);
  // 挪一整枝的过程中树是乱的：先不看有没有子标签。
  if (index < 0 || moving_) {
    return index;
  }
  return Compute(model_)[index].children > 0 ? index : -1;
}

void WindowTree::OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) {
  content::WebContents* contents = tab->GetContents();
  TreeState* state = State(contents);
  if (tab->IsPinned()) {
    // 固定区不进树：它的子标签改挂，它自己不再有父标签。
    const Info gone = InfoOf(state);
    PromoteChildren(model_, gone);
    SetParent(contents, std::string());
    SetCollapsedState(contents, false);
    return;
  }
  const int at = model_->GetIndexOfTab(tab);
  if (at != TabStripModel::kNoTab) {
    SetParent(contents, ParentFor(model_, at, at + 1, std::string()));
  }
}

void WindowTree::Inserted(content::WebContents* contents, int index) {
  TreeState* state = State(contents);
  state->source.clear();
  // 同一份会话恢复了两次：ID 重了，新来的换一个。
  for (int i = 0; i < model_->count(); ++i) {
    content::WebContents* other = model_->GetWebContentsAt(i);
    if (other != contents && TreeState::FromWebContents(other) &&
        State(other)->id == state->id) {
      state->id = NewId();
      break;
    }
  }
  if (model_->IsTabPinned(index)) {
    state->parent.clear();
    state->collapsed = false;
  } else {
    if (expected_parent_) {
      state->parent = *std::exchange(expected_parent_, std::nullopt);
    }
    state->parent = ParentFor(model_, index, index + 1, state->parent);
  }
  SaveInSession(contents);
}

void WindowTree::Moved(content::WebContents* contents, int to) {
  if (moving_) {
    return;
  }
  const Info gone = InfoOf(State(contents));
  // 只挪了它一个：留在原处的子标签改挂到它原来的父标签。
  for (int i = model_->IndexOfFirstNonPinnedTab(); i < model_->count(); ++i) {
    content::WebContents* other = model_->GetWebContentsAt(i);
    if (other != contents && State(other)->parent == gone.id) {
      SetParent(other, gone.parent);
    }
  }
  if (model_->IsTabPinned(to)) {
    SetParent(contents, std::string());
  } else {
    // 挪到哪就认哪：和下面那一行同级。
    SetParent(contents, ParentFor(model_, to, to + 1, std::nullopt));
  }
  ClearIfChildless(model_, gone.parent);
  ClearIfChildless(model_, gone.id);
}

void WindowTree::RevealActive() {
  RevealActiveIn(model_);
}

}  // namespace starry::tab_tree
