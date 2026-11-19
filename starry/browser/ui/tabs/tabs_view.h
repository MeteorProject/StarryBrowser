// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TABS_VIEW_H_
#define STARRY_BROWSER_UI_TABS_TABS_VIEW_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/elastic_scroll_view.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "starry/browser/ui/tabs/tab_list_view.h"
#include "starry/browser/ui/tabs/tab_tree.h"
#include "starry/browser/ui/tabs/tree_sticky_header.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_tracker.h"

class BrowserWindowInterface;

namespace starry {

class PinFolderCard;
class TabDragPill;
class TabMenu;

// 侧栏和顶部标签栏共用的标签控制器。Chromium 的 TabStripModel 将常用标签和
// 固定标签都视为 pinned。子类负责各自的布局和落点区域。
class TabsView : public views::View,
                 public TabStripModelObserver,
                 public TabCellDelegate {
  METADATA_HEADER(TabsView, views::View)

 public:
  TabsView(const TabsView&) = delete;
  TabsView& operator=(const TabsView&) = delete;
  ~TabsView() override;

  virtual void SetTheme(const Theme& theme);

  // 调试用：第 index 个标签（模型里的顺序）的单元。
  TabCell* CellAtModelIndex(int index);
  TabListView* pinned_list() { return pinned_; }
  TabListView* tab_list() { return list_; }
  // 顶部标签行的固定区（散着的固定标签、文件夹的胶囊）；侧栏里没有。
  TabListView* pin_strip() { return pins_; }
  // 正拖着一个标签或者文件夹（自动隐藏的侧栏这时不收回）。
  bool is_dragging() const { return drag_.has_value(); }
  // 拖着的是标签：侧栏里空着的常用区这时露出落点。
  bool is_dragging_tab() const {
    return drag_ && drag_->folder.empty() && drag_->tile.empty();
  }
  // 拖着的是常用图块里的文件夹（只能在图块那一层排序）。
  bool is_dragging_tile() const { return drag_ && !drag_->tile.empty(); }
  // 开着的文件夹卡片；没开着、正在收起时是空的。
  PinFolderCard* card() const;
  // 调试用：现在松手会放到哪。
  std::string DropStateForTesting() const;

  // 连着改几处（文件夹、标签的位置和排序值）时先不刷新，最后按 change 一起刷新
  // 一次：列表直接动到最后的样子。
  void BeginBatch();
  void EndBatch(TabListView::Change change);
  // 别的窗口变了，这个窗口里显示哪些文件夹可能跟着变（pin_folders::ShownIn）。
  void RefreshFolders();
  // 调试用：普通标签的列表滚到头后现在被拉出去多少（ElasticScrollView）。
  float scroll_stretch() const { return scroll_->CurrentStretch(); }
  // 调试用：树里吸顶的父标签（顶部标签布局下没有）。
  TreeStickyHeader* sticky_header() const { return sticky_; }

  // 刚新建、正在起名的文件夹：按 Esc
  // 取消时删掉它，建的时候放进去的标签放回原处。
  struct NewFolder {
    std::string id;
    tabs::TabHandle moved;
    bool was_pinned = false;
    bool was_favorite = false;
    std::string old_folder;
  };
  // 让侧栏里这个文件夹的行就地改名。这一行不在时返回 false（顶部标签布局、收在
  // 折叠的文件夹里）。
  bool BeginFolderRename(const std::string& id);
  bool BeginFolderRename(const std::string& id, NewFolder fresh);
  // 这个文件夹的卡片开着：卡片上面的名字就地改名。没开着时返回 false。
  bool BeginCardRename(const std::string& id);

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;
  void OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) override;
  void OnTabBlockedStateChanged(tabs::TabInterface* tab) override;
  void OnSplitTabChanged(const SplitTabChange& change) override;

  // views::View:
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;

