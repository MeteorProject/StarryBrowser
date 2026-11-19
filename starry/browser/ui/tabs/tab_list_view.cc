// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tab_list_view.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/motion/frame_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 相邻两个标签之间的空隙：列表 2，网格 6。
constexpr int kListSpacing = 2;
constexpr int kGridSpacing = metrics::kPinnedSpacing;
constexpr int kGridColumns = metrics::kPinnedColumns;
// 顶部标签行里相邻两个标签之间。
constexpr int kTopSpacing = 4;
// 拖动时插入位置的提示线。
constexpr int kMarkerThickness = 2;
// 侧栏的列表里提示线左端的小圆头的直径；线离行的两边留一点。
constexpr int kMarkerHead = 6;
constexpr int kMarkerInset = 2;
// 列表上下各留 2px：分屏托盘比行上下各多出 2px，避免首尾行的托盘被裁切。
constexpr int kListPadding = 2;
// 层级线在文件夹图标的正下方（行左边 10 + 图标的一半 8），宽 1。
constexpr int kGuideX = 17;
// 树状标签页的缩进：每层 6–14，标题至少留 92；行里不算标题的部分是左 10、图标 16、
// 间距 10、右 6。缩进窄于 20 时层级线在子标签行左边 3，不压进它的高亮。
constexpr int kTreeMaxStep = 14;
constexpr int kTreeMinStep = 6;
constexpr int kTreeMinTitle = 92;
constexpr int kTreeRowChrome = 42;
constexpr int kTreeGuideGap = 3;
// 展开文件夹时一起出现的行，后面的多落这么远，最多算到第 8 个。
constexpr int kRevealStep = 3;
constexpr int kRevealLimit = 8;
// 顶部标签行里插入线的高度。
constexpr int kTopMarkerHeight = 18;
// 聚焦一组的面包屑出现时从这么大放大。
constexpr float kHoistInsertScale = 0.98f;
// 树：悬停的那一路的层级线叠这么深的一层（加上线本来的 10% 大约 26%）。
constexpr float kTreeHoverOpacity = 0.18f;

SkColor WithOpacity(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(255 * opacity));
}

// 树里一个父标签的层级线在 guides_ 里的 ID。
std::string TreeGuideId(tabs::TabHandle handle) {
  return "@tree/" + base::NumberToString(handle.raw_value());
}

}  // namespace

// 高亮和提示线：自己有图层的一块圆角色块，不接鼠标。
class TabListFill : public views::View {
  METADATA_HEADER(TabListFill, views::View)

 public:
  TabListFill() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetCanProcessEventsWithinSubtree(false);
  }
  ~TabListFill() override = default;

  base::WeakPtr<TabListFill> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  void SetColor(SkColor color, float radius) {
    if (color_ == color && radius_ == radius) {
      return;
    }
    color_ = color;
    radius_ = radius;
    SchedulePaint();
  }

  void SetBorderColor(SkColor color) {
    if (border_ != color) {
      border_ = color;
      SchedulePaint();
    }
  }

  // 从上面开始 length 这么长一段叠一层另一种颜色（树里的当前路径）；length 为 0 是没有。
  // 有 curve 时从现在的长度伸缩过去（同选中的高亮、行的挪动）。
  void SetOverlay(SkColor color, int length, std::optional<MotionCurve> curve) {
    if (overlay_ != color) {
      overlay_ = color;
      SchedulePaint();
    }
    if (overlay_length_.target() == length && !overlay_length_.is_animating()) {
      if (overlay_length_.value() != length) {
        overlay_length_.SetValue(length);
        SchedulePaint();
      }
      return;
    }
    if (curve) {
      overlay_length_.AnimateTo(Compositor(), length, *curve, 0.5);
    } else {
      overlay_length_.SetValue(length);
      SchedulePaint();
    }
  }

  // 悬停的那一路：从上面开始 length 这么长一段叠一层 color，淡入；length 为 0 时淡出。
  void SetHover(SkColor color, int length) {
    hover_ = color;
    if (length > 0) {
      hover_length_ = length;
    }
    hover_alpha_.AnimateTo(Compositor(), length > 0 ? 1 : 0, motion::Hover());
    SchedulePaint();
  }

  // 层级线变长：从 from 这么长伸到现在的高度；没有 curve 时直接是现在的高度。
  void Grow(int from, std::optional<MotionCurve> curve) {
    if (!curve || from >= height()) {
      grown_.SetValue(kFullLength);
      SchedulePaint();
      return;
    }
    grown_.SetValue(
        grown_.is_animating() ? std::min<double>(from, grown_.value()) : from);
    grown_.AnimateTo(Compositor(), height(), *curve, 0.5);
  }

  // 画成一排小圆环（中心的 x，竖着在正中）：缝里能选的另外几层。
  void SetRings(std::vector<float> centers) {
    if (rings_ != centers) {
      rings_ = std::move(centers);
      SchedulePaint();
    }
  }

  // 画成左端带一个小圆环的细线（列表里的插入线）。
  void SetHead(bool head) {
    if (head_ != head) {
      head_ = head;
      SchedulePaint();
    }
  }

  LayerMotion& motion() { return motion_; }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(color_);
    if (!rings_.empty()) {
      const float mid = height() / 2.0f;
      flags.setStyle(cc::PaintFlags::kStroke_Style);
      flags.setStrokeWidth(1.5f);
      for (const float x : rings_) {
        canvas->DrawCircle(gfx::PointF(x, mid), mid - 0.75f, flags);
      }
      return;
    }
    if (head_) {
      const float mid = height() / 2.0f;
      cc::PaintFlags ring = flags;
      ring.setStyle(cc::PaintFlags::kStroke_Style);
      ring.setStrokeWidth(1.5f);
      canvas->DrawCircle(gfx::PointF(mid, mid), mid - 0.75f, ring);
      canvas->DrawRoundRect(
          gfx::RectF(height(), mid - kMarkerThickness / 2.0f,
                     std::max(0, width() - height()), kMarkerThickness),
          kMarkerThickness / 2.0f, flags);
      return;
    }
    // 层级线伸长的过程中只画上面一段。
    const float drawn =
        grown_.is_animating()
            ? std::min(static_cast<float>(height()),
                       static_cast<float>(std::max(0.0, grown_.value())))
            : height();
    const float radius = std::min(radius_, std::min<float>(width(), drawn) / 2);
    canvas->DrawRoundRect(gfx::RectF(0, 0, width(), drawn), radius, flags);
    const float hover = static_cast<float>(hover_alpha_.value());
    if (hover > 0 && hover_length_ > 0) {
      cc::PaintFlags paint;
      paint.setAntiAlias(true);
      paint.setColor(SkColorSetA(
          hover_, static_cast<U8CPU>(SkColorGetA(hover_) *
                                     std::clamp(hover, 0.0f, 1.0f))));
      canvas->DrawRoundRect(
          gfx::RectF(0, 0, width(), std::min<float>(hover_length_, drawn)),
          radius, paint);
    }
    const float overlay_length = static_cast<float>(overlay_length_.value());
    if (overlay_length > 0.5f) {
      cc::PaintFlags overlay;
      overlay.setAntiAlias(true);
      overlay.setColor(overlay_);
      canvas->DrawRoundRect(
          gfx::RectF(0, 0, width(), std::min(overlay_length, drawn)), radius,
          overlay);
    }
    if (SkColorGetA(border_) > 0) {
      cc::PaintFlags border;
      border.setAntiAlias(true);
      border.setStyle(cc::PaintFlags::kStroke_Style);
      border.setStrokeWidth(1);
      border.setColor(border_);
      gfx::RectF stroke(GetLocalBounds());
      stroke.Inset(0.5f);
      canvas->DrawRoundRect(stroke, std::max(0.0f, radius - 0.5f), border);
    }
  }

 private:
  // 没在伸长：画整条。
  static constexpr double kFullLength = 1e6;

  ui::Compositor* Compositor() {
    return GetWidget() ? GetWidget()->GetCompositor() : nullptr;
  }
  base::RepeatingClosure Repaint() {
    return base::BindRepeating(&TabListFill::SchedulePaint,
                               base::Unretained(this));
  }

  SkColor color_ = SK_ColorTRANSPARENT;
  SkColor border_ = SK_ColorTRANSPARENT;
  SkColor overlay_ = SK_ColorTRANSPARENT;
  SkColor hover_ = SK_ColorTRANSPARENT;
  int hover_length_ = 0;
  float radius_ = 0;
  bool head_ = false;
  std::vector<float> rings_;
  FrameMotion overlay_length_{0, Repaint()};
  FrameMotion hover_alpha_{0, Repaint()};
  FrameMotion grown_{kFullLength, Repaint()};
  LayerMotion motion_;
  base::WeakPtrFactory<TabListFill> weak_factory_{this};
};

