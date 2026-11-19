// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/top_tab_strip.h"

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/tab_organizer/tab_grouper.h"
#include "starry/browser/ui/archive/organize_page.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/media/media_capsule.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "starry/browser/ui/toolbar/toolbar_parts.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/views/controls/scroll_view.h"

namespace starry {

namespace {

constexpr int kSpacing = 4;
constexpr int kNewTabButtonWidth = 28;
// 拖动时鼠标离开标签行这么远以内还算在标签行上。
constexpr int kDropSlop = 24;

MotionCurve CurveFor(TabListView::Change change) {
  switch (change) {
    case TabListView::Change::kClose:
      return MotionCurve::Of(motion::kTabClose);
    case TabListView::Change::kSelect:
      return MotionCurve::Of(motion::kSelection);
    case TabListView::Change::kFold:
      return MotionCurve::Of(motion::kFolderOpen);
    case TabListView::Change::kEdit:
      return MotionCurve::Of(motion::kListEdit);
    case TabListView::Change::kInsert:
    case TabListView::Change::kNone:
      return MotionCurve::Of(motion::kTabInsert);
  }
}

}  // namespace

TopTabStrip::TopTabStrip(BrowserWindowInterface* browser)
    : TabsView(browser,
               TabListView::Style::kTopPinned,
               TabListView::Style::kTop),
      scroll_motion_(0,
                     base::BindRepeating(&TopTabStrip::ScrollStep,
                                         base::Unretained(this))) {
  // 自己的图层：换布局时整行从上边移入、淡入。标签行的空白处可以拖动窗口。
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetWindowDraggable(this, true);
  separator_ = AddChildView(std::make_unique<ToolbarSeparator>());
  pins_separator_ = AddChildView(std::make_unique<ToolbarSeparator>());
  // 固定区变宽变窄时后面的东西滑过去：它们各有自己的图层。
  for (views::View* view : std::vector<views::View*>{
           separator_.get(), pins_separator_.get(), pins(), scroll()}) {
    view->SetPaintToLayer();
    view->layer()->SetFillsBoundsOpaquely(false);
  }
  new_tab_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(
          [](BrowserWindowInterface* browser) {
            if (StarryBrowserView* view = StarryBrowserView::From(browser)) {
              view->OpenCommandBar(CommandBarView::Mode::kNewTab);
            }
          },
          browser),
      icons::kPlus, L("tabs.newTab.help"), metrics::kToolButtonSize, 12));
  assistant_ = AddChildView(std::make_unique<AssistantButton>(browser));
  // 媒体岛收起后的胶囊：✦ 左边，标签只平分它左边的宽度。
  media_ = AddChildView(std::make_unique<MediaCapsule>(browser));
  media_->SetPresenceChangedCallback(
      base::BindRepeating([](TopTabStrip* strip) { strip->InvalidateLayout(); },
                          base::Unretained(this)));
  organize_ = AddChildView(std::make_unique<PillButton>(
      base::BindRepeating(&OpenOrganize, browser), std::u16string(),
      ButtonVariant::kSecondary));
  organize_->SetHeight(24);
  organize_->SetIcon(&icons::kSparkle);
  organize_->SetTooltipText(L("organize.pill.help"));
  organize_->SetVisible(false);
  Sync(TabListView::Change::kNone);
}

TopTabStrip::~TopTabStrip() = default;

views::View* TopTabStrip::organize_button_for_testing() {
  return organize_;
}

void TopTabStrip::SetTheme(const Theme& theme) {
  separator_->SetTheme(theme);
  pins_separator_->SetTheme(theme);
  new_tab_->SetTheme(theme);
  assistant_->SetTheme(theme);
  media_->SetTheme(theme);
  organize_->SetTheme(theme);
  TabsView::SetTheme(theme);
}

void TopTabStrip::SetInsets(int leading, int trailing) {
  if (leading_ == leading && trailing_ == trailing) {
    return;
  }
  leading_ = leading;
  trailing_ = trailing;
  InvalidateLayout();
}

// static
int TopTabStrip::TabWidth(int available, int count) {
  if (count <= 0) {
    return metrics::kTopTabMaxWidth;
  }
  const int share = (available - kNewTabButtonWidth) / count - kSpacing;
  return std::clamp(share, metrics::kTopTabMinWidth, metrics::kTopTabMaxWidth);
}