  // TabCellDelegate:
  void TabCellClicked(TabCell* cell, bool reset, bool keep_current) override;
  void TabCellCloseRequested(TabCell* cell, bool button) override;
  void TabCellMuteToggled(TabCell* cell) override;
  void TabCellHoverChanged(TabCell* cell, bool hovered) override;
  void TabCellTreeToggled(TabCell* cell, bool all) override;
  void TabCellSuggestionAccepted(TabCell* cell) override;
  void TabFolderAction(const std::string& id,
                       std::optional<gfx::Point> menu_point) override;
  void TabFolderAddCurrent(const std::string& id) override;
  void TabFolderOpenAll(const std::string& id) override;
  void TabFolderToggleTree(const std::string& id) override;
  bool TabListKeyPressed(views::View* item, const ui::KeyEvent& event) override;
  void TabFolderRenameFinished(const std::string& id,
                               std::optional<std::u16string> name) override;
  void TabListGapHovered(bool hovered) override;
  bool TabCellShowsTooltip() override;
  void TabCellDragStarted(TabCell* cell, const gfx::Point& location) override;
  void TabCellDragged(TabCell* cell, const gfx::Point& location) override;
  void TabCellDragEnded(TabCell* cell, bool cancelled) override;
  void TabRowDragStarted(TabListRow* row, const gfx::Point& location) override;
  void TabRowDragged(TabListRow* row, const gfx::Point& location) override;
  void TabRowDragEnded(TabListRow* row, bool cancelled) override;
  void TabCellContextMenu(TabCell* cell,
                          const gfx::Point& screen_point,
                          ui::mojom::MenuSourceType source) override;

 protected:
  struct DropTarget {
    // 落在哪一块：侧栏的列表或者顶部的普通标签、顶部标签行的固定区、文件夹的
    // 卡片、常用图块。
    enum class Area { kList, kPins, kCard, kGrid };
    // 放进常用（GridDropTarget、图块文件夹的卡片）：folder 是放进哪一层（空的
    // 是图块那一层），before 是排在谁前面，onto 是停在哪个图块文件夹上。
    bool pinned = false;
    // 插到这一组的第几个之前（按拖动前的顺序，包括拖着的那个）；常用图块里是
    // 第几格。
    size_t index = 0;
    std::string folder = {};
    // 侧栏的固定区（ListDropTarget 给的）：这时 folder 是放进哪一层，下面几项
    // 才有意义。
    bool ordered = false;
    // 排在这一项之前；空的是放在这一层的最后。
    pin_folders::Child before = {};
    // 停在一行上（文件夹、落点、折叠着的小标题）、松手是放进去：这一行的 ID。
    std::string onto = {};
    // 停满 kFolderSpringLoad 要展开的那一行（折叠着的文件夹，或者 "@pins"）。
    std::string spring = {};
    // 停在图块那一层的一个常用标签上：松手和它合成一个新的图块文件夹。
    tabs::TabHandle merge = {};
    // 放在两行之间：插入线的位置（列表的坐标）和层级。
    int line_y = 0;
    int line_depth = 0;
    Area area = Area::kList;
    // 顶部标签行的固定区：竖着的插入线在哪（固定区的坐标）。
    int line_x = 0;
    // 树状标签页（侧栏列表里的普通标签，TreeListDropTarget 给的）：松手后挂在
    // tree_parent 下面（空的是最外层）、排在 tree_before 前面（空的是最后），这时
    // index 只管插入线画在哪、line_depth 是放下以后的层级。tree_into：停在这个标签
    // 上，松手是放进去当它的最后一个子标签（它描边，没有插入线）。
    bool tree = false;
    tabs::TabHandle tree_parent = {};
    tabs::TabHandle tree_before = {};
    tabs::TabHandle tree_into = {};
    // 树里的缝：能选的层级是 [line_min, line_max]（另外几层画小圆圈）。
    int line_min = 0;
    int line_max = 0;
  };

  TabsView(BrowserWindowInterface* browser,
           TabListView::Style pinned_style,
           TabListView::Style list_style);