BEGIN_METADATA(TabListFill)
END_METADATA

void TabListView::SetRows(std::vector<TabListRowData> rows) {
  pending_rows_ = std::move(rows);
}

void TabListView::SetDropRow(const std::string& id) {
  if (drop_row_ == id) {
    return;
  }
  drop_row_ = id;
  for (TabListRow* row : row_views_) {
    row->SetDropTarget(!id.empty() && row->data().id == id);
  }
}

void TabListView::SetDropCell(tabs::TabHandle handle) {
  if (drop_cell_ == handle) {
    return;
  }
  drop_cell_ = handle;
  for (TabCell* cell : cells_) {
    cell->SetDropTarget(handle != tabs::TabHandle() &&
                        cell->handle() == handle);
  }
}

void TabListView::SnapInPlace(tabs::TabHandle handle,
                              const std::string& folder) {
  auto snap = [this](TabCell* cell) {
    cell->motion().SetTransform(cell->layer(), LayerPose());
    if (highlight_shown_ && highlight_handle_ == cell->handle()) {
      highlight_motion_.SetTransform(highlight_->layer(), LayerPose());
    }
  };
  if (TabCell* cell = CellFor(handle)) {
    snap(cell);
  }
  if (folder.empty()) {
    return;
  }
  // 常用图块里拖的是图块文件夹。
  if (pinned()) {
    for (TabListRow* row : row_views_) {
      if (row->data().id == folder) {
        row->motion().SetTransform(row->layer(), LayerPose());
      }
    }
    return;
  }
  std::optional<int> depth;
  for (const Entry& entry : entries_) {
    if (depth) {
      if (entry.depth <= *depth) {
        break;
      }
    } else if (entry.row && rows_[entry.index].id == folder) {
      depth = entry.depth;
    } else {
      continue;
    }
    if (!entry.row) {
      snap(cells_[entry.index]);
      continue;
    }
    TabListRow* row = row_views_[entry.index];
    row->motion().SetTransform(row->layer(), LayerPose());
    if (auto guide = guides_.find(rows_[entry.index].id);
        guide != guides_.end()) {
      guide->second->motion().SetTransform(guide->second->layer(), LayerPose());
    }
  }
}

bool TabListView::BeginRename(const std::string& id) {
  for (TabListRow* row : row_views_) {
    if (row->data().id == id &&
        row->data().kind == TabListRowData::Kind::kFolder) {
      row->ScrollViewToVisible();
      row->BeginEdit();
      return row->editing();
    }
  }
  return false;
}

std::vector<views::View*> TabListView::FocusOrder() const {
  std::vector<views::View*> order;
  for (TabCell* cell : cells_) {
    if (!cell->leaving() && cell->IsFocusable()) {
      order.push_back(cell);
    }
  }
  for (TabListRow* row : row_views_) {
    if (row->IsFocusable()) {
      order.push_back(row);
    }
  }
  std::ranges::stable_sort(order, {}, [](const views::View* view) {
    return std::make_pair(view->y(), view->x());
  });
  return order;
}

views::View* TabListView::FocusEntry() {
  for (TabCell* cell : cells_) {
    if (cell->selected() && !cell->leaving() && cell->IsFocusable()) {
      return cell;
    }
  }
  const std::vector<views::View*> order = FocusOrder();
  return order.empty() ? nullptr : order.front();
}

// static
views::View* TabListView::FocusEntryFor(views::View* item) {
  auto* list = item->parent() ? views::AsViewClass<TabListView>(item->parent())
                              : nullptr;
  return list ? list->FocusEntry() : nullptr;
}

tabs::TabInterface* TabListView::TabAt(size_t index) const {
  return index < cells_.size() ? cells_[index]->handle().Get() : nullptr;
}

std::optional<size_t> TabListView::IndexOf(tabs::TabHandle handle) const {
  for (size_t i = 0; i < cells_.size(); ++i) {
    if (cells_[i]->handle() == handle) {
      return i;
    }
  }
  return std::nullopt;
}

namespace {
// 每个列表一个组号（组号相同的视图在 Tab 键的顺序里算一站）。
int NextFocusGroup() {
  static int next = 0x5354;
  return next++;
}
}  // namespace

TabListView::TabListView(Style style, TabCellDelegate* delegate)
    : style_(style), delegate_(delegate), focus_group_(NextFocusGroup()) {
  SetNotifyEnterExitOnChild(true);
  highlight_ = AddChildView(std::make_unique<TabListFill>());
  marker_ = AddChildView(std::make_unique<TabListFill>());
  hints_ = AddChildView(std::make_unique<TabListFill>());
}

TabListView::~TabListView() = default;

void TabListView::Update(const std::vector<Item>& items,
                         const Theme& theme,
                         Change change,
                         std::optional<gfx::Rect> highlight_from) {
  theme_ = theme;
  const size_t old_count = cells_.size();
  const int old_width = horizontal() ? ContentWidth() : 0;
  const int old_height = content_height_;

  std::map<tabs::TabHandle, TabCell*> old;
  for (TabCell* cell : cells_) {
    old[cell->handle()] = cell;
  }
  std::vector<raw_ptr<TabCell, VectorExperimental>> next;
  std::vector<TabCell*> inserted;
  TabCellStyle list_style = TabCellStyle::kRow;
  switch (style_) {
    case Style::kList:
      list_style = TabCellStyle::kRow;
      break;
    case Style::kGrid:
      list_style = TabCellStyle::kTile;
      break;
    case Style::kTop:
      list_style = TabCellStyle::kTopRow;
      break;
    case Style::kTopPinned:
    case Style::kTopPins:
      list_style = TabCellStyle::kTopTile;
      break;
  }
  for (const Item& item : items) {
    // 顶部标签行的固定区：撑开的是带标题的标签，别的只有图标。
    const TabCellStyle cell_style = style_ == Style::kTopPins && item.wide
                                        ? TabCellStyle::kTopRow
                                        : list_style;
    TabCell* cell = nullptr;
    auto it = old.find(item.handle);
    if (it != old.end()) {
      cell = it->second;
      old.erase(it);
      cell->SetStyle(cell_style);
    } else {
      cell = AddChildView(std::make_unique<TabCell>(cell_style, delegate_));
      cell->SetGroup(focus_group_);
      inserted.push_back(cell);
    }
    cell->SetData(item.handle, item.data, item.selected, theme);
    cell->SetSplit(item.split.has_value(), item.split_active);
    if (item.tree) {
      cell->SetTree(item.tree->parent, item.tree->collapsed, item.tree->count,
                    item.tree->icons);
    } else {
      cell->SetTree(false, false, 0, {});
    }
    next.push_back(cell);
  }
  splits_.clear();
  for (const Item& item : items) {
    splits_.emplace_back(item.split, item.split_active);
  }
  depths_.clear();
  tray_ids_.clear();
  trees_.clear();
  tree_depth_ = 0;
  for (const Item& item : items) {
    depths_.push_back(item.depth);
    tray_ids_.push_back(item.tray);
    trees_.push_back(item.tree);
    if (item.tree) {
      tree_depth_ = std::max(tree_depth_, item.tree->depth);
    }
  }
  for (auto& [handle, cell] : old) {
    AnimateRemove(cell, change);
  }
  cells_ = std::move(next);

  std::vector<TabListRow*> inserted_rows;
  if (style_ == Style::kList) {
    SyncRows(&inserted_rows);
    ComputeListGeometry();
    fold_grew_ = content_height_ >= old_height;
  } else if (style_ == Style::kTopPins) {
    SyncRows(&inserted_rows);
    entries_.clear();
    content_width_ = LayoutTopPins(rows_, items, &entries_);
    tab_x_.assign(cells_.size(), content_width_);
    tab_w_.assign(cells_.size(), 0);
    row_x_.assign(rows_.size(), content_width_);
    row_w_.assign(rows_.size(), 0);
    for (const Entry& entry : entries_) {
      (entry.row ? row_x_ : tab_x_)[entry.index] = entry.x;
      (entry.row ? row_w_ : tab_w_)[entry.index] = entry.width;
    }
    fold_grew_ = content_width_ >= old_width;
  } else if (pinned()) {
    SyncRows(&inserted_rows);
    ComputeGridGeometry();
  }

  int ordinal = 0;
  for (size_t i = 0; i < cells_.size(); ++i) {
    TabCell* cell = cells_[i];
    const auto [tray_left, tray_right] = TrayInsets(i);
    cell->SetTrayInsets(tray_left, tray_right);
    const gfx::Rect target = SlotRect(i);
    if (std::ranges::find(inserted, cell) != inserted.end()) {
      cell->SetBoundsRect(target);
      if (change != Change::kNone) {
        AnimateInsert(cell, change, ordinal++);
      }
    } else {
      Animate(cell, target, change);
    }
  }

  if (style_ != Style::kTop) {
    PlaceRows(change, inserted_rows);
  }
  if (style_ == Style::kList) {
    PlaceGuides(change);
  }
  // 托盘、层级线在最下面，高亮在所有标签下面，提示线在最上面。
  PlaceTrays(change);
  PlaceFolderTrays(change);
  ReorderChildView(highlight_, 0);
  for (auto& [id, tray] : trays_) {
    ReorderChildView(tray, 0);
  }
  for (auto& [id, tray] : folder_trays_) {
    ReorderChildView(tray, 0);
  }
  for (auto& [id, guide] : guides_) {
    ReorderChildView(guide, 0);
  }
  ReorderChildView(hints_, children().size());
  ReorderChildView(marker_, children().size());
  PlaceHighlight(change, highlight_from);
  // 分屏托盘的头尾缩进也占宽度，标签数量没变时也要刷新滚动内容的首选尺寸。
  if (cells_.size() != old_count || content_height_ != old_height ||
      (horizontal() && ContentWidth() != old_width)) {
    PreferredSizeChanged();
  }
}

