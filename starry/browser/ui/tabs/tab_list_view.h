// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TAB_LIST_VIEW_H_
#define STARRY_BROWSER_UI_TABS_TAB_LIST_VIEW_H_

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "components/split_tabs/split_tab_id.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "starry/browser/ui/tabs/tab_list_row.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

namespace starry {

class TabListFill;

// 布局标签和文件夹等行。位置变化先设置最终边界，再通过合成器变换从旧位置过渡。
class TabListView : public views::View {
  METADATA_HEADER(TabListView, views::View)

 public:
  enum class Style { kList, kGrid, kTop, kTopPinned, kTopPins };
  // kFold：文件夹、固定区开合，或者文件夹本身变了（新建、改名、移动、解散）。
  // kEdit：树状标签页聚焦一组、回到整棵树：行滑到新的缩进（listEdit），别的行淡出淡入。
  enum class Change { kNone, kInsert, kClose, kSelect, kFold, kEdit };

  struct Item {
    tabs::TabHandle handle;
    tabs::TabData data;
    bool selected = false;
    // 所在的分屏；显示着的那组托盘用强调色。
    std::optional<split_tabs::SplitTabId> split;
    bool split_active = false;
    // 在第几层文件夹里（侧栏的列表）：每层往右缩一格。
    int depth = 0;
    // 树状标签页（侧栏的列表里的普通标签，tab_tree.h）：在第几层、有没有子标签、
    // 折着、里面一共几个、折着时叠着的前几个子标签的图标、父标签。缩进按列表的宽度
    // 和最深的一层算（TreeStep）。
    struct Tree {
      int depth = 0;
      bool parent = false;
      bool collapsed = false;
      int count = 0;
      std::vector<ui::ImageModel> icons;
      tabs::TabHandle up;
    };
    std::optional<Tree> tree;
    // 顶部标签行的固定区：撑开成带标题的标签（选中的散着的固定标签、挂在
    // 胶囊后面的当前标签），宽 width；tray 是挂在哪个文件夹的胶囊后面。
    bool wide = false;
    int width = 0;
    std::string tray;
  };

  // 顶部标签行的固定区：托盘比里面的胶囊和标签左右各多出这么多，胶囊和挂在
  // 后面的标签之间隔这么多。
  static constexpr int kTrayPad = 3;
  static constexpr int kTrayGap = 2;

  // 侧栏的列表里，行和标签按从上到下的顺序排在一起。
  struct Entry {
    // 是行（index 是 rows() 的下标）还是标签（index 是第几个标签）。
    bool row = false;
    size_t index = 0;
    int depth = 0;
    int y = 0;
    int height = 0;
    // 顶部标签行的固定区是横着排的。
    int x = 0;
    int width = 0;
  };
  // 拖动时插入位置的提示线。
  struct DropMarker {
    // 插到第几个标签之前（count() 表示末尾）。
    size_t index = 0;
    // 侧栏的固定区里线的位置和层级由 TabsView 定（同一条缝可以是不同的层级）；
    // 没有 y 时按 index 算。
    std::optional<int> y;
    int depth = 0;
    // 顶部标签行的固定区：竖着的线在哪。
    std::optional<int> x;
    // 树状标签页：放下以后在第几层（线缩到那一层）；没有时和下面那一行一样。
    // 这条缝能选的层级是 [tree_min, tree_max]：另外几层的位置上画淡淡的小圆圈。
    std::optional<int> tree_depth;
    int tree_min = 0;
    int tree_max = 0;
    bool operator==(const DropMarker&) const = default;
  };

  // 侧栏的列表里不是标签的行，按 before_tab 排好。下一次 Update 时生效。
  void SetRows(std::vector<TabListRowData> rows);
  const std::vector<raw_ptr<TabListRow, VectorExperimental>>& rows() const {
    return row_views_;
  }
  const std::vector<TabListRowData>& row_data() const { return rows_; }
  const std::vector<Entry>& entries() const { return entries_; }
  TabCell* CellAt(size_t index) const {
    return index < cells_.size() ? cells_[index].get() : nullptr;
  }
  // 让这个文件夹的行进入改名；没有这一行时返回 false。
  bool BeginRename(const std::string& id);
  tabs::TabInterface* TabAt(size_t index) const;
  std::optional<size_t> IndexOf(tabs::TabHandle handle) const;

  TabListView(Style style, TabCellDelegate* delegate);
  TabListView(const TabListView&) = delete;
  TabListView& operator=(const TabListView&) = delete;
  ~TabListView() override;