  // 拖到这一点（本视图的坐标）松手会放到哪里；空的时候不排序。
  virtual std::optional<DropTarget> DropTargetAt(
      const gfx::Point& point) const = 0;
  // 这一点（本视图的坐标）在内容区上：拖到这里是分屏。
  virtual bool IsOverContent(const gfx::Point& point) const = 0;
  // 预览卡片放在哪：侧栏布局下放在 edge（preview
  // 的坐标）右边；空的时候放在标签下面。
  virtual std::optional<int> PreviewEdge(views::View* preview) const = 0;
  // 选中的标签换了：不在视野里时滚过去。
  virtual void RevealActive() = 0;
  // 两组标签按模型刷新过了（子类重新排版）。
  virtual void OnSynced(TabListView::Change change) {}
  // 开着的文件夹卡片里的列表（拖动时也能放进去）；没开着是空的。
  TabListView* CardList() const;
  // 这个文件夹（图块文件夹、顶部的胶囊，或者 kMorePinsId）的卡片开着。
  bool IsCardOpen(const std::string& id) const;
  // 开着的卡片是哪个文件夹的；没开着是空的。
  std::string OpenCardId() const;
  // 顶部标签行的固定区最多多宽；0 是不限。
  virtual int PinStripLimit() const;
  // 文件夹的卡片（PopoverHost 里，同一时间一张）：id 是图块文件夹、顶部的胶囊
  // 或者 kMorePinsId。sprung：拖着东西停上去落下来的，放完就收起。
  void ToggleCard(const std::string& id);
  void OpenCard(const std::string& id, bool sprung);
  void CloseCard();
  // 卡片在哪（本视图的坐标）；没开着是空的。
  gfx::Rect CardBounds() const;
  // 卡片挂在哪（弹出框那一层的坐标）、怎么放、多宽（0 是默认的宽度）。
  struct CardAnchor {
    gfx::Rect rect;
    PopoverHost::Placement placement = PopoverHost::Placement::kBelow;
    int width = 0;
    // 从哪一块长出来（图块文件夹的图块）；没有是弹出框平常的出现。
    std::optional<PopoverHost::Grow> grow;
  };
  // 这个文件夹的卡片挂在哪；没有可以挂的地方时是空的。这里是图块文件夹，
  // 顶部标签行再加上胶囊。
  virtual std::optional<CardAnchor> CardAnchorFor(const std::string& id) const;
  PopoverHost* popover_host() const;
  // 鼠标（本视图的坐标）在开着的卡片上：target 是卡片里的落点（可能是空的，
  // 不能放），返回 true。
  bool CardDropTargetAt(const gfx::Point& point,
                        std::optional<DropTarget>* target) const;
  // 常用图块的这一点（常用图块的坐标）松手会放到哪。
  std::optional<DropTarget> GridDropTarget(const gfx::Point& point) const;

  // 文件夹卡片里的行和标签：root 里的东西（root 是 kMorePinsId 时是固定区里
  // 收不下的那些），按树的顺序，里面的文件夹按各自的开合。root 没了（文件夹
  // 删了、都收得下了）时返回 false。
  bool BuildCard(const std::string& root,
                 std::u16string* title,
                 std::vector<TabListRowData>* rows,
                 std::vector<TabListView::Item>* items);
  // 卡片里的列表这一点（列表的坐标）松手会放到哪。
  std::optional<DropTarget> CardDropTarget(const std::string& root,
                                           const gfx::Point& point) const;
  // 顶部标签行的固定区这一点（固定区的坐标）松手会放到哪。
  std::optional<DropTarget> StripDropTarget(const gfx::Point& point) const;
  // 按鼠标现在的位置重新算落点（列表重排、滚动、卡片开关以后）。
  void UpdateDropTarget();
  // 顶部标签行的固定区上次刷新时全都放下要多宽：胶囊带名字、只有图标。
  int strip_full_width() const { return strip_full_width_; }
  int strip_compact_width() const { return strip_compact_width_; }

  // 侧栏的列表里这一点（列表的坐标）松手会放到哪：分隔线以上是固定区的树，以下
  // 是普通标签。拖着文件夹时只能放在固定区里。
  std::optional<DropTarget> ListDropTarget(const gfx::Point& point) const;