TabCell* TabListView::CellFor(tabs::TabHandle handle) {
  for (TabCell* cell : cells_) {
    if (cell->handle() == handle) {
      return cell;
    }
  }
  return nullptr;
}

std::optional<gfx::Rect> TabListView::VisibleHighlightRect() const {
  if (!highlight_shown_) {
    return std::nullopt;
  }
  const LayerPose pose =
      const_cast<LayerMotion&>(highlight_motion_)
          .CurrentPose(highlight_->layer());
  gfx::Rect rect = highlight_->bounds();
  rect.Offset(static_cast<int>(pose.offset.x()),
              static_cast<int>(pose.offset.y()));
  return rect;
}

gfx::Rect TabListView::SlotRect(size_t index) const {
  return SlotRectFor(index, width());
}

gfx::Rect TabListView::HighlightRect(size_t index) const {
  gfx::Rect rect = SlotRect(index);
  const auto [left, right] = TrayInsets(index);
  rect.Inset(gfx::Insets::TLBR(0, left, 0, right));
  return rect;
}

std::pair<int, int> TabListView::TrayInsets(size_t index) const {
  if (index >= splits_.size() || !splits_[index].first || pinned() ||
      style_ == Style::kTopPins) {
    return {0, 0};
  }
  if (style_ == Style::kList) {
    return {kSplitTrayInset, kSplitTrayInset};
  }
  // 标签行：只缩一组头尾的外侧。
  const std::optional<split_tabs::SplitTabId>& split = splits_[index].first;
  const bool first = index == 0 || splits_[index - 1].first != split;
  const bool last =
      index + 1 >= splits_.size() || splits_[index + 1].first != split;
  return {first ? kSplitTrayInset : 0, last ? kSplitTrayInset : 0};
}

int TabListView::ItemWidth(size_t index) const {
  if (style_ == Style::kTopPinned) {
    return metrics::kTopPinnedTabWidth;
  }
  if (style_ == Style::kTopPins) {
    return index < tab_w_.size() ? tab_w_[index] : 0;
  }
  const auto [left, right] = TrayInsets(index);
  return item_width_ + left + right;
}

size_t TabListView::slot_count() const {
  return pinned() ? slot_count_ : cells_.size();
}

int TabListView::ContentWidth() const {
  if (style_ == Style::kTopPins) {
    return content_width_;
  }
  if (style_ == Style::kTopPinned) {
    const int slots = static_cast<int>(slot_count_);
    return slots > 0 ? slots * (metrics::kTopPinnedTabWidth + kTopSpacing) -
                           kTopSpacing
                     : 0;
  }
  int width = 0;
  for (size_t i = 0; i < cells_.size(); ++i) {
    width += ItemWidth(i) + (i > 0 ? kTopSpacing : 0);
  }
  return width;
}

// static
int TabListView::LayoutTopPins(const std::vector<TabListRowData>& rows,
                               const std::vector<Item>& items,
                               std::vector<Entry>* entries) {
  // 一个接一个，隔 kTopSpacing；托盘（胶囊和挂在后面的标签）左右各多出
  // kTrayPad，里面两个隔 kTrayGap。
  int x = 0;
  bool first = true;
  auto place = [&](bool row, size_t index, int width, bool tray_head,
                   bool tray_tail) {
    if (!first) {
      x += tray_tail ? kTrayGap : kTopSpacing;
    }
    first = false;
    if (tray_head) {
      x += kTrayPad;
    }
    if (entries) {
      Entry entry;
      entry.row = row;
      entry.index = index;
      entry.x = x;
      entry.width = width;
      entry.height = metrics::kTopTabHeight;
      entries->push_back(entry);
    }
    x += width;
    if (tray_tail) {
      x += kTrayPad;
    }
  };
  size_t next_row = 0;
  for (size_t i = 0; i <= items.size(); ++i) {
    while (next_row < rows.size() &&
           (rows[next_row].before_tab <= i || i == items.size())) {
      const TabListRowData& row = rows[next_row];
      // 托盘的头：后面紧跟着挂在它上面的标签。
      const bool head = i < items.size() && items[i].tray == row.id;
      place(true, next_row, TabListRow::ChipWidth(row), head, false);
      ++next_row;
    }
    if (i < items.size()) {
      const Item& item = items[i];
      place(false, i, item.wide ? item.width : metrics::kTopPinnedTabWidth,
            false, !item.tray.empty());
    }
  }
  return x;
}

void TabListView::SetItemWidth(int width, Change change) {
  if (item_width_ == width) {
    return;
  }
  item_width_ = width;
  if (style_ != Style::kTop) {
    return;
  }
  PlaceCells(change);
  PlaceTrays(change);
  PlaceHighlight(change, std::nullopt);
  PreferredSizeChanged();
}

void TabListView::PlaceCells(Change change) {
  for (size_t i = 0; i < cells_.size(); ++i) {
    Animate(cells_[i], SlotRect(i), change);
  }
}

void TabListView::PlaceTrays(Change change) {
  // 同一组相邻的标签并成一块，上下各多出 2（标签行里 3）。
  std::map<split_tabs::SplitTabId, std::pair<gfx::Rect, bool>> groups;
  if (!pinned() && style_ != Style::kTopPins) {
    for (size_t i = 0; i < splits_.size() && i < cells_.size(); ++i) {
      const std::optional<split_tabs::SplitTabId>& split = splits_[i].first;
      if (!split) {
        continue;
      }
      const gfx::Rect slot = SlotRect(i);
      auto it = groups.find(*split);
      if (it == groups.end()) {
        groups[*split] = {slot, splits_[i].second};
      } else {
        it->second.first.Union(slot);
      }
    }
  }
  for (auto it = trays_.begin(); it != trays_.end();) {
    if (groups.contains(it->first)) {
      ++it;
      continue;
    }
    TabListFill* tray = it->second;
    it = trays_.erase(it);
    FadeOutAndRemove(tray);
  }
  const bool dark = theme_.is_dark;
  for (const auto& [id, group] : groups) {
    gfx::Rect rect = group.first;
    rect.Outset(gfx::Outsets::VH(horizontal() ? kSplitTrayInset : 2, 0));
    const bool active = group.second;
    TabListFill* tray = nullptr;
    if (auto it = trays_.find(id); it != trays_.end()) {
      tray = it->second;
      const gfx::Rect old = tray->bounds();
      tray->SetBoundsRect(rect);
      if (old.origin() != rect.origin() && change != Change::kNone &&
          !motion::ReduceMotion()) {
        tray->motion().ShiftAndAnimate(tray->layer(),
                                       old.origin() - rect.origin(),
                                       LayerPose(), CurveFor(change));
      }
    } else {
      tray = AddChildView(std::make_unique<TabListFill>());
      trays_[id] = tray;
      tray->SetBoundsRect(rect);
      if (change == Change::kNone) {
        tray->motion().SetOpacity(tray->layer(), 1);
      } else {
        tray->motion().AnimateOpacity(tray->layer(), 1, motion::Hover());
      }
    }
    tray->SetColor(active ? WithOpacity(theme_.accent, dark ? 0.1f : 0.07f)
                          : theme_.OnSurface(0.04f),
                   radius::kRow + kSplitTrayInset);
    tray->SetBorderColor(active ? WithOpacity(theme_.accent, 0.32f)
                           : theme_.OnSurface(0.08f));
  }
}