int TopTabStrip::PinsStart() const {
  int x = leading_;
  if (pinned()->slot_count() > 0) {
    const int count = static_cast<int>(pinned()->slot_count());
    x += count * (metrics::kTopPinnedTabWidth + kSpacing) - kSpacing;
    x += kSpacing + ToolbarSeparator::kWidth + kSpacing;
  }
  return x;
}

int TopTabStrip::TrailingEdge() const {
  int right = width() - trailing_;
  if (assistant_->GetVisible()) {
    right -= metrics::kToolButtonSize + kSpacing;
  }
  if (media_->GetVisible()) {
    right -= MediaCapsule::kWidth + kSpacing;
  }
  if (organize_->GetVisible()) {
    right -= organize_->GetPreferredSize().width() + kSpacing;
  }
  return right;
}

gfx::Rect TopTabStrip::FlexibleArea() const {
  int x = PinsStart();
  if (const int pins_width = pins()->ContentWidth(); pins_width > 0) {
    x += pins_width + kSpacing + ToolbarSeparator::kWidth + kSpacing;
  }
  const int right = TrailingEdge();
  return gfx::Rect(x, 0, std::max(0, right - x), height());
}

int TopTabStrip::PinStripLimit() const {
  // 固定区最多占常用图块后面那一段的四成（不然普通标签会被挤没）。
  if (width() == 0) {
    return 0;
  }
  return std::max(metrics::kTopPinnedTabWidth,
                  (TrailingEdge() - PinsStart()) * 2 / 5);
}

gfx::Rect TopTabStrip::MediaCapsuleTargetBounds() const {
  int right = width() - trailing_;
  if (assistant_->GetVisible()) {
    right -= metrics::kToolButtonSize + kSpacing;
  }
  return gfx::Rect(right - MediaCapsule::kWidth,
                   (height() - MediaCapsule::kHeight) / 2, MediaCapsule::kWidth,
                   MediaCapsule::kHeight);
}

int TopTabStrip::ComputeItemWidth() const {
  // 分屏的一组标签外面有托盘，头尾各多占 kSplitTrayInset。
  std::set<split_tabs::SplitTabId> groups;
  for (int i = model()->IndexOfFirstNonPinnedTab(); i < model()->count(); ++i) {
    if (const std::optional<split_tabs::SplitTabId> split =
            model()->GetTabAtIndex(i)->GetSplit()) {
      groups.insert(*split);
    }
  }
  const int trays = static_cast<int>(groups.size()) * kSplitTrayInset * 2;
  return TabWidth(FlexibleArea().width() - trays,
                  static_cast<int>(list()->count()));
}

void TopTabStrip::OnSynced(TabListView::Change change) {
  // 「✦ 整理 N」：能整理的普通标签多了才有。
  const size_t organizable = tab_organizer::OrganizableTabs(browser()).size();
  const bool organize =
      tab_organizer::OrganizeHintWanted(browser()->GetProfile(), organizable);
  if (organize) {
    organize_->SetTitle(
        L("organize.pill.count", {{"count", static_cast<int>(organizable)}}));
  }
  organize_->SetVisible(organize);
  synced_limit_ = PinStripLimit();
  if (width() == 0) {
    return;
  }
  // 标签多了、少了：平分的宽度跟着变，其余的用同一条曲线挤开、合拢。
  item_width_ = ComputeItemWidth();
  list()->SetItemWidth(item_width_, change);
  // 固定区变宽变窄、标签增删：后面的细线、普通标签和「新标签页」按钮下一次
  // 排版时从原来的位置滑过去。
  if (change != TabListView::Change::kNone) {
    pending_change_ = change;
  }
}

void TopTabStrip::Slide(views::View* view,
                        const gfx::Rect& target,
                        LayerMotion& motion) {
  const gfx::Rect old = view->bounds();
  const bool was_visible = view->GetVisible();
  view->SetBoundsRect(target);
  if (pending_change_ == TabListView::Change::kNone || !was_visible ||
      old.IsEmpty() || old.origin() == target.origin() || !view->layer() ||
      motion::ReduceMotion()) {
    return;
  }
  motion.ShiftAndAnimate(view->layer(), old.origin() - target.origin(),
                         LayerPose(), CurveFor(pending_change_));
}