  void Sync(TabListView::Change change);
  // 树状标签页显示着（侧栏的列表、设置里开着）：普通标签按层缩进，拖的是整枝。
  bool TreeShown() const;
  // 聚焦着的一组的父标签在模型里的下标（tab_tree::WindowTree::Hoist）；没有是 -1。
  int HoistedIndex() const;
  // 回到整棵树；focus：焦点放在刚才聚焦的那个标签上。
  void LeaveHoist(bool focus);
  // 父标签吸顶：按滚动的位置重新算停着哪几行（滚动、刷新、排版以后）；放到
  // 滚动区域顶上，下面的行在它下面淡掉。
  void UpdateSticky();
  void PlaceSticky();
  BrowserWindowInterface* browser() const { return browser_; }
  TabStripModel* model() const { return model_; }
  const Theme& theme() const { return theme_; }
  views::ScrollView* scroll() { return scroll_; }
  const views::ScrollView* scroll() const { return scroll_; }
  TabListView* pinned() const { return pinned_; }
  TabListView* list() const { return list_; }
  TabListView* pins() const { return pins_; }

 private:
  struct Drag {
    tabs::TabHandle handle;
    // 拖的是文件夹（这时 handle 是空的）。
    std::string folder;
    // 拖的是常用图块里的文件夹（这时 handle、folder 是空的）。
    std::string tile;
    bool from_pinned = false;
    // 按下的位置在标签里的偏移：小条跟着鼠标时保持这个偏移。
    gfx::Vector2d grab_offset;
    // 鼠标现在在哪（本视图的坐标）；还没动过是空的。
    std::optional<gfx::Point> point;
    // 侧栏的固定区空着：标签拖到列表顶上时露出一个落点，露出来就留到松手。
    bool reveal_pins = false;
    std::optional<DropTarget> target;
    // 在内容区上（分屏的落点预览开着）。
    bool over_content = false;
    // 小条的左边在鼠标左边多远（DragTo 算的）：树里按小条的左边选放在哪一层。
    int grab_x = 0;
    // 树里按着 ⌥ 只拖一个：它原来的子标签（拖起来时上移了一级）。
    std::optional<tab_tree::Detached> detached;
  };