void TabListView::PlaceFolderTrays(Change change) {
  // 胶囊和挂在后面的当前标签并成一块，四周各多出 kTrayPad。
  std::map<std::string, gfx::Rect> wanted;
  if (style_ == Style::kTopPins) {
    for (size_t i = 0; i < cells_.size() && i < tray_ids_.size(); ++i) {
      if (tray_ids_[i].empty()) {
        continue;
      }
      for (size_t r = 0; r < rows_.size(); ++r) {
        if (rows_[r].id == tray_ids_[i]) {
          gfx::Rect rect = RowRect(r);
          rect.Union(SlotRect(i));
          rect.Outset(kTrayPad);
          wanted[tray_ids_[i]] = rect;
        }
      }
    }
  }
  for (auto it = folder_trays_.begin(); it != folder_trays_.end();) {
    if (wanted.contains(it->first)) {
      ++it;
      continue;
    }
    TabListFill* tray = it->second;
    it = folder_trays_.erase(it);
    FadeOutAndRemove(tray);
  }
  for (const auto& [id, rect] : wanted) {
    TabListFill* tray = nullptr;
    if (auto it = folder_trays_.find(id); it != folder_trays_.end()) {
      tray = it->second;
      const gfx::Rect old = tray->bounds();
      tray->SetBoundsRect(rect);
      if (old.origin() != rect.origin() && change != Change::kNone &&
          !motion::ReduceMotion()) {
        tray->motion().ShiftAndAnimate(tray->layer(),
                                       old.origin() - rect.origin(),
                                       LayerPose(), CurveFor(change));
      }
    } else {
      tray = AddChildView(std::make_unique<TabListFill>());
      folder_trays_[id] = tray;
      tray->SetBoundsRect(rect);
      if (change == Change::kNone) {
        tray->motion().SetOpacity(tray->layer(), 1);
      } else {
        tray->motion().AnimateOpacity(tray->layer(), 1, motion::Hover());
      }
    }
    // 当前标签在里面：和显示着的分屏托盘同一种画法，淡一点。
    tray->SetColor(WithOpacity(theme_.accent, theme_.is_dark ? 0.07f : 0.05f),
                   radius::kRow + kTrayPad);
    tray->SetBorderColor(WithOpacity(theme_.accent, 0.22f));
  }
}

gfx::Rect TabListView::SlotRectFor(size_t index, int width) const {
  if (pinned()) {
    const size_t slot =
        index < tab_slot_.size() ? tab_slot_[index] : slot_count_;
    return GridSlotRect(slot, width, /*tile=*/false);
  }
  if (style_ == Style::kTopPins) {
    const int y = std::max(0, (height() - metrics::kTopTabHeight) / 2);
    if (index < tab_x_.size()) {
      return gfx::Rect(tab_x_[index], y, tab_w_[index], metrics::kTopTabHeight);
    }
    return gfx::Rect(content_width_, y, 0, metrics::kTopTabHeight);
  }
  if (horizontal()) {
    // 横排：一个接一个，上下居中。
    int x = 0;
    for (size_t i = 0; i < index; ++i) {
      x += ItemWidth(i) + kTopSpacing;
    }
    const int y = std::max(0, (height() - metrics::kTopTabHeight) / 2);
    return gfx::Rect(x, y, ItemWidth(index), metrics::kTopTabHeight);
  }
  if (style_ == Style::kList) {
    const int inset =
        index < trees_.size() && trees_[index] ? TreeInset(index, width)
        : index < depths_.size() ? TabListRow::IndentFor(depths_[index])
                                 : 0;
    const int y = index < tab_y_.size()
                      ? tab_y_[index]
                      : std::max(kListPadding, content_height_ - kListPadding);
    return gfx::Rect(inset, y, std::max(0, width - inset),
                     metrics::kTabRowHeight);
  }
  return gfx::Rect();
}

gfx::Rect TabListView::GridSlotRect(size_t slot, int width, bool tile) const {
  if (style_ == Style::kTopPinned) {
    // 顶部标签行：一个接一个，上下居中。
    const int y = std::max(0, (height() - metrics::kTopTabHeight) / 2);
    return gfx::Rect(
        static_cast<int>(slot) * (metrics::kTopPinnedTabWidth + kTopSpacing), y,
        metrics::kTopPinnedTabWidth, metrics::kTopTabHeight);
  }
  const int tile_width =
      std::max(0, (width - (kGridColumns - 1) * kGridSpacing) / kGridColumns);
  const int column = static_cast<int>(slot % kGridColumns);
  const size_t row = slot / kGridColumns;
  // 最后一列吃掉除不尽的几个像素，右边和列表对齐。
  const int x = column * (tile_width + kGridSpacing);
  const int w = column == kGridColumns - 1 ? width - x : tile_width;
  int y = 0;
  int h = metrics::kPinnedTileHeight;
  if (row < grid_row_y_.size()) {
    y = grid_row_y_[row];
    if (tile) {
      h = grid_row_h_[row];
    }
  } else if (!grid_row_y_.empty()) {
    y = grid_row_y_.back() + grid_row_h_.back() + kGridSpacing;
  }
  return gfx::Rect(x, y, w, h);
}

void TabListView::ComputeGridGeometry() {
  // 行（图块文件夹）排在第 before_tab 个标签之前，一项一格。
  tab_slot_.assign(cells_.size(), 0);
  row_slot_.assign(rows_.size(), 0);
  std::vector<bool> tiles;
  size_t next_row = 0;
  for (size_t i = 0; i <= cells_.size(); ++i) {
    while (next_row < rows_.size() &&
           (rows_[next_row].before_tab <= i || i == cells_.size())) {
      row_slot_[next_row++] = tiles.size();
      tiles.push_back(true);
    }
    if (i < cells_.size()) {
      tab_slot_[i] = tiles.size();
      tiles.push_back(false);
    }
  }
  slot_count_ = tiles.size();
  grid_row_y_.clear();
  grid_row_h_.clear();
  content_height_ = 0;
  if (style_ == Style::kGrid) {
    // 有图块文件夹的那一排下面多一行名字。
    int y = 0;
    for (size_t first = 0; first < slot_count_; first += kGridColumns) {
      const size_t last = std::min(slot_count_, first + kGridColumns);
      const bool caption =
          std::any_of(tiles.begin() + first, tiles.begin() + last,
                      [](bool tile) { return tile; });
      const int height = metrics::kPinnedTileHeight +
                         (caption ? TabListRow::kTileCaptionHeight : 0);
      grid_row_y_.push_back(y);
      grid_row_h_.push_back(height);
      y += height + kGridSpacing;
    }
    content_height_ = grid_row_y_.empty() ? 0 : y - kGridSpacing;
  }
  FillGridEntries();
}

void TabListView::FillGridEntries() {
  entries_.assign(slot_count_, Entry());
  auto fill = [this](size_t slot, bool row, size_t index) {
    if (slot >= entries_.size()) {
      return;
    }
    const gfx::Rect rect = GridSlotRect(slot, width(), row);
    Entry& entry = entries_[slot];
    entry.row = row;
    entry.index = index;
    entry.x = rect.x();
    entry.y = rect.y();
    entry.width = rect.width();
    entry.height = rect.height();
  };
  for (size_t i = 0; i < tab_slot_.size(); ++i) {
    fill(tab_slot_[i], false, i);
  }
  for (size_t i = 0; i < row_slot_.size(); ++i) {
    fill(row_slot_[i], true, i);
  }
}