void TopTabStrip::Layout(PassKey) {
  const int h = height();
  const int button = metrics::kToolButtonSize;
  int right = width() - trailing_;
  if (assistant_->GetVisible()) {
    assistant_->SetBounds(right - button, (h - button) / 2, button, button);
  }
  if (media_->GetVisible()) {
    media_->SetBoundsRect(MediaCapsuleTargetBounds());
  }
  if (organize_->GetVisible()) {
    const gfx::Size size = organize_->GetPreferredSize();
    const int edge = TrailingEdge() + kSpacing + size.width();
    organize_->SetBounds(edge - size.width(), (h - size.height()) / 2,
                         size.width(), size.height());
  }
  int x = leading_;
  const int pinned_count = static_cast<int>(pinned()->slot_count());
  if (pinned_count > 0) {
    const int w =
        pinned_count * (metrics::kTopPinnedTabWidth + kSpacing) - kSpacing;
    pinned()->SetBounds(x, 0, w, h);
    x += w + kSpacing;
    Slide(separator_, gfx::Rect(x, 0, ToolbarSeparator::kWidth, h),
          separator_motion_);
    separator_->SetVisible(true);
    x += ToolbarSeparator::kWidth + kSpacing;
  } else {
    pinned()->SetBounds(x, 0, 0, h);
    separator_->SetVisible(false);
  }
  // 固定区：散着的固定标签、文件夹的胶囊，后面一条细线。
  if (const int pins_width = pins()->ContentWidth(); pins_width > 0) {
    Slide(pins(), gfx::Rect(x, 0, pins_width, h), pins_motion_);
    x += pins_width + kSpacing;
    Slide(pins_separator_, gfx::Rect(x, 0, ToolbarSeparator::kWidth, h),
          pins_separator_motion_);
    pins_separator_->SetVisible(true);
  } else {
    pins()->SetBounds(x, 0, 0, h);
    pins_separator_->SetVisible(false);
  }

  const gfx::Rect area = FlexibleArea();
  const int item_width = ComputeItemWidth();
  if (item_width != item_width_) {
    // 窗口变宽变窄：直接放过去，不做动画。
    item_width_ = item_width;
    list()->SetItemWidth(item_width_, TabListView::Change::kNone);
  }
  const int count = static_cast<int>(list()->count());
  const int room = std::max(0, area.width() - kNewTabButtonWidth - kSpacing);
  const int list_width = count > 0 ? std::min(list()->ContentWidth(), room) : 0;
  Slide(scroll(), gfx::Rect(area.x(), 0, list_width, h), scroll_shift_motion_);
  scroll()->SetVisible(count > 0);
  const int new_tab_x = area.x() + (count > 0 ? list_width + kSpacing : 0);
  Slide(new_tab_, gfx::Rect(new_tab_x, (h - button) / 2, button, button),
        new_tab_motion_);
  pending_change_ = TabListView::Change::kNone;
}

void TopTabStrip::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  // 窗口变宽变窄：固定区够不够放可能变了（胶囊收成图标、收进「…」）。拖动
  // 窗口边缘时每一点都会到这里，只有放法变了才重新刷新。
  const int limit = PinStripLimit();
  if (limit == synced_limit_) {
    return;
  }
  auto fit = [this](int max) {
    if (max <= 0 || strip_full_width() <= max) {
      return 0;
    }
    return strip_compact_width() <= max ? 1 : 2;
  };
  const int before = fit(synced_limit_);
  const int after = fit(limit);
  if (before != after || after == 2) {
    Sync(TabListView::Change::kNone);
    return;
  }
  synced_limit_ = limit;
}