  tabs::TabInterface* TabFor(TabCell* cell) const;
  // 点了一个标签：切过去（reset：固定标签回到固定时的网页）。
  void ActivateCell(TabCell* cell, bool reset, bool keep_current);
  int ModelIndexFor(TabCell* cell) const;
  // 侧栏的列表里一个固定标签显示在哪个文件夹下面（最外层是空的）。kept：那个
  // 文件夹折叠着，它是留在下面的当前标签（真正所在的文件夹可能更深）。
  struct ShownTab {
    std::string under;
    bool kept = false;
  };
  // 显示着的几组标签：常用、普通（侧栏是整个列表）、顶部的固定区、开着的卡片。
  std::vector<TabListView*> Lists() const;
  TabListView* ListFor(DropTarget::Area area) const;
  // 读文件夹和这个窗口的固定区（folders_、folder_parents_、levels_）。
  void ReadTree();
  // 一个文件夹和它下面所有子文件夹里的标签，按显示的顺序。
  void Collect(const std::string& id,
               std::vector<const TabListView::Item*>* out,
               int depth) const;
  // parent 这一层的行和标签（subset 不是空的时候只放这些），一层一层往里放；
  // collapsed：整个固定区折叠着，只留当前标签。shown 记下每个标签显示在哪。
  void EmitTree(const std::string& parent,
                int depth,
                bool collapsed,
                const std::vector<pin_folders::Child>* subset,
                std::vector<TabListRowData>* rows,
                std::vector<TabListView::Item>* items,
                std::map<tabs::TabHandle, ShownTab>* shown) const;
  // 侧栏：固定区的行（小标题、文件夹、落点、分隔线）和里面的标签，按树的顺序。
  void BuildPinRows(bool collapsed,
                    std::vector<TabListRowData>* rows,
                    std::vector<TabListView::Item>* items);
  // 常用图块：图块那一层的常用标签和图块文件夹（rows），按排序值混排。
  void BuildGrid(std::vector<TabListRowData>* rows,
                 std::vector<TabListView::Item>* items);
  // 常用图块里这个文件夹的图块；没有是空的。
  TabListRow* TileFor(const std::string& id) const;
  // 模型里挪到常用图块（图块文件夹展开）的顺序里它的位置上。
  void MoveToFavoriteOrder(tabs::TabInterface* tab);
  void OnCardClosed(int serial);
  // 按现在的样子重填卡片；文件夹没了（或者「…」里空了）返回 false。
  bool FillCard(PinFolderCard* card, TabListView::Change change);
  void UpdateCard(TabListView::Change change);
  // 开始、结束拖动。
  void DragStateChanged(bool dragging);
  // 拖着的小条放在哪：卡片开着时放到弹出框那一层的正上面（盖住卡片，列表还在
  // 卡片下面），不然放回这里。屏幕上的位置不变。
  void PlacePill(bool above_card);
  // 本视图里的一点换成小条所在那一层的坐标。
  gfx::Point ToPillParent(const gfx::Point& point) const;
  // 顶部标签行的固定区：最外层的东西，放不下时先把胶囊收成图标，再把后面的
  // 收进「…」（overflow_）。
  void BuildTopPins(std::vector<TabListRowData>* rows,
                    std::vector<TabListView::Item>* items);
  // 固定区的树在 list 里（前 end 项）时这一点松手会放到哪：root 是最外层
  // （卡片里是那个文件夹），first 是它里面的第一项（列表最上面没有小标题时，
  // 放在第一项上半截是排在它前面）。
  std::optional<DropTarget> TreeDropTarget(
      const TabListView* list,
      const gfx::Point& point,
      size_t end,
      const std::string& root,
      const pin_folders::Child& first,
      const std::map<tabs::TabHandle, ShownTab>& shown,
      DropTarget::Area area) const;
  // 拖着的是文件夹，id 是它自己或者它里面的文件夹。
  bool InsideDragged(std::string id) const;
  // 列表里这条线（列表的坐标）上面该吸顶的父标签，最外层在前。
  std::vector<TreeStickyHeader::Row> StickyRowsAt(int line) const;
  // 点了吸顶的一行：滚到它在列表里的位置。
  void ScrollToTab(tabs::TabHandle handle);
  // 树状标签页：侧栏列表里普通标签（从第 first 个标签起）这一点松手会放到哪：
  // 一行的上四分之一是放在它前面、下四分之一是后面、中间是当它的子标签；缝里按小条
  // 的左边界水平位置选层级。拖着的那一枝当作不在列表里。
  std::optional<DropTarget> TreeListDropTarget(const gfx::Point& point,
                                               size_t first) const;
  // 键盘：list 前后能接着走的列表（侧栏是常用图块、列表；顶部是常用图块、固定
  // 区、普通标签；卡片只有自己）。
  std::vector<TabListView*> FocusChain(TabListView* list) const;
  // 焦点往前（-1）、往后（1）挪一项，到头了换到 FocusChain 里的下一个列表。
  void MoveFocus(TabListView* list, views::View* from, int direction);
  // 焦点挪到 list 里这个文件夹那一行；没有这一行时返回 false。
  bool FocusFolderRow(TabListView* list, const std::string& id);
  // 收着的文件夹上挂的智能体提示：智能体在操作的固定标签变了就刷新。
  void OnAgentsChanged();
  std::set<tabs::TabHandle> AgentPinnedTabs() const;
  // 模型里挪到固定区（树的先序）里它的位置上：⌘1…9、切到下一个标签按模型。
  void MoveToTreeOrder(tabs::TabInterface* tab);
  std::string FolderParent(const std::string& id) const;
  pin_folders::Child DraggedItem() const;
  // 松手后这次拖动会不会改变位置。
  bool DropChangesOrder(const DropTarget& target) const;
  void ShowDropTarget(std::optional<DropTarget> target);
  void DragTo(const gfx::Point& point);
  void FinishDrag(bool cancelled);
  void Drop(const Drag& drag, bool cancelled, bool changes);
  // 拖着的文件夹和它下面显示着的行原地变淡。
  void LiftFolder(const std::string& id);
  void SpringOpen();
  // 拖着东西时自己落下来的卡片：鼠标离开卡片和挂着它的图块（胶囊）一会儿就
  // 收起来（kFolderSpringLeave），回来了不收。
  void TrackSprungCard();
  void CloseSprungCard();
  void UpdateAutoScroll();
  void AutoScrollStep();
  void UpdateLoadingTimer();
  void StepLoadingAnimations();

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<TabStripModel> model_;
  Theme theme_ = Theme::DarkBase();