int TabListView::PreferredHeightFor(size_t count) const {
  if (style_ == Style::kList || style_ == Style::kGrid) {
    return content_height_;
  }
  // 横排的：顶部标签行的常用图块按格数。
  if (style_ == Style::kTopPinned) {
    count = slot_count_;
  }
  return count == 0 ? 0 : metrics::kTopTabHeight;
}

size_t TabListView::DropIndexAt(const gfx::Point& point) const {
  if (cells_.empty()) {
    return 0;
  }
  if (horizontal()) {
    size_t index = 0;
    while (index < cells_.size() &&
           SlotRect(index).CenterPoint().x() < point.x()) {
      ++index;
    }
    return index;
  }
  if (style_ == Style::kList) {
    size_t index = 0;
    while (index < cells_.size() &&
           SlotRect(index).CenterPoint().y() < point.y()) {
      ++index;
    }
    return index;
  }
  const int pitch = metrics::kPinnedTileHeight + kGridSpacing;
  const int rows =
      static_cast<int>((cells_.size() + kGridColumns - 1) / kGridColumns);
  const int row = std::clamp(point.y() / std::max(pitch, 1), 0, rows - 1);
  size_t index = static_cast<size_t>(row * kGridColumns);
  while (index < cells_.size() &&
         static_cast<int>(index) / kGridColumns == row &&
         SlotRect(index).CenterPoint().x() < point.x()) {
    ++index;
  }
  return index;
}

void TabListView::SetDropMarker(std::optional<DropMarker> marker) {
  if (marker == marker_state_) {
    return;
  }
  const bool was_shown = marker_state_.has_value();
  const bool hints_were_shown =
      was_shown && marker_state_->tree_depth &&
      marker_state_->tree_max > marker_state_->tree_min;
  marker_state_ = marker;
  if (!marker) {
    marker_motion_.AnimateOpacity(
        marker_->layer(), 0, MotionCurve::EaseOut(motion::kDropMarkerDuration));
    hints_motion_.AnimateOpacity(
        hints_->layer(), 0, MotionCurve::EaseOut(motion::kDropMarkerDuration));
    return;
  }
  const std::optional<size_t> index = marker->index;
  bool head = false;
  gfx::Rect rect;
  if (pinned()) {
    // 常用图块：index 是第几格；竖着的线在两格之间（最后一格的后面）。
    const size_t slot = std::min(*index, slot_count_);
    const bool end = slot >= slot_count_;
    const gfx::Rect tile = GridSlotRect(
        slot_count_ == 0 ? 0 : (end ? slot_count_ - 1 : slot), width(), false);
    if (style_ == Style::kTopPinned) {
      const int x =
          slot_count_ == 0 ? 0 : (end ? tile.right() + 1 : tile.x() - 3);
      rect = gfx::Rect(std::max(x, 0), tile.CenterPoint().y() - 9,
                       kMarkerThickness, 18);
    } else {
      const int x = slot_count_ == 0 ? 0
                    : end            ? tile.right() + kGridSpacing / 2
                                     : tile.x() - kGridSpacing / 2;
      rect = gfx::Rect(x - kMarkerThickness / 2, tile.y() + 8, kMarkerThickness,
                       std::max(0, tile.height() - 16));
    }
  } else if (horizontal() && marker->x) {
    // 顶部标签行的固定区：位置由 TabsView 定（两项之间的缝的正中）。
    rect = gfx::Rect(*marker->x - kMarkerThickness / 2,
                     height() / 2 - kTopMarkerHeight / 2, kMarkerThickness,
                     kTopMarkerHeight);
  } else if (horizontal()) {
    // 竖直插入指示线：高 18，置于标签左侧 3px 或最后一项右侧 1px。
    gfx::Rect tab;
    int x = 0;
    if (cells_.empty()) {
      tab = SlotRect(0);
    } else if (*index < cells_.size()) {
      tab = SlotRect(*index);
      x = tab.x() - 3;
    } else {
      tab = SlotRect(cells_.size() - 1);
      x = tab.right() + 1;
    }
    rect = gfx::Rect(std::max(x, 0), tab.CenterPoint().y() - 9,
                     kMarkerThickness, 18);
  } else if (style_ == Style::kList) {
    int y = 0;
    if (marker->y) {
      y = *marker->y;
    } else if (cells_.empty()) {
      // 只有「新标签页」那些行：放在它们下面。
      y = std::max(0, content_height_ - kListPadding) + kListSpacing / 2;
    } else if (*index < cells_.size()) {
      y = SlotRect(*index).y() - kListSpacing / 2;
    } else {
      y = SlotRect(cells_.size() - 1).bottom() + kListSpacing / 2;
    }
    // 树里：线缩到放下以后的层级（没给时和下面那一行同级）。
    const bool tree = !marker->y && *index < cells_.size() &&
                      *index < trees_.size() && trees_[*index];
    int indent = TabListRow::IndentFor(marker->depth);
    if (marker->tree_depth) {
      indent =
          std::min(*marker->tree_depth, TreeCap(width())) * TreeStep(width());
    } else if (tree) {
      indent = SlotRect(*index).x();
    }
    const int inset = indent + kMarkerInset;
    rect = gfx::Rect(inset, y - kMarkerHead / 2,
                     std::max(0, width() - inset - kMarkerInset), kMarkerHead);
    head = true;
    // 缝里能选的另外几层：在它们的缩进处画淡淡的小圆圈（同一个缩进的只画一个）。
    std::vector<float> rings;
    int left = 0;
    if (marker->tree_depth && marker->tree_max > marker->tree_min) {
      const int cap = TreeCap(width());
      const int step = TreeStep(width());
      left = std::min(marker->tree_min, cap) * step + kMarkerInset;
      for (int depth = marker->tree_min; depth <= marker->tree_max; ++depth) {
        const int x = std::min(depth, cap) * step + kMarkerInset;
        if (x == inset ||
            (!rings.empty() && rings.back() == x - left + kMarkerHead / 2.0f)) {
          continue;
        }
        rings.push_back(x - left + kMarkerHead / 2.0f);
      }
    }
    const MotionCurve fade = MotionCurve::EaseOut(motion::kDropMarkerDuration);
    if (rings.empty()) {
      hints_motion_.AnimateOpacity(hints_->layer(), 0, fade);
    } else {
      const gfx::Rect hints(
          left, rect.y(),
          static_cast<int>(rings.back() + kMarkerHead / 2.0f) + 1, kMarkerHead);
      hints_->SetColor(theme_.OnSurface(0.3f), 0);
      hints_->SetRings(std::move(rings));
      const gfx::Rect old = hints_->bounds();
      hints_->SetBoundsRect(hints);
      if (!hints_were_shown || motion::ReduceMotion()) {
        hints_motion_.SetTransform(hints_->layer(), LayerPose());
      } else if (old.origin() != hints.origin()) {
        hints_motion_.ShiftAndAnimate(
            hints_->layer(), old.origin() - hints.origin(), LayerPose(), fade);
      }
      hints_motion_.AnimateOpacity(hints_->layer(), 1, fade);
    }
  }
  marker_->SetHead(head);
  marker_->SetColor(theme_.accent, kMarkerThickness / 2.0f);
  const MotionCurve curve = MotionCurve::EaseOut(motion::kDropMarkerDuration);
  if (!was_shown || motion::ReduceMotion()) {
    marker_->SetBoundsRect(rect);
    marker_motion_.SetTransform(marker_->layer(), LayerPose());
    marker_motion_.AnimateOpacity(marker_->layer(), 1, curve);
    return;
  }
  const gfx::Rect old = marker_->bounds();
  marker_->SetBoundsRect(rect);
  marker_motion_.ShiftAndAnimate(marker_->layer(),
                                 old.origin() - rect.origin(), LayerPose(),
                                 curve);
  marker_motion_.AnimateOpacity(marker_->layer(), 1, curve);
}

bool TabListView::StepLoadingAnimations(base::TimeDelta elapsed) {
  bool any = false;
  for (TabCell* cell : cells_) {
    any |= cell->StepLoadingAnimation(elapsed);
  }
  return any;
}