  Style style() const { return style_; }
  size_t count() const { return cells_.size(); }
  // 常用图块一共占几格（常用标签、图块文件夹各占一格）；别的样式同 count()。
  size_t slot_count() const;
  bool horizontal() const {
    return style_ == Style::kTop || style_ == Style::kTopPinned ||
           style_ == Style::kTopPins;
  }
  bool pinned() const {
    return style_ == Style::kGrid || style_ == Style::kTopPinned;
  }
  // 顶部标签行里普通标签的宽度（TopTabStrip 平分出来的）。变了用 change
  // 的曲线挤开。
  void SetItemWidth(int width, Change change);
  // 横排时所有标签占的宽度。
  int ContentWidth() const;
  // 顶部标签行的固定区：这些行和标签排起来多宽；entries 不是空的时候填上每一项
  // 的位置。
  static int LayoutTopPins(const std::vector<TabListRowData>& rows,
                           const std::vector<Item>& items,
                           std::vector<Entry>* entries);

  // highlight_from：选中的标签是从另一组（固定 ↔ 列表）过来的，高亮从那里（本视图的
  // 坐标）滑过来。
  void Update(const std::vector<Item>& items,
              const Theme& theme,
              Change change,
              std::optional<gfx::Rect> highlight_from);

  TabCell* CellFor(tabs::TabHandle handle);
  // 现在屏幕上高亮在哪里（本视图的坐标）；没有高亮时为空。
  std::optional<gfx::Rect> VisibleHighlightRect() const;
  // 第 index 个标签该在的位置。
  gfx::Rect SlotRect(size_t index) const;
  // 树状标签页：列表宽 width 时每层缩进多少（整个列表一样）、最多缩几层。
  // 每层 = clamp((width - 42 - 最短标题 92) / 显示着的最深层数, 6, 14)。
  int TreeStep(int width) const;
  int TreeCap(int width) const;
  // 第 index 个标签在树里的样子（不在树里的是空的）。
  std::optional<Item::Tree> TreeAt(size_t index) const {
    return index < trees_.size() ? trees_[index] : std::nullopt;
  }
  // 鼠标在这个标签上（空的是没有）：它每一层祖先的层级线从上面亮到它那一路。
  void SetHoveredTab(tabs::TabHandle handle);
  tabs::TabHandle hovered_tab() const { return hovered_tab_; }

  // 拖动：point 处松手会插到第几个之前（count() 表示末尾）。
  size_t DropIndexAt(const gfx::Point& point) const;
  // 显示插入位置的提示线；nullopt 时收起。侧栏的列表里线的左端带一个小圆头，
  // 按层级往右缩。
  void SetDropMarker(std::optional<DropMarker> marker);
  // 拖着的东西会放进这一行（文件夹、落点、折叠着的小标题）：这一行描边。空的是
  // 没有。
  void SetDropRow(const std::string& id);
  // 拖着的标签会和这个标签合成文件夹（常用图块）：它描边。空的是没有。
  void SetDropCell(tabs::TabHandle handle);
  // 拖动松手以后：被拖的那一项（标签，或者 folder 这个文件夹连同它下面显示着的
  // 行）不从原来的位置滑过来，直接待在新的位置上——跟着鼠标的小条本来就在那儿，
  // 滑过来会和让开的行叠在一起。
  void SnapInPlace(tabs::TabHandle handle, const std::string& folder);

  // 加载中的转圈往前走一步；返回还有没有在转的。
  bool StepLoadingAnimations(base::TimeDelta elapsed);

  // 键盘：能拿焦点的标签和行，按显示的顺序（从上到下、从左到右）。
  std::vector<views::View*> FocusOrder() const;
  // 按 Tab 键进到这个列表时停在哪：选中的标签，没有就是第一项。
  views::View* FocusEntry();
  // item 所在的列表的 FocusEntry（item 不在列表里时是空的）。
  static views::View* FocusEntryFor(views::View* item);

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void Layout(PassKey) override;
  void OnThemeChanged() override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;