  raw_ptr<TabListView> pinned_ = nullptr;
  raw_ptr<TabListView> pins_ = nullptr;
  raw_ptr<ElasticScrollView> scroll_ = nullptr;
  raw_ptr<TabListView> list_ = nullptr;
  raw_ptr<TreeStickyHeader> sticky_ = nullptr;
  base::CallbackListSubscription scrolled_;
  raw_ptr<TabDragPill> drag_pill_ = nullptr;

  std::optional<Drag> drag_;
  // 最近一次打开的标签菜单：菜单开着时模型要活着，到下一次打开才换。
  std::unique_ptr<TabMenu> context_menu_;
  std::unique_ptr<pin_folders::Menu> folder_menu_;
  NewFolder new_folder_;
  PrefChangeRegistrar folder_prefs_;
  base::CallbackListSubscription session_restored_;
  // 树状标签页变了（开合、设置）：重新排。
  base::CallbackListSubscription tree_changed_;
  // 上次刷新时聚焦着的一组的父标签（变了用 kEdit 的动画）。
  tabs::TabHandle hoist_shown_;
  // 每个固定标签上次刷新时在哪：是不是常用、文件夹、排序值。
  std::map<tabs::TabHandle,
           std::tuple<bool, std::string, std::optional<double>>>
      locations_;
  // 侧栏上次刷新时固定区的样子：每一层的东西、每个文件夹的上一层、每个显示着的
  // 固定标签在哪个文件夹下面。拖动时按它们算落点。
  pin_folders::Levels levels_;
  std::map<std::string, std::string> folder_parents_;
  std::map<std::string, pin_folders::Folder> folders_;
  std::map<tabs::TabHandle, ShownTab> shown_tabs_;
  // 上次刷新时的固定标签（不算常用的）；顶部标签行里收进「…」的最外层的东西；
  // 开着的卡片里每个标签显示在哪。
  std::map<tabs::TabHandle, TabListView::Item> pin_items_;
  std::vector<pin_folders::Child> overflow_;
  int strip_full_width_ = 0;
  int strip_compact_width_ = 0;
  std::map<tabs::TabHandle, ShownTab> card_shown_;
  // 上次刷新时的常用图块：每一层（图块那一层、每个图块文件夹）的东西、图块
  // 文件夹、常用标签。
  pin_folders::Levels fav_levels_;
  std::map<std::string, pin_folders::Folder> fav_folders_;
  std::map<tabs::TabHandle, TabListView::Item> fav_items_;
  // 开着的卡片（PopoverHost 拥有，它被换掉、删掉时这里跟着变空）。
  mutable views::ViewTracker card_tracker_;
  std::string card_id_;
  // 每开一次卡片加一：收起的回调只认最后一张。
  int card_serial_ = 0;
  bool card_sprung_ = false;
  // 刚收起的卡片：在图块、胶囊上按下时卡片先收起了，松开时不要再打开。
  std::string card_closed_id_;
  base::TimeTicks card_closed_at_;

  // 上次刷新时智能体在操作的固定标签（在文件夹里的）。
  std::set<tabs::TabHandle> agent_tabs_;
  base::CallbackListSubscription agent_subscription_;
  int batch_depth_ = 0;
  bool batch_dirty_ = false;
  // 拖着的东西停在这一行上，到时间就展开它。
  std::string spring_id_;
  base::OneShotTimer spring_timer_;
  base::OneShotTimer card_leave_timer_;
  // 拖到列表边缘时每一步滚多少（负的往上）。
  int autoscroll_step_ = 0;
  base::RepeatingTimer autoscroll_timer_;
  tabs::TabHandle last_preview_;
  bool favorites_scheduled_ = false;
  void ScheduleFavoritesSync();
  base::RepeatingTimer loading_timer_;
  base::TimeTicks loading_start_;

  base::WeakPtrFactory<TabsView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TABS_VIEW_H_