gfx::Size TabListView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  if (horizontal()) {
    return gfx::Size(ContentWidth(), PreferredHeightFor(cells_.size()));
  }
  return gfx::Size(0, PreferredHeightFor(cells_.size()));
}

void TabListView::Layout(PassKey) {
  // 竖排列表跟随滚动视口的宽度，高度按标签数量撑开。ScrollView 默认沿用内容
  // 的旧尺寸，只会把图层扩大到视口；缩窄侧栏时必须在这里主动缩小内容。
  if (style_ == Style::kList && parent()) {
    SetSize(gfx::Size(
        parent()->width(),
        std::max(parent()->height(), PreferredHeightFor(cells_.size()))));
  }

  if (pinned()) {
    FillGridEntries();
  }
  for (size_t i = 0; i < row_views_.size(); ++i) {
    row_views_[i]->SetBoundsRect(RowRect(i));
  }

  // 宽度变了：直接放到新的位置，不做动画。
  for (size_t i = 0; i < cells_.size(); ++i) {
    cells_[i]->SetBoundsRect(SlotRect(i));
  }
  if (highlight_shown_ && highlight_handle_) {
    for (size_t i = 0; i < cells_.size(); ++i) {
      if (cells_[i]->handle() == *highlight_handle_) {
        highlight_->SetBoundsRect(HighlightRect(i));
      }
    }
  }
  PlaceTrays(Change::kNone);
  PlaceFolderTrays(Change::kNone);
  if (style_ == Style::kList) {
    PlaceGuides(Change::kNone);
  }
}

void TabListView::OnThemeChanged() {
  views::View::OnThemeChanged();
  SchedulePaint();
}

void TabListView::OnMouseMoved(const ui::MouseEvent& event) {
  // 背景收到移动事件时，鼠标在标签之间；列表末尾的空白不算。
  gfx::Rect occupied = GetLocalBounds();
  if (horizontal()) {
    occupied.set_width(std::min(width(), ContentWidth()));
  } else {
    occupied.set_height(std::min(height(), PreferredHeightFor(cells_.size())));
  }
  delegate_->TabListGapHovered(occupied.Contains(event.location()));
}

void TabListView::OnMouseExited(const ui::MouseEvent& event) {
  delegate_->TabListGapHovered(false);
}

void TabListView::Animate(TabCell* cell,
                          const gfx::Rect& target,
                          Change change) {
  const gfx::Rect old = cell->bounds();
  cell->SetBoundsRect(target);
  if (old.origin() == target.origin()) {
    return;
  }
  if (change == Change::kNone || motion::ReduceMotion()) {
    cell->motion().SetTransform(cell->layer(), LayerPose());
    return;
  }
  cell->motion().ShiftAndAnimate(cell->layer(), old.origin() - target.origin(),
                                 LayerPose(), CurveFor(change));
}

void TabListView::AnimateInsert(TabCell* cell, Change change, int ordinal) {
  const bool fold = change == Change::kFold;
  const MotionCurve fade =
      fold || change == Change::kEdit
          ? MotionCurve::EaseInOut(motion::kFolderRevealFade)
          : MotionCurve::EaseOut(motion::kTabInsertFade);
  cell->motion().AnimateOpacityFrom(cell->layer(), 0, 1, fade);
  // 回到整棵树：别的行在原处淡入。
  if (motion::ReduceMotion() || change == Change::kEdit) {
    return;
  }
  LayerPose from;
  gfx::PointF origin;
  // 顶部标签行的固定区：撑开的标签同普通标签，图标同图块。
  const bool top_row =
      style_ == Style::kTop || (style_ == Style::kTopPins && !cell->is_tile());
  if (style_ == Style::kList && fold) {
    // 展开文件夹：里面的行从上方落下来。
    from.offset =
        gfx::Vector2dF(0, -(motion::kFolderRevealOffset +
                            kRevealStep * std::min(ordinal, kRevealLimit)));
  } else if (style_ == Style::kList) {
    from.offset = gfx::Vector2dF(-motion::kTabInsertOffset, 0);
  } else if (top_row) {
    // 顶部标签以左边缘为锚点缩放出现。
    from.scale = motion::kTopTabInsertScale;
    origin = gfx::PointF(0, cell->height() / 2.0f);
  } else {
    from.scale = motion::kPinnedInsertScale;
    origin = gfx::PointF(cell->width() / 2.0f, cell->height() / 2.0f);
  }
  cell->motion().AnimateTransformFrom(
      cell->layer(), from, LayerPose(),
      fold ? CurveFor(change) : MotionCurve::Of(motion::kTabInsert), origin);
}

void TabListView::AnimateRemove(TabCell* cell, Change change) {
  cell->SetLeaving();
  base::OnceClosure done =
      base::BindOnce(&TabListView::RemoveLeavingCell,
                     weak_factory_.GetWeakPtr(), cell->GetWeakPtr());
  // 聚焦一组：别的行在原处淡出。
  if (change == Change::kEdit) {
    cell->motion().AnimateOpacity(cell->layer(), 0,
                                  MotionCurve::EaseOut(motion::kFolderHideFade),
                                  std::move(done));
    return;
  }
  if (!motion::ReduceMotion()) {
    LayerPose to;
    to.scale = motion::kTabCloseScale;
    gfx::PointF origin(cell->width() / 2.0f, cell->height() / 2.0f);
    if (style_ == Style::kTop ||
        (style_ == Style::kTopPins && !cell->is_tile())) {
      // 顶部普通标签向左收缩；顶部固定标签缩回 kPinnedInsertScale。
      origin = gfx::PointF(0, cell->height() / 2.0f);
    } else if (style_ == Style::kTopPinned || style_ == Style::kTopPins) {
      to.scale = motion::kPinnedInsertScale;
    }
    cell->motion().AnimateTransform(cell->layer(), to,
                                    MotionCurve::Of(motion::kTabClose), origin);
  }
  cell->motion().AnimateOpacity(
      cell->layer(), 0,
      MotionCurve::EaseOut(change == Change::kFold ? motion::kFolderHideFade
                                                   : motion::kTabCloseFade),
      std::move(done));
}

void TabListView::RemoveLeavingCell(base::WeakPtr<TabCell> cell) {
  // 确认它还是这里的子视图再删。
  if (!cell) {
    return;
  }
  for (views::View* child : children()) {
    if (child == cell.get()) {
      RemoveChildViewT(cell.get());
      return;
    }
  }
}

void TabListView::PlaceHighlight(Change change,
                                 std::optional<gfx::Rect> from) {
  std::optional<size_t> selected;
  for (size_t i = 0; i < cells_.size(); ++i) {
    if (cells_[i]->selected()) {
      selected = i;
      break;
    }
  }
  if (!selected) {
    if (highlight_shown_) {
      highlight_motion_.AnimateOpacity(highlight_->layer(), 0,
                                       motion::Hover());
    }
    highlight_shown_ = false;
    highlight_handle_.reset();
    return;
  }

  const bool dark = theme_.is_dark;
  const float opacity =
      pinned() ? (dark ? 0.2f : 0.14f) : (dark ? 0.16f : 0.12f);
  highlight_->SetColor(WithOpacity(theme_.accent, opacity), radius::kRow);

  const gfx::Rect target = HighlightRect(*selected);
  const tabs::TabHandle handle = cells_[*selected]->handle();
  ui::Layer* layer = highlight_->layer();
  if (!highlight_shown_) {
    highlight_->SetBoundsRect(target);
    if (from && !motion::ReduceMotion()) {
      // 从另一组滑过来（大小不同：只移位置）。
      LayerPose start;
      start.offset = gfx::Vector2dF(from->x() - target.x(),
                                    from->y() - target.y());
      highlight_motion_.AnimateTransformFrom(
          layer, start, LayerPose(), MotionCurve::Of(motion::kSelection));
      highlight_motion_.AnimateOpacity(layer, 1, motion::Hover());
    } else {
      highlight_motion_.SetTransform(layer, LayerPose());
      highlight_motion_.AnimateOpacity(layer, 1, motion::Hover());
    }
  } else {
    const gfx::Rect old = highlight_->bounds();
    highlight_->SetBoundsRect(target);
    if (old.origin() != target.origin()) {
      if (motion::ReduceMotion()) {
        highlight_motion_.SetTransform(layer, LayerPose());
      } else {
        // 切换活动标签时统一使用选中曲线滑动；同一标签随列表挪动时跟随本次变更曲线。
        const MotionCurve curve =
            highlight_handle_ != handle ? MotionCurve::Of(motion::kSelection)
                                        : CurveFor(change);
        highlight_motion_.ShiftAndAnimate(layer, old.origin() - target.origin(),
                                          LayerPose(), curve);
      }
    }
    if (highlight_motion_.IsAnimatingOpacity() ||
        layer->GetTargetOpacity() < 1) {
      highlight_motion_.AnimateOpacity(layer, 1, motion::Hover());
    }
  }
  highlight_shown_ = true;
  highlight_handle_ = handle;
}