 private:
  int PreferredHeightFor(size_t count) const;
  gfx::Rect SlotRectFor(size_t index, int width) const;
  // 分屏托盘里第 index
  // 个标签左右缩进多少（侧栏两边都缩，标签行只缩一组的头尾）。
  std::pair<int, int> TrayInsets(size_t index) const;
  // 横排时第 index 个标签占的宽度（含托盘的缩进）。
  int ItemWidth(size_t index) const;
  void PlaceCells(Change change);
  void Animate(TabCell* cell, const gfx::Rect& target, Change change);
  // ordinal：这次一起出现的里面的第几个（展开文件夹时后面的落得远一点）。
  void AnimateInsert(TabCell* cell, Change change, int ordinal);
  void AnimateRemove(TabCell* cell, Change change);
  // 侧栏的列表：对上这次的行（新的建、没了的淡出），算出每一行、每个标签的位置。
  void SyncRows(std::vector<TabListRow*>* inserted);
  void ComputeListGeometry();
  // 常用图块：每个标签、每个图块文件夹占第几格，每一排的上沿和高度。
  void ComputeGridGeometry();
  // 按现在的宽度重新算 entries_ 里每一格的位置。
  void FillGridEntries();
  // 第 slot 格的位置（slot 是 slot_count() 时是下一格）；tile：图块文件夹，
  // 连下面的名字。
  gfx::Rect GridSlotRect(size_t slot, int width, bool tile) const;
  gfx::Rect RowRect(size_t index) const;
  void PlaceRows(Change change, const std::vector<TabListRow*>& inserted);
  // 展开的文件夹、树里展开的父标签左边的层级线。
  void PlaceGuides(Change change);
  // 第 index 个标签在树里往右缩多少。
  int TreeInset(size_t index, int width) const;
  // 第 parent 个标签的层级线（上沿在 top）上，从上面到通往第 target 个标签的那一行
  // 的中线多长；target 不在它那一枝里是 0。
  int TreePathLength(size_t parent,
                     std::optional<size_t> target,
                     int top) const;
  // 悬停的标签那一路：层级线上提亮的一段。
  void PlaceGuideHover();
  void FadeOutAndRemove(TabListFill* fill);
  // 淡出完了：还在这里的话删掉（窗口关的时候可能已经没了）。
  void RemoveLeavingRow(base::WeakPtr<TabListRow> row);
  void PlaceHighlight(Change change, std::optional<gfx::Rect> from);
  // 同一组分屏的行垫一层圆角托盘（只有列表有，图块用右下角的小标）。
  void PlaceTrays(Change change);
  // 顶部标签行的固定区：文件夹的胶囊和挂在后面的当前标签垫一层托盘。
  void PlaceFolderTrays(Change change);
  // 高亮在第 index 个标签上时的位置（分屏的行左右缩进）。
  gfx::Rect HighlightRect(size_t index) const;
  MotionCurve CurveFor(Change change) const;
  void RemoveLeavingCell(base::WeakPtr<TabCell> cell);

  std::vector<TabListRowData> pending_rows_;
  std::vector<TabListRowData> rows_;
  std::vector<raw_ptr<TabListRow, VectorExperimental>> row_views_;
  // 每个标签在第几层（同 cells_ 的顺序）。
  std::vector<int> depths_;
  // 树状标签页：每个标签在树里的样子（同 cells_ 的顺序，不在树里的是空的）、最深的
  // 一层。
  std::vector<std::optional<Item::Tree>> trees_;
  int tree_depth_ = 0;
  std::vector<Entry> entries_;
  std::vector<int> tab_y_;
  std::vector<int> row_y_;
  int content_height_ = 0;
  // 顶部标签行的固定区：每个标签、每一行横着的位置和宽度，一共多宽；每个标签
  // 挂在哪个文件夹的胶囊后面。
  std::vector<int> tab_x_;
  std::vector<int> tab_w_;
  std::vector<int> row_x_;
  std::vector<int> row_w_;
  int content_width_ = 0;
  std::vector<std::string> tray_ids_;
  // 常用图块：每个标签、每一行占第几格，每一排的上沿和高度，一共几格。
  std::vector<size_t> tab_slot_;
  std::vector<size_t> row_slot_;
  std::vector<int> grid_row_y_;
  std::vector<int> grid_row_h_;
  size_t slot_count_ = 0;
  std::map<std::string, raw_ptr<TabListFill>> folder_trays_;
  // 这次开合列表是变长了（展开）还是变短了（折叠）。
  bool fold_grew_ = true;
  std::map<std::string, raw_ptr<TabListFill>> guides_;

  const Style style_;
  const raw_ptr<TabCellDelegate> delegate_;
  // 列表里的标签、行都在这一组（View::SetGroup）：Tab 键只在列表里停一站。
  const int focus_group_;
  Theme theme_ = Theme::DarkBase();
  int item_width_ = 220;
  std::vector<raw_ptr<TabCell, VectorExperimental>> cells_;

  raw_ptr<TabListFill> highlight_ = nullptr;
  LayerMotion highlight_motion_;
  std::optional<tabs::TabHandle> highlight_handle_;
  bool highlight_shown_ = false;

  // 每个标签所在的分屏和那组显示着没有（同 cells_ 的顺序）。
  std::vector<std::pair<std::optional<split_tabs::SplitTabId>, bool>> splits_;
  std::map<split_tabs::SplitTabId, raw_ptr<TabListFill>> trays_;

  raw_ptr<TabListFill> marker_ = nullptr;
  LayerMotion marker_motion_;
  // 缝里能选的另外几层的小圆圈。
  raw_ptr<TabListFill> hints_ = nullptr;
  LayerMotion hints_motion_;
  tabs::TabHandle hovered_tab_;
  std::optional<DropMarker> marker_state_;
  std::string drop_row_;
  tabs::TabHandle drop_cell_;

  base::WeakPtrFactory<TabListView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TAB_LIST_VIEW_H_