std::optional<TabsView::DropTarget> TopTabStrip::DropTargetAt(
    const gfx::Point& point) const {
  // 文件夹的卡片开着：拖到卡片上就是放进卡片里的列表。
  if (std::optional<DropTarget> in_card; CardDropTargetAt(point, &in_card)) {
    return in_card;
  }
  if (point.y() > height() + kDropSlop) {
    return std::nullopt;
  }
  // 拖着固定区的文件夹：只能放在固定区里；拖着图块文件夹：只能放在常用图块
  // 里。
  const bool folder =
      is_dragging() && !is_dragging_tab() && !is_dragging_tile();
  if (pinned()->slot_count() > 0 && point.x() < separator_->bounds().right()) {
    if (folder) {
      return std::nullopt;
    }
    gfx::Point local = point;
    views::View::ConvertPointToTarget(this, pinned(), &local);
    return GridDropTarget(local);
  }
  if (is_dragging_tile()) {
    return std::nullopt;
  }
  if (pins_separator_->GetVisible() &&
      point.x() < pins_separator_->bounds().right()) {
    gfx::Point local = point;
    views::View::ConvertPointToTarget(this, pins(), &local);
    return StripDropTarget(local);
  }
  if (folder) {
    return std::nullopt;
  }
  const gfx::Rect list_bounds = scroll()->bounds();
  if (list()->count() > 0 && point.x() < list_bounds.right()) {
    if (point.x() < list_bounds.x()) {
      return DropTarget{false, 0};
    }
    gfx::Point local = point;
    views::View::ConvertPointToTarget(this, list(), &local);
    return DropTarget{false, list()->DropIndexAt(local)};
  }
  // 「新标签页」按钮和后面的空白：放到最后。
  return DropTarget{false, list()->count()};
}

bool TopTabStrip::IsOverContent(const gfx::Point& point) const {
  if (card() && CardBounds().Contains(point)) {
    return false;
  }
  return point.y() > height();
}

std::optional<int> TopTabStrip::PreviewEdge(views::View* preview) const {
  return std::nullopt;
}

void TopTabStrip::RevealActive() {
  const int active = model()->active_index();
  auto index = list()->IndexOf(model()->GetTabAtIndex(active)->GetHandle());
  if (!index) {
    return;
  }
  const gfx::Rect slot = list()->SlotRect(*index);
  const gfx::Rect visible = scroll()->GetVisibleRect();
  if (visible.width() <= 1) {
    return;
  }
  int x = visible.x();
  if (slot.x() < visible.x()) {
    x = slot.x();
  } else if (slot.right() > visible.right()) {
    x = slot.right() - visible.width();
  }
  x = std::clamp(x, 0, std::max(0, list()->ContentWidth() - visible.width()));
  if (x == visible.x()) {
    revealed_ = true;
    return;
  }
  // 首次显示直接滚到位，之后按选中动效平滑滚动。
  if (!revealed_ || !GetWidget()) {
    revealed_ = true;
    scroll_motion_.SetValue(x);
    scroll()->ScrollToOffset(gfx::PointF(x, 0));
    return;
  }
  scroll_motion_.SetValue(visible.x());
  scroll_motion_.AnimateTo(x, MotionCurve::Of(motion::kSelection));
}

void TopTabStrip::ScrollStep() {
  scroll()->ScrollToOffset(gfx::PointF(scroll_motion_.value(), 0));
}

void TopTabStrip::TabFolderAction(const std::string& id,
                                  std::optional<gfx::Point> menu_point) {
  // 点胶囊：落下（收起）文件夹的卡片。卡片里的文件夹行照常开合。
  if (!menu_point && ChipFor(id)) {
    ToggleCard(id);
    return;
  }
  TabsView::TabFolderAction(id, menu_point);
}

TabListRow* TopTabStrip::ChipFor(const std::string& id) const {
  for (TabListRow* row : pins()->rows()) {
    if (row->data().kind == TabListRowData::Kind::kChip &&
        row->data().id == id) {
      return row;
    }
  }
  return nullptr;
}

std::optional<TabsView::CardAnchor> TopTabStrip::CardAnchorFor(
    const std::string& id) const {
  TabListRow* chip = ChipFor(id);
  PopoverHost* host = popover_host();
  if (!chip || !host) {
    return TabsView::CardAnchorFor(id);
  }
  CardAnchor anchor;
  anchor.rect =
      views::View::ConvertRectToTarget(chip, host, chip->GetLocalBounds());
  return anchor;
}

BEGIN_METADATA(TopTabStrip)
END_METADATA

}  // namespace starry