MotionCurve TabListView::CurveFor(Change change) const {
  switch (change) {
    case Change::kClose:
      return MotionCurve::Of(motion::kTabClose);
    case Change::kSelect:
      return MotionCurve::Of(motion::kSelection);
    case Change::kFold:
      return MotionCurve::Of(fold_grew_ ? motion::kFolderOpen
                                        : motion::kFolderClose);
    case Change::kEdit:
      return MotionCurve::Of(motion::kListEdit);
    case Change::kInsert:
    case Change::kNone:
      return MotionCurve::Of(motion::kTabInsert);
  }
}

void TabListView::SyncRows(std::vector<TabListRow*>* inserted) {
  std::map<std::string, TabListRow*> old;
  for (TabListRow* row : row_views_) {
    old[row->data().id] = row;
  }
  rows_ = pending_rows_;
  std::vector<raw_ptr<TabListRow, VectorExperimental>> next;
  for (const TabListRowData& data : rows_) {
    TabListRow* row = nullptr;
    if (auto it = old.find(data.id); it != old.end()) {
      row = it->second;
      old.erase(it);
    } else {
      row = AddChildView(std::make_unique<TabListRow>(delegate_));
      row->SetGroup(focus_group_);
      inserted->push_back(row);
    }
    row->SetData(data, theme_);
    row->SetDropTarget(!drop_row_.empty() && data.id == drop_row_);
    next.push_back(row);
  }
  for (auto& [id, row] : old) {
    row->SetLeaving();
    row->motion().AnimateOpacity(
        row->layer(), 0, MotionCurve::EaseOut(motion::kFolderHideFade),
        base::BindOnce(&TabListView::RemoveLeavingRow,
                       weak_factory_.GetWeakPtr(), row->GetWeakPtr()));
  }
  row_views_ = std::move(next);
}

void TabListView::ComputeListGeometry() {
  tab_y_.assign(cells_.size(), 0);
  row_y_.assign(rows_.size(), 0);
  entries_.clear();
  int y = kListPadding;
  size_t next_row = 0;
  for (size_t i = 0; i <= cells_.size(); ++i) {
    while (next_row < rows_.size() &&
           (rows_[next_row].before_tab <= i || i == cells_.size())) {
      const TabListRowData& row = rows_[next_row];
      const int height = TabListRow::HeightFor(row.kind);
      const bool nested = row.kind == TabListRowData::Kind::kFolder ||
                          row.kind == TabListRowData::Kind::kEmpty;
      row_y_[next_row] = y;
      entries_.push_back({true, next_row, nested ? row.depth : 0, y, height});
      y += height + kListSpacing;
      ++next_row;
    }
    if (i < cells_.size()) {
      tab_y_[i] = y;
      entries_.push_back({false, i, i < depths_.size() ? depths_[i] : 0, y,
                          metrics::kTabRowHeight});
      y += metrics::kTabRowHeight + kListSpacing;
    }
  }
  content_height_ = entries_.empty() ? 0 : y - kListSpacing + kListPadding;
}

gfx::Rect TabListView::RowRect(size_t index) const {
  if (pinned()) {
    return index < row_slot_.size()
               ? GridSlotRect(row_slot_[index], width(), /*tile=*/true)
               : gfx::Rect();
  }
  if (style_ == Style::kTopPins) {
    const int y = std::max(0, (height() - metrics::kTopTabHeight) / 2);
    return index < row_x_.size() ? gfx::Rect(row_x_[index], y, row_w_[index],
                                             metrics::kTopTabHeight)
                                 : gfx::Rect();
  }
  const TabListRowData& row = rows_[index];
  const bool nested = row.kind == TabListRowData::Kind::kFolder ||
                      row.kind == TabListRowData::Kind::kEmpty;
  const int inset = nested ? TabListRow::IndentFor(row.depth) : 0;
  return gfx::Rect(inset, row_y_[index], std::max(0, width() - inset),
                   TabListRow::HeightFor(row.kind));
}

void TabListView::PlaceRows(Change change,
                            const std::vector<TabListRow*>& inserted) {
  int ordinal = 0;
  for (size_t i = 0; i < row_views_.size(); ++i) {
    TabListRow* row = row_views_[i];
    const gfx::Rect target = RowRect(i);
    if (std::ranges::find(inserted, row) != inserted.end()) {
      row->SetBoundsRect(target);
      if (change == Change::kNone) {
        continue;
      }
      row->motion().AnimateOpacityFrom(
          row->layer(), 0, 1,
          change == Change::kFold || change == Change::kEdit
              ? MotionCurve::EaseInOut(motion::kFolderRevealFade)
              : MotionCurve::EaseOut(motion::kTabInsertFade));
      if (!motion::ReduceMotion() &&
          rows_[i].kind == TabListRowData::Kind::kHoist) {
        // 聚焦一组的面包屑：从 0.98 放大、往下落 8 淡入（folderOpen）。
        LayerPose from;
        from.offset = gfx::Vector2dF(0, -motion::kFolderRevealOffset);
        from.scale = kHoistInsertScale;
        row->motion().AnimateTransformFrom(
            row->layer(), from, LayerPose(),
            MotionCurve::Of(motion::kFolderOpen),
            gfx::PointF(row->width() / 2.0f, row->height() / 2.0f));
      } else if (change == Change::kEdit) {
        // 回到整棵树：别的行在原处淡入。
      } else if (!motion::ReduceMotion() && style_ == Style::kTopPins) {
        // 新的胶囊同顶部的标签：从左边放大。
        LayerPose from;
        from.scale = motion::kTopTabInsertScale;
        row->motion().AnimateTransformFrom(
            row->layer(), from, LayerPose(),
            MotionCurve::Of(motion::kTabInsert),
            gfx::PointF(0, row->height() / 2.0f));
      } else if (!motion::ReduceMotion() && pinned()) {
        // 新的图块文件夹同常用的图块：从中间放大。
        LayerPose from;
        from.scale = motion::kPinnedInsertScale;
        row->motion().AnimateTransformFrom(
            row->layer(), from, LayerPose(),
            MotionCurve::Of(motion::kTabInsert),
            gfx::PointF(row->width() / 2.0f, row->height() / 2.0f));
      } else if (!motion::ReduceMotion()) {
        LayerPose from;
        from.offset =
            change == Change::kFold
                ? gfx::Vector2dF(
                      0, -(motion::kFolderRevealOffset +
                           kRevealStep * std::min(ordinal, kRevealLimit)))
                : gfx::Vector2dF(-motion::kTabInsertOffset, 0);
        row->motion().AnimateTransformFrom(row->layer(), from, LayerPose(),
                                           CurveFor(change));
      }
      ++ordinal;
      continue;
    }
    const gfx::Rect old = row->bounds();
    row->SetBoundsRect(target);
    if (old.origin() == target.origin()) {
      continue;
    }
    if (change == Change::kNone || motion::ReduceMotion()) {
      row->motion().SetTransform(row->layer(), LayerPose());
    } else {
      row->motion().ShiftAndAnimate(row->layer(),
                                    old.origin() - target.origin(), LayerPose(),
                                    CurveFor(change));
    }
  }
}

int TabListView::TreeStep(int width) const {
  const int budget = width - kTreeRowChrome - kTreeMinTitle;
  if (tree_depth_ <= 0) {
    return kTreeMaxStep;
  }
  const int levels = std::min(tree_depth_, TreeCap(width));
  return std::clamp(budget / std::max(levels, 1), kTreeMinStep, kTreeMaxStep);
}

int TabListView::TreeCap(int width) const {
  const int budget = width - kTreeRowChrome - kTreeMinTitle;
  return std::max(1, budget / kTreeMinStep);
}

int TabListView::TreeInset(size_t index, int width) const {
  if (index >= trees_.size() || !trees_[index]) {
    return 0;
  }
  return std::min(trees_[index]->depth, TreeCap(width)) * TreeStep(width);
}

void TabListView::PlaceGuides(Change change) {
  // 文件夹后面紧跟着的、比它深的行（标签、子文件夹、落点）都在它的线旁边。
  struct Guide {
    gfx::Rect rect;
    // 整条用强调色（当前标签在这个文件夹下面）。
    bool active = false;
    // 树：从上面到通往当前标签的那一行的中线，这一段用强调色。
    int path = 0;
    bool tree = false;
  };
  std::map<std::string, Guide> wanted;
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    if (!entry.row ||
        rows_[entry.index].kind != TabListRowData::Kind::kFolder ||
        entry.depth >= TabListRow::kMaxIndentDepth) {
      continue;
    }
    size_t last = i;
    while (last + 1 < entries_.size() &&
           entries_[last + 1].depth > entry.depth) {
      ++last;
    }
    if (last == i) {
      continue;
    }
    const int top = entry.y + entry.height + 1;
    const int bottom = entries_[last].y + entries_[last].height;
    wanted[rows_[entry.index].id] = {
        gfx::Rect(TabListRow::IndentFor(entry.depth) + kGuideX, top, 1,
                  std::max(0, bottom - top)),
        rows_[entry.index].active};
  }
  // 树里展开的父标签：线从它下面到这一枝最后一行（折着的子孙没有在列表里）。
  std::optional<size_t> selected;
  for (size_t i = 0; i < cells_.size() && i < trees_.size(); ++i) {
    if (cells_[i]->selected()) {
      selected = i;
    }
  }
  const int cap = TreeCap(width());
  const int step = TreeStep(width());
  for (size_t i = 0; i < cells_.size() && i < trees_.size(); ++i) {
    if (!trees_[i] || !trees_[i]->parent || trees_[i]->collapsed) {
      continue;
    }
    const int depth = trees_[i]->depth;
    size_t last = i;
    while (last + 1 < trees_.size() && trees_[last + 1] &&
           trees_[last + 1]->depth > depth) {
      ++last;
    }
    // 缩到头了：子标签和它在同一个缩进上，画不出线。
    if (last == i || std::min(depth + 1, cap) <= std::min(depth, cap)) {
      continue;
    }
    const int x =
        TreeInset(i, width()) + std::min(kGuideX, step - kTreeGuideGap);
    const int top = tab_y_[i] + metrics::kTabRowHeight + 1;
    const int bottom = tab_y_[last] + metrics::kTabRowHeight;
    Guide guide{gfx::Rect(x, top, 1, std::max(0, bottom - top))};
    // 当前标签在这一枝里：从上面到挂在它下面、通往当前标签的那一行。
    guide.path = TreePathLength(i, selected, top);
    guide.tree = true;
    wanted[TreeGuideId(cells_[i]->handle())] = guide;
  }
  for (auto it = guides_.begin(); it != guides_.end();) {
    if (wanted.contains(it->first)) {
      ++it;
      continue;
    }
    TabListFill* guide = it->second;
    it = guides_.erase(it);
    FadeOutAndRemove(guide);
  }
  // 跟着这次改动的曲线动（换了标签时当前路径同选中的高亮一起伸缩）。
  std::optional<MotionCurve> curve;
  if (change != Change::kNone && !motion::ReduceMotion()) {
    curve = CurveFor(change);
  }
  for (const auto& [id, line] : wanted) {
    const gfx::Rect& rect = line.rect;
    TabListFill* guide = nullptr;
    if (auto it = guides_.find(id); it != guides_.end()) {
      guide = it->second;
      const gfx::Rect old = guide->bounds();
      guide->SetBoundsRect(rect);
      if (old.origin() != rect.origin() && curve) {
        guide->motion().ShiftAndAnimate(
            guide->layer(), old.origin() - rect.origin(), LayerPose(), *curve);
      }
      // 多了子标签：线同曲线往下长。
      if (old.height() != rect.height()) {
        guide->Grow(old.height(), rect.height() > old.height()
                                      ? curve
                                      : std::optional<MotionCurve>());
      }
    } else {
      guide = AddChildView(std::make_unique<TabListFill>());
      guides_[id] = guide;
      guide->SetBoundsRect(rect);
      if (change == Change::kNone) {
        guide->motion().SetOpacity(guide->layer(), 1);
      } else {
        guide->motion().AnimateOpacity(guide->layer(), 1, motion::Hover());
        // 第一个子标签来了、展开：线从父标签往下长。
        if (line.tree) {
          guide->Grow(0, curve);
        }
      }
    }
    guide->SetColor(line.active ? WithOpacity(theme_.accent, 0.38f)
                                : theme_.OnSurface(0.1f),
                    0.5f);
    guide->SetOverlay(
        WithOpacity(theme_.accent, 0.38f), line.path,
        change == Change::kSelect && curve
            ? std::optional<MotionCurve>(MotionCurve::Of(motion::kSelection))
            : curve);
  }
  PlaceGuideHover();
}

int TabListView::TreePathLength(size_t parent,
                                std::optional<size_t> target,
                                int top) const {
  if (!target || *target <= parent || *target >= cells_.size() ||
      *target >= trees_.size()) {
    return 0;
  }
  const tabs::TabHandle handle = cells_[parent]->handle();
  size_t child = *target;
  // 顺着父标签往上找到挂在它下面的那一行。
  for (int guard = 0; guard < 64 && trees_[child]; ++guard) {
    if (trees_[child]->up == handle) {
      return tab_y_[child] + metrics::kTabRowHeight / 2 - top;
    }
    const std::optional<size_t> up = IndexOf(trees_[child]->up);
    if (!up || *up >= child || *up < parent) {
      return 0;
    }
    child = *up;
  }
  return 0;
}

void TabListView::SetHoveredTab(tabs::TabHandle handle) {
  if (hovered_tab_ == handle) {
    return;
  }
  hovered_tab_ = handle;
  PlaceGuideHover();
}

void TabListView::PlaceGuideHover() {
  const std::optional<size_t> hovered =
      hovered_tab_ != tabs::TabHandle() ? IndexOf(hovered_tab_) : std::nullopt;
  for (size_t i = 0; i < cells_.size() && i < trees_.size(); ++i) {
    auto it = guides_.find(TreeGuideId(cells_[i]->handle()));
    if (it == guides_.end()) {
      continue;
    }
    it->second->SetHover(theme_.OnSurface(kTreeHoverOpacity),
                         TreePathLength(i, hovered, it->second->y()));
  }
}

void TabListView::FadeOutAndRemove(TabListFill* fill) {
  fill->motion().AnimateOpacity(
      fill->layer(), 0, motion::Hover(),
      base::BindOnce(
          [](base::WeakPtr<TabListView> view, base::WeakPtr<TabListFill> fill) {
            // 动画的回调里不能删掉自己的 LayerMotion：回到消息循环再删。
            // 窗口关的时候它可能已经跟着列表没了：用弱指针。
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(
                               [](base::WeakPtr<TabListView> view,
                                  base::WeakPtr<TabListFill> fill) {
                                 if (!view || !fill) {
                                   return;
                                 }
                                 for (views::View* child : view->children()) {
                                   if (child == fill.get()) {
                                     view->RemoveChildViewT(fill.get());
                                     return;
                                   }
                                 }
                               },
                               view, fill));
          },
          weak_factory_.GetWeakPtr(), fill->GetWeakPtr()));
}

void TabListView::RemoveLeavingRow(base::WeakPtr<TabListRow> row) {
  // 动画的回调里不能删掉自己的 LayerMotion：回到消息循环再删。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<TabListView> view, base::WeakPtr<TabListRow> row) {
            if (!view || !row) {
              return;
            }
            for (views::View* child : view->children()) {
              if (child == row.get()) {
                view->RemoveChildViewT(row.get());
                return;
              }
            }
          },
          weak_factory_.GetWeakPtr(), std::move(row)));
}

BEGIN_METADATA(TabListView)
END_METADATA

}  // namespace starry
