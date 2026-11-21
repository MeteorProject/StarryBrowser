// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tabs_view.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_user_gesture_details.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/agent/agent_activity.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/tab_organizer/organizer_memory.h"
#include "starry/browser/tab_organizer/tab_grouper.h"
#include "starry/browser/tab_organizer/tab_organizer.h"
#include "starry/browser/ui/archive/organize_page.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/split/split_overlay.h"
#include "starry/browser/ui/tabs/favorite_folders.h"
#include "starry/browser/ui/tabs/pin_folder_card.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/tabs/pinned_peek.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "starry/browser/ui/tabs/tab_list_row.h"
#include "starry/browser/ui/tabs/tab_menu.h"
#include "starry/browser/ui/tabs/tab_preview.h"
#include "starry/browser/ui/tabs/tab_tree.h"
#include "starry/browser/ui/tabs/tree_sticky_header.h"
#include "starry/common/internal_urls.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/compositor/layer.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/linear_gradient.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/view_utils.h"

namespace starry {

namespace {

// 拖着的小条：左右 10，图标和标题之间 8，高 30。外面留出阴影的位置。
constexpr int kPillHeight = 30;
constexpr int kPillMaxWidth = 240;
constexpr int kPillShadowInset = 10;
// 文件夹的名字和后面的数字之间。
constexpr int kPillTrailingGap = 6;
constexpr int kLoadingFrameMs = 30;
// 拖到列表上下边缘这么近时自动滚动：离边缘越近越快，每一步最多滚这么多。
constexpr int kAutoScrollEdge = 28;
constexpr int kAutoScrollMaxStep = 12;
constexpr int kAutoScrollFrameMs = 16;
// 侧栏的固定区空着时，标签拖到列表顶上这么高以内（「新标签页」那一行）露出
// 落点；左右可以超出列表这么多。
constexpr int kPinRevealZone = 40;
constexpr int kPinRevealMargin = 24;
// 文件夹最多往里找这么多层（设置被改坏了也不会没完没了）。
constexpr int kMaxFolderDepth = 64;
// 顶部标签行的固定区：撑开的标签（选中的散着的固定标签、挂在胶囊后面的当前
// 标签）多宽；插入线离两边的项多远。
constexpr int kTopPinWideWidth = 160;
// 顶部标签行里拖着的小条：左边最多在鼠标左边这么远，上边在鼠标下面这么远。
constexpr int kTopPillGrabX = 32;
constexpr int kTopPillDrop = 14;
constexpr int kStripLineGap = 2;
// 在图块、胶囊上按下时卡片先收起了：这么快的松开不再打开它。
constexpr base::TimeDelta kReopenGuard = base::Milliseconds(400);
// 自己落下来的卡片：鼠标在卡片和图块（胶囊）外面这么近还不算离开。
constexpr int kSprungCardSlop = 12;
// 树中拖到一枝末尾的缝隙时，每水平移动该距离至少切换一层（缩进小于该值时）。
constexpr int kTreeDragStep = 12;
// 停在树里折着的父标签上要展开它：spring_id_ 是这个前缀加上它的 TabHandle。
constexpr char kTreeSpringPrefix[] = "@tree/";
// 父标签吸顶：最多停两层；下面滚过去的行在 8 里渐渐淡掉；点一下滚到那一行离
// 顶上 2。
constexpr size_t kStickyMax = 2;
constexpr float kStickyFade = 8;
constexpr int kStickyScrollMargin = 2;

bool IsAudible(const tabs::TabData& data) {
  return data.alert_state == tabs::TabAlert::kAudioPlaying;
}

SplitOverlay* SplitOverlayFor(BrowserWindowInterface* browser) {
  StarryBrowserView* view = StarryBrowserView::From(browser);
  return view && view->has_starry_chrome() ? view->split_overlay() : nullptr;
}

}  // namespace

// 拖动时跟随鼠标的浮条：标签显示图标和标题，文件夹显示图标、名称和标签数。
class TabDragPill : public views::View {
  METADATA_HEADER(TabDragPill, views::View)

 public:
  TabDragPill() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
    SetVisible(false);
  }

  // extra：树里拖的是一整枝，它下面还有几个（写成「+N」）。
  void Show(const tabs::TabData& data, const Theme& theme, int extra = 0) {
    title_ = data.title.empty() || data.visible_url.IsAboutBlank() ||
                     IsInternalURL(data.visible_url)
                 ? (data.title.empty() || data.visible_url.IsAboutBlank()
                        ? L("tabs.untitled")
                        : data.title)
                 : data.title;
    favicon_ = data.favicon;
    folder_ = false;
    trailing_ =
        extra > 0 ? u"+" + base::NumberToString16(extra) : std::u16string();
    Present(theme);
  }

  void ShowFolder(const std::u16string& name,
                  int count,
                  std::optional<SkColor> tint,
                  const Theme& theme) {
    title_ = name;
    favicon_ = ui::ImageModel();
    folder_ = true;
    tint_ = tint;
    trailing_ = count > 0 ? base::NumberToString16(count) : std::u16string();
    Present(theme);
  }

  // 小条（不算阴影）的左上角放到 point。
  void MoveTo(const gfx::Point& point) {
    SetPosition(point - gfx::Vector2d(kPillShadowInset, kPillShadowInset));
  }

  // 松手放进了文件夹：朝文件夹图标的中心 target（上一层视图的坐标）缩小、淡出。
  void FlyTo(const gfx::Point& target) {
    if (motion::ReduceMotion()) {
      SetVisible(false);
      return;
    }
    LayerPose pose;
    pose.offset = gfx::Vector2dF(target - bounds().CenterPoint());
    pose.scale = motion::kFolderDropPillScale;
    motion_.AnimateTransform(layer(), pose, MotionCurve::Of(motion::kLift),
                             gfx::PointF(width() / 2.0f, height() / 2.0f));
    motion_.AnimateOpacity(
        layer(), 0, MotionCurve::EaseOut(motion::kFolderDropPillFade),
        base::BindOnce(&TabDragPill::Hide, weak_factory_.GetWeakPtr()));
  }

  void Hide() { SetVisible(false); }

  int pill_width() const { return pill_width_; }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF pill(kPillShadowInset, kPillShadowInset, pill_width_,
                          kPillHeight);
    cc::PaintFlags shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(theme_.surface_panel);
    shadow.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
        gfx::Vector2d(0, 4), 14,
        SkColorSetA(SK_ColorBLACK, theme_.is_dark ? 90 : 40))}));
    canvas->DrawRoundRect(pill, radius::kRow, shadow);

    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = pill;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius::kRow - 0.5f, border);

    const int size = metrics::kFaviconSize;
    const gfx::Rect icon(kPillShadowInset + 10,
                         kPillShadowInset + (kPillHeight - size) / 2, size,
                         size);
    if (folder_) {
      canvas->Save();
      canvas->Translate(icon.OffsetFromOrigin());
      gfx::PaintVectorIcon(canvas, icons::kFolder, size,
                           tint_.value_or(theme_.on_surface_variant));
      canvas->Restore();
    } else if (!favicon_.IsEmpty() && GetColorProvider()) {
      canvas->DrawImageInt(favicon_.Rasterize(GetColorProvider()), 0, 0,
                           favicon_.Size().width(), favicon_.Size().height(),
                           icon.x(), icon.y(), size, size, true);
    }
    int right = kPillShadowInset + pill_width_ - 10;
    if (!trailing_.empty()) {
      const int width = gfx::GetStringWidth(trailing_, UiFont(11));
      canvas->DrawStringRectWithFlags(
          trailing_, UiFont(11), theme_.on_surface_variant,
          gfx::Rect(right - width, kPillShadowInset, width, kPillHeight),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
      right -= width + kPillTrailingGap;
    }
    const int x = icon.right() + 8;
    canvas->DrawStringRectWithFlags(
        title_, UiFont(13), theme_.on_surface,
        gfx::Rect(x, kPillShadowInset, std::max(0, right - x), kPillHeight),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  void Present(const Theme& theme) {
    theme_ = theme;
    const int text = gfx::GetStringWidth(title_, UiFont(13));
    const int trailing =
        trailing_.empty()
            ? 0
            : kPillTrailingGap + gfx::GetStringWidth(trailing_, UiFont(11));
    pill_width_ = std::min(
        kPillMaxWidth, 10 + metrics::kFaviconSize + 8 + text + trailing + 10);
    SetSize(gfx::Size(pill_width_ + 2 * kPillShadowInset,
                      kPillHeight + 2 * kPillShadowInset));
    // 上一次可能是缩进文件夹里去的。
    motion_.SetTransform(layer(), LayerPose());
    motion_.SetOpacity(layer(), 1);
    SetVisible(true);
    SchedulePaint();
  }

  std::u16string title_;
  // 文件夹里有几个标签，写在名字后面。
  std::u16string trailing_;
  ui::ImageModel favicon_;
  bool folder_ = false;
  std::optional<SkColor> tint_;
  Theme theme_ = Theme::DarkBase();
  int pill_width_ = 0;
  LayerMotion motion_;
  base::WeakPtrFactory<TabDragPill> weak_factory_{this};
};

BEGIN_METADATA(TabDragPill)
END_METADATA

TabsView::TabsView(BrowserWindowInterface* browser,
                   TabListView::Style pinned_style,
                   TabListView::Style list_style)
    : browser_(browser), model_(browser->GetTabStripModel()) {
  pinned_ = AddChildView(std::make_unique<TabListView>(pinned_style, this));
  if (list_style == TabListView::Style::kTop) {
    // 顶部标签行：固定标签和文件夹在常用图块和普通标签之间，不跟着滚动。
    pins_ = AddChildView(
        std::make_unique<TabListView>(TabListView::Style::kTopPins, this));
  }

  // 标签有自己的图层：滚动区域也要用图层，才能把它们裁在里面（Windows、Linux
  // 上默认不用）。滚到头带回弹：侧栏列表不够长时也允许回弹，顶部标签可横向滚动时才弹。
  const bool top = list_style == TabListView::Style::kTop;
  scroll_ = AddChildView(std::make_unique<ElasticScrollView>(
      top ? ElasticScrollView::Axis::kHorizontal
          : ElasticScrollView::Axis::kVertical,
      top ? ElasticScrollView::Elasticity::kWhenScrollable
          : ElasticScrollView::Elasticity::kAlways));
  scroll_->SetBackgroundColor(std::nullopt);
  scroll_->SetDrawOverflowIndicator(false);
  if (top) {
    // 跟随标签当前的总宽度；沿用旧尺寸会在关标签、缩窄标签后留下空白的滚动范围。
    scroll_->SetUseContentsPreferredSize(true);
    // 顶部标签放不下时横着滚，普通鼠标的滚轮（只有竖直方向）也能横着滚。
    scroll_->SetVerticalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    scroll_->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kHiddenButEnabled);
    scroll_->SetTreatAllScrollEventsAsHorizontal(true);
  } else {
    scroll_->SetVerticalScrollBarMode(
        views::ScrollView::ScrollBarMode::kHiddenButEnabled);
    scroll_->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
  }
  list_ = scroll_->SetContents(std::make_unique<TabListView>(list_style, this));
  // 侧栏：树里往下滚时祖先的行停在列表顶上。
  if (!top) {
    sticky_ = AddChildView(std::make_unique<TreeStickyHeader>(
        base::BindRepeating(&TabsView::ScrollToTab, base::Unretained(this)),
        base::BindRepeating(&TabsView::PlaceSticky, base::Unretained(this))));
    scrolled_ = scroll_->AddContentsScrolledCallback(
        base::BindRepeating(&TabsView::UpdateSticky, base::Unretained(this)));
  }

  drag_pill_ = AddChildView(std::make_unique<TabDragPill>());

  pinned_tabs::InitializeProfile(browser_->GetProfile());
  // 自动整理：普通窗口有了以后开始定时看。
  if (!browser_->GetProfile()->IsOffTheRecord()) {
    tab_organizer::TabOrganizer::ForProfile(browser_->GetProfile())->Start();
  }
  folder_prefs_.Init(browser_->GetProfile()->GetPrefs());
  folder_prefs_.AddMultiple(
      {prefs::kPinFolders, prefs::kPinsCollapsed},
      base::BindRepeating(&TabsView::Sync, base::Unretained(this),
                          TabListView::Change::kFold));
  auto schedule = base::BindRepeating(&TabsView::ScheduleFavoritesSync,
                                      base::Unretained(this));
  folder_prefs_.Add(prefs::kFavorites, schedule);
  folder_prefs_.Add(prefs::kFavoriteFolders, schedule);
  // 按网站的规则提示放进文件夹（标签行右边）：规则、开关变了就刷新。
  folder_prefs_.AddMultiple(
      {prefs::kOrganizerMemory, prefs::kOrganizeSuggest},
      base::BindRepeating(&TabsView::Sync, base::Unretained(this),
                          TabListView::Change::kNone));
  session_restored_ =
      SessionRestore::RegisterOnSessionRestoredCallback(base::BindRepeating(
          [](TabsView* self, Profile* profile, int) {
            if (self->browser_->GetProfile() == profile) {
              self->ScheduleFavoritesSync();
            }
          },
          base::Unretained(this)));
  agent_subscription_ = agent::AgentActivities::Get()->AddObserver(
      base::BindRepeating(&TabsView::OnAgentsChanged, base::Unretained(this)));
  pin_folders::WindowShown(browser_);
  ScheduleFavoritesSync();
  if (tab_tree::WindowTree* tree = tab_tree::WindowTree::From(model_)) {
    tree_changed_ = tree->AddChangedCallback(base::BindRepeating(
        &TabsView::Sync, base::Unretained(this), TabListView::Change::kFold));
  }
  model_->AddObserver(this);
}

bool TabsView::TreeShown() const {
  return !list_->horizontal() && tab_tree::Enabled(browser_->GetProfile());
}

int TabsView::HoistedIndex() const {
  tab_tree::WindowTree* tree = tab_tree::WindowTree::From(model_);
  return tree && TreeShown() ? tree->HoistedIndex() : -1;
}

void TabsView::LeaveHoist(bool focus) {
  tab_tree::WindowTree* tree = tab_tree::WindowTree::From(model_);
  const int index = HoistedIndex();
  if (!tree || index < 0) {
    return;
  }
  const tabs::TabHandle handle = model_->GetTabAtIndex(index)->GetHandle();
  tree->Unhoist();
  if (TabCell* cell = focus ? list_->CellFor(handle) : nullptr) {
    cell->RequestFocus();
  }
}

void TabsView::UpdateSticky() {
  if (!sticky_) {
    return;
  }
  // 吸顶的行盖住了上面一截：再按它们下面的那一行算一次。
  std::vector<TreeStickyHeader::Row> rows;
  if (TreeShown() && !drag_) {
    const int top = scroll_->GetVisibleRect().y();
    int covered = 0;
    for (int pass = 0; pass < 2; ++pass) {
      rows = StickyRowsAt(top + covered);
      const int height =
          static_cast<int>(rows.size()) * TreeStickyHeader::kRowHeight;
      if (height == covered) {
        break;
      }
      covered = height;
    }
  }
  sticky_->SetRows(std::move(rows), theme_);
  PlaceSticky();
}

std::vector<TreeStickyHeader::Row> TabsView::StickyRowsAt(int line) const {
  std::vector<TreeStickyHeader::Row> rows;
  if (line <= 0) {
    return rows;
  }
  // 下沿在这条线下面的第一个标签：它在线上面的最近两层祖先。
  std::optional<size_t> first;
  for (size_t i = 0; i < list_->count() && !first; ++i) {
    if (list_->SlotRect(i).bottom() > line) {
      first = i;
    }
  }
  std::optional<TabListView::Item::Tree> tree =
      first ? list_->TreeAt(*first) : std::nullopt;
  while (tree && tree->up != tabs::TabHandle() && rows.size() < kStickyMax) {
    const std::optional<size_t> up = list_->IndexOf(tree->up);
    tabs::TabInterface* tab = tree->up.Get();
    if (!up || !tab) {
      break;
    }
    const gfx::Rect slot = list_->SlotRect(*up);
    if (slot.y() < line) {
      const tabs::TabData data = StarryTabData(tab);
      rows.insert(rows.begin(), TreeStickyHeader::Row{tree->up, data.favicon,
                                                      data.title, slot.x()});
    }
    tree = list_->TreeAt(*up);
  }
  return rows;
}

void TabsView::PlaceSticky() {
  if (!sticky_) {
    return;
  }
  const gfx::Rect area = scroll_->bounds();
  sticky_->SetBounds(area.x(), area.y(), area.width(),
                     std::min(area.height(), sticky_->PreferredHeight()));
  // 滚过去的行在吸顶的行下面消失：滚动区域上面这一截遮掉，往下 kStickyFade
  // 渐渐露出来（侧栏是玻璃，吸顶的行没有底色）。
  ui::Layer* layer = list_->parent() ? list_->parent()->layer() : nullptr;
  if (!layer) {
    return;
  }
  const float covered = sticky_->VisibleHeight();
  const float total = static_cast<float>(layer->bounds().height());
  if (covered <= 0 || total <= 0) {
    if (layer->HasGradientMask()) {
      layer->SetGradientMask(gfx::LinearGradient::GetEmpty());
    }
    return;
  }
  gfx::LinearGradient mask;
  mask.set_angle(270);
  mask.AddStep(0, 0);
  mask.AddStep(std::min(1.0f, covered / total), 0);
  mask.AddStep(std::min(1.0f, (covered + kStickyFade) / total), 255);
  mask.AddStep(1, 255);
  if (layer->gradient_mask() != mask) {
    layer->SetGradientMask(mask);
  }
}

void TabsView::ScrollToTab(tabs::TabHandle handle) {
  const std::optional<size_t> index = list_->IndexOf(handle);
  if (!index) {
    return;
  }
  // 滚到它刚好露在自己的祖先（那时吸顶的几行）下面。
  int above = 0;
  for (std::optional<TabListView::Item::Tree> tree = list_->TreeAt(*index);
       tree && tree->up != tabs::TabHandle() &&
       above < static_cast<int>(kStickyMax);) {
    const std::optional<size_t> up = list_->IndexOf(tree->up);
    if (!up) {
      break;
    }
    ++above;
    tree = list_->TreeAt(*up);
  }
  const int limit =
      std::max(0, list_->height() - scroll_->GetVisibleRect().height());
  scroll_->ScrollToOffset(gfx::PointF(
      0,
      std::clamp(list_->SlotRect(*index).y() -
                     above * TreeStickyHeader::kRowHeight - kStickyScrollMargin,
                 0, limit)));
}

TabsView::~TabsView() {
  // 卡片可能还在窗口里（淡出中）：点击、拖动不再交给这里。
  if (PinFolderCard* card = this->card()) {
    card->Detach();
  }
  model_->RemoveObserver(this);
  pin_folders::WindowGone(browser_);
}

void TabsView::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  Sync(TabListView::Change::kNone);
}

TabCell* TabsView::CellAtModelIndex(int index) {
  if (index < 0 || index >= model_->count()) {
    return nullptr;
  }
  const tabs::TabHandle handle = model_->GetTabAtIndex(index)->GetHandle();
  for (TabListView* list : {pinned_.get(), pins_.get(), list_.get()}) {
    if (TabCell* cell = list ? list->CellFor(handle) : nullptr) {
      return cell;
    }
  }
  return nullptr;
}

void TabsView::ScheduleFavoritesSync() {
  if (favorites_scheduled_) {
    return;
  }
  favorites_scheduled_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<TabsView> self) {
                       if (!self) {
                         return;
                       }
                       pinned_tabs::SyncFavorites(self->browser_);
                       self->Sync(TabListView::Change::kNone);
                       self->favorites_scheduled_ = false;
                     },
                     weak_factory_.GetWeakPtr()));
}

void TabsView::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  TabListView::Change motion = TabListView::Change::kNone;
  switch (change.type()) {
    case TabStripModelChange::kInserted:
      if (!list_->horizontal()) {
        ScheduleFavoritesSync();
      }
      [[fallthrough]];
    case TabStripModelChange::kMoved:
      motion = TabListView::Change::kInsert;
      break;
    case TabStripModelChange::kRemoved:
      if (!list_->horizontal() && !model_->closing_all()) {
        for (const auto& removed : change.GetRemove()->contents) {
          if (removed.tab_detach_reason ==
              tabs::TabInterface::DetachReason::kDelete) {
            pinned_tabs::TabClosed(removed.contents);
          }
        }
      }
      motion = TabListView::Change::kClose;
      break;
    case TabStripModelChange::kReplaced:
      if (change.GetReplace()->tab->IsPinned()) {
        pinned_tabs::CopyStateIfMissing(change.GetReplace()->old_contents,
                                        change.GetReplace()->new_contents);
      }
      motion = TabListView::Change::kNone;
      break;
    case TabStripModelChange::kSelectionOnly:
      motion = TabListView::Change::kSelect;
      break;
  }
  // 切换标签时折起了其他组：用开合的动画。
  if (tab_tree::WindowTree* tree = tab_tree::WindowTree::From(model_);
      tree && tree->TakeFolded()) {
    motion = TabListView::Change::kFold;
  }
  Sync(motion);
  if (selection.active_tab_changed() && model_->active_index() >= 0) {
    RevealActive();
  }
}

void TabsView::OnTabChangedAt(tabs::TabInterface* tab,
                              TabChangeType change_type) {
  auto* view = StarryBrowserView::From(browser_);
  auto* peek = view ? view->pinned_peek() : nullptr;
  if (peek && peek->preview() == tab) {
    peek->SchedulePaint();
  }
  auto location = locations_.find(tab->GetHandle());
  const bool favorite = pinned_tabs::IsFavorite(tab);
  const std::string folder =
      favorite ? pinned_tabs::FavoriteFolder(tab) : pinned_tabs::Folder(tab);
  const bool moved =
      tab->IsPinned() &&
      (location == locations_.end() ||
       location->second !=
           std::make_tuple(favorite, folder, pinned_tabs::Order(tab)));
  const auto preview = peek && peek->preview() ? peek->preview()->GetHandle()
                                               : tabs::TabHandle();
  if (moved || preview != last_preview_) {
    // 换了文件夹、换了顺序：别的行让开。
    Sync(moved ? TabListView::Change::kInsert : TabListView::Change::kNone);
    return;
  }
  // 文件夹里的标签开始、停止出声：收着的文件夹上的小喇叭跟着变。图块文件夹
  // 上还有前四个网站的图标。
  const auto& shown = favorite ? fav_items_ : pin_items_;
  if (auto pin = shown.find(tab->GetHandle());
      pin != shown.end() && !folder.empty()) {
    const tabs::TabData now = tabs::TabData::FromTabInterface(tab);
    if (IsAudible(pin->second.data) != IsAudible(now) ||
        (favorite && !(pin->second.data.favicon == now.favicon))) {
      Sync(TabListView::Change::kNone);
      return;
    }
  }
  const tabs::TabHandle handle = tab->GetHandle();
  // 同一个标签可能在两处：顶部标签行里挂在胶囊后面，又在开着的卡片里。
  for (TabListView* list : Lists()) {
    if (TabCell* cell = list->CellFor(handle)) {
      cell->SetData(handle, StarryTabData(tab),
                    model_->GetIndexOfTab(tab) == model_->active_index() ||
                        (peek && peek->source() == tab),
                    theme_);
    }
  }
  UpdateLoadingTimer();
}

void TabsView::OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) {
  pinned_tabs::PinnedStateChanged(tab);
  Sync(TabListView::Change::kInsert);
}

void TabsView::OnTabBlockedStateChanged(tabs::TabInterface* tab) {
  OnTabChangedAt(tab, TabChangeType::kAll);
}

void TabsView::OnSplitTabChanged(const SplitTabChange& change) {
  Sync(TabListView::Change::kInsert);
}

void TabsView::BeginBatch() {
  ++batch_depth_;
}

void TabsView::RefreshFolders() {
  Sync(TabListView::Change::kFold);
}

std::set<tabs::TabHandle> TabsView::AgentPinnedTabs() const {
  std::set<tabs::TabHandle> result;
  const agent::AgentActivities* agents = agent::AgentActivities::Get();
  for (int i = 0; i < model_->IndexOfFirstNonPinnedTab(); ++i) {
    tabs::TabInterface* tab = model_->GetTabAtIndex(i);
    const bool folded = pinned_tabs::IsFavorite(tab)
                            ? !pinned_tabs::FavoriteFolder(tab).empty()
                            : !pinned_tabs::Folder(tab).empty();
    if (folded && agents->For(tab)) {
      result.insert(tab->GetHandle());
    }
  }
  return result;
}

void TabsView::OnAgentsChanged() {
  if (AgentPinnedTabs() != agent_tabs_) {
    Sync(TabListView::Change::kNone);
  }
}

void TabsView::EndBatch(TabListView::Change change) {
  if (--batch_depth_ == 0 && std::exchange(batch_dirty_, false)) {
    Sync(change);
  }
}

void TabsView::Sync(TabListView::Change change) {
  if (batch_depth_ > 0) {
    batch_dirty_ = true;
    return;
  }
  std::vector<TabListView::Item> pinned_items;
  std::vector<TabListView::Item> list_items;
  std::vector<TabListView::Item> ordinary;
  const bool sidebar = !list_->horizontal();
  const bool collapsed =
      sidebar &&
      browser_->GetProfile()->GetPrefs()->GetBoolean(prefs::kPinsCollapsed);
  bool selected_pinned = false;
  // 显示着的分屏：当前标签所在的那一组。
  std::optional<split_tabs::SplitTabId> active_split;
  if (model_->active_index() >= 0) {
    active_split = model_->GetTabAtIndex(model_->active_index())->GetSplit();
  }
  auto* window = StarryBrowserView::From(browser_);
  auto* peek = window ? window->pinned_peek() : nullptr;
  locations_.clear();
  pin_items_.clear();
  fav_items_.clear();
  agent_tabs_ = AgentPinnedTabs();
  last_preview_ = peek && peek->preview() ? peek->preview()->GetHandle()
                                          : tabs::TabHandle();
  // 树状标签页：普通标签按层缩进，折起来的不显示。聚焦一组时只有这一组里面的，
  // 从第 0 层排。
  const bool tree = TreeShown();
  const std::vector<tab_tree::Node> nodes =
      tree ? tab_tree::Compute(model_) : std::vector<tab_tree::Node>();
  const int hoisted = tree ? HoistedIndex() : -1;
  int hoist_end = hoisted + 1;
  while (hoisted >= 0 && hoist_end < model_->count() &&
         nodes[hoist_end].depth > nodes[hoisted].depth) {
    ++hoist_end;
  }
  const tabs::TabHandle hoist_handle =
      hoisted >= 0 ? model_->GetTabAtIndex(hoisted)->GetHandle()
                   : tabs::TabHandle();
  // 进出聚焦：行滑到新的缩进，别的行淡出淡入。
  if (hoist_handle != hoist_shown_) {
    hoist_shown_ = hoist_handle;
    change = TabListView::Change::kEdit;
  }
  for (int i = 0; i < model_->count(); ++i) {
    tabs::TabInterface* tab = model_->GetTabAtIndex(i);
    if (tab->IsPinned()) {
      const bool favorite = pinned_tabs::IsFavorite(tab);
      locations_[tab->GetHandle()] = {favorite,
                                      favorite
                                          ? pinned_tabs::FavoriteFolder(tab)
                                          : pinned_tabs::Folder(tab),
                                      pinned_tabs::Order(tab)};
    }
    if (peek && peek->preview() == tab) {
      continue;
    }
    TabListView::Item item;
    item.handle = tab->GetHandle();
    item.data = StarryTabData(tab);
    item.selected =
        i == model_->active_index() || (peek && peek->source() == tab);
    item.split = tab->GetSplit();
    item.split_active = item.split && item.split == active_split;
    if (pinned_tabs::IsFavorite(tab)) {
      // 常用图块（图块文件夹里的在卡片里）。
      const tabs::TabHandle handle = item.handle;
      fav_items_.emplace(handle, std::move(item));
    } else if (tab->IsPinned()) {
      // 固定区的树：侧栏的列表、顶部标签行的固定区。
      const tabs::TabHandle handle = item.handle;
      pin_items_.emplace(handle, std::move(item));
    } else {
      if (tree) {
        const tab_tree::Node& node = nodes[i];
        int base = 0;
        if (hoisted >= 0) {
          // 聚焦着：只看这一组里面的开合。
          if (i <= hoisted || i >= hoist_end) {
            continue;
          }
          base = nodes[hoisted].depth + 1;
          bool folded = false;
          for (int p = node.parent; p > hoisted; p = nodes[p].parent) {
            folded = folded || nodes[p].collapsed;
          }
          if (folded) {
            continue;
          }
        } else if (node.hidden) {
          continue;
        }
        TabListView::Item::Tree info;
        info.depth = node.depth - base;
        info.parent = node.children > 0;
        info.collapsed = node.collapsed;
        info.count = node.descendants;
        if (node.parent >= 0 && node.parent != hoisted) {
          info.up = model_->GetTabAtIndex(node.parent)->GetHandle();
        }
        // 折着：右边叠着前几个子标签的图标。
        for (int k = i + 1;
             node.collapsed && k < model_->count() &&
             nodes[k].depth > node.depth && info.icons.size() < 3;
             ++k) {
          if (nodes[k].parent == i) {
            info.icons.push_back(
                StarryTabData(model_->GetTabAtIndex(k)).favicon);
          }
        }
        item.tree = std::move(info);
      }
      ordinary.push_back(std::move(item));
    }
  }
  ReadTree();
  {
    std::vector<TabListRowData> tiles;
    BuildGrid(&tiles, &pinned_items);
    pinned_->SetRows(std::move(tiles));
    selected_pinned =
        std::ranges::any_of(pinned_items, &TabListView::Item::selected);
  }
  if (sidebar) {
    std::vector<TabListRowData> rows;
    BuildPinRows(collapsed, &rows, &list_items);
    TabListRowData new_tab;
    new_tab.kind = TabListRowData::Kind::kNewTab;
    new_tab.before_tab = list_items.size();
    new_tab.id = "@tabs";
    new_tab.title = L("tabs.untitled");
    // 「✦ 整理」：能整理的普通标签多了时一直在。
    new_tab.organize =
        static_cast<int>(tab_organizer::OrganizableTabs(browser_).size());
    new_tab.organize_hint = tab_organizer::OrganizeHintWanted(
        browser_->GetProfile(), static_cast<size_t>(new_tab.organize));
    rows.push_back(std::move(new_tab));
    // 聚焦一组：普通标签上面是面包屑。
    if (hoisted >= 0) {
      TabListRowData crumb;
      crumb.kind = TabListRowData::Kind::kHoist;
      crumb.before_tab = list_items.size();
      crumb.id = kTreeHoistId;
      const tabs::TabData data = StarryTabData(model_->GetTabAtIndex(hoisted));
      crumb.title = data.title;
      crumb.icons = {data.favicon};
      crumb.count = nodes[hoisted].descendants;
      crumb.active = hoisted == model_->active_index();
      for (int p = nodes[hoisted].parent; p >= 0; p = nodes[p].parent) {
        crumb.path.insert(crumb.path.begin(),
                          StarryTabData(model_->GetTabAtIndex(p)).title);
      }
      rows.push_back(std::move(crumb));
    }
    list_->SetRows(std::move(rows));
  }
  std::vector<TabListView::Item> strip_items;
  if (pins_) {
    std::vector<TabListRowData> rows;
    BuildTopPins(&rows, &strip_items);
    pins_->SetRows(std::move(rows));
  }
  list_items.insert(list_items.end(), ordinary.begin(), ordinary.end());

  // 选中从一组换到另一组：高亮从原来那一组现在的位置滑过来。
  TabListView* selected_list = list_;
  if (selected_pinned) {
    selected_list = pinned_;
  } else if (std::ranges::any_of(strip_items, &TabListView::Item::selected)) {
    selected_list = pins_;
  }
  std::optional<gfx::Rect> from;
  for (TabListView* other : {pinned_.get(), pins_.get(), list_.get()}) {
    if (!other || other == selected_list) {
      continue;
    }
    if (std::optional<gfx::Rect> rect = other->VisibleHighlightRect()) {
      gfx::RectF converted(*rect);
      views::View::ConvertRectToTarget(other, selected_list, &converted);
      from = gfx::ToRoundedRect(converted);
      break;
    }
  }
  auto from_for = [&](TabListView* target) -> std::optional<gfx::Rect> {
    if (target != selected_list) {
      return std::nullopt;
    }
    return from;
  };

  pinned_->Update(pinned_items, theme_, change, from_for(pinned_));
  if (pins_) {
    pins_->Update(strip_items, theme_, change, from_for(pins_));
  }
  list_->Update(list_items, theme_, change, from_for(list_));
  UpdateCard(change);
  OnSynced(change);
  UpdateSticky();
  InvalidateLayout();
  UpdateLoadingTimer();

  if (drag_) {
    // 拖着的标签、文件夹没了：拖动作废。还在的话，行可能挪了位置：重新算落点。
    const bool gone = !drag_->tile.empty() ? !fav_folders_.contains(drag_->tile)
                      : drag_->folder.empty()
                          ? !drag_->handle.Get()
                          : !folder_parents_.contains(drag_->folder);
    if (gone) {
      FinishDrag(/*cancelled=*/true);
    } else {
      UpdateDropTarget();
    }
  }
}

void TabsView::BuildGrid(std::vector<TabListRowData>* rows,
                         std::vector<TabListView::Item>* items) {
  fav_levels_ = favorite_folders::Layout(browser_);
  fav_folders_.clear();
  for (pin_folders::Folder& folder :
       favorite_folders::Read(browser_->GetProfile())) {
    const std::string id = folder.id;
    fav_folders_.emplace(id, std::move(folder));
  }
  auto level = fav_levels_.find(std::string());
  if (level == fav_levels_.end()) {
    return;
  }
  for (const pin_folders::Child& child : level->second) {
    if (!child.is_folder()) {
      if (auto item = fav_items_.find(child.tab); item != fav_items_.end()) {
        items->push_back(item->second);
      }
      continue;
    }
    auto found = fav_folders_.find(child.folder);
    if (found == fav_folders_.end()) {
      continue;
    }
    TabListRowData tile;
    tile.kind = TabListRowData::Kind::kTile;
    tile.before_tab = items->size();
    tile.id = child.folder;
    tile.title = found->second.name;
    tile.compact = list_->horizontal();
    tile.open = IsCardOpen(child.folder);
    tile.tint = pin_folders::ColorValue(found->second.color, theme_.is_dark);
    // 前四个网站的图标；当前标签在里面时图块亮起来，出声、智能体挂在角上。
    for (const pin_folders::Child& inner : fav_levels_[child.folder]) {
      auto item = fav_items_.find(inner.tab);
      if (item == fav_items_.end()) {
        continue;
      }
      ++tile.count;
      if (tile.icons.size() < 4) {
        tile.icons.push_back(item->second.data.favicon);
      }
      tile.active |= item->second.selected;
      tile.audible |= IsAudible(item->second.data);
      tile.agent |= agent_tabs_.contains(inner.tab);
    }
    rows->push_back(std::move(tile));
  }
}

TabListRow* TabsView::TileFor(const std::string& id) const {
  for (TabListRow* row : pinned_->rows()) {
    if (row->data().kind == TabListRowData::Kind::kTile &&
        row->data().id == id) {
      return row;
    }
  }
  return nullptr;
}

void TabsView::ReadTree() {
  folders_.clear();
  folder_parents_.clear();
  // 别的窗口里的空文件夹不显示（pin_folders::ShownIn）。
  const std::set<std::string> shown = pin_folders::ShownIn(browser_);
  for (pin_folders::Folder& folder :
       pin_folders::Read(browser_->GetProfile())) {
    if (!shown.contains(folder.id)) {
      continue;
    }
    folder_parents_[folder.id] = folder.parent;
    const std::string id = folder.id;
    folders_.emplace(id, std::move(folder));
  }
  levels_ = pin_folders::Layout(browser_);
  for (auto level = levels_.begin(); level != levels_.end();) {
    if (!level->first.empty() && !shown.contains(level->first)) {
      level = levels_.erase(level);
      continue;
    }
    std::erase_if(level->second, [&](const pin_folders::Child& child) {
      return child.is_folder() && !shown.contains(child.folder);
    });
    ++level;
  }
}

void TabsView::Collect(const std::string& id,
                       std::vector<const TabListView::Item*>* out,
                       int depth) const {
  auto level = levels_.find(id);
  if (level == levels_.end() || depth > kMaxFolderDepth) {
    return;
  }
  for (const pin_folders::Child& child : level->second) {
    if (child.is_folder()) {
      Collect(child.folder, out, depth + 1);
    } else if (auto pin = pin_items_.find(child.tab); pin != pin_items_.end()) {
      out->push_back(&pin->second);
    }
  }
}

void TabsView::EmitTree(const std::string& parent,
                        int depth,
                        bool collapsed,
                        const std::vector<pin_folders::Child>* subset,
                        std::vector<TabListRowData>* rows,
                        std::vector<TabListView::Item>* items,
                        std::map<tabs::TabHandle, ShownTab>* shown) const {
  using Row = TabListRowData;
  if (!subset) {
    auto level = levels_.find(parent);
    if (level == levels_.end() || depth > kMaxFolderDepth) {
      return;
    }
    subset = &level->second;
  }
  // 一层里的东西：标签和文件夹按手动排的顺序混在一起。
  for (const pin_folders::Child& child : *subset) {
    if (!child.is_folder()) {
      auto pin = pin_items_.find(child.tab);
      // 整个区折叠时只留当前标签（和它所在的最外层文件夹）。
      if (pin == pin_items_.end() || (collapsed && !pin->second.selected)) {
        continue;
      }
      items->push_back(pin->second);
      items->back().depth = depth;
      (*shown)[child.tab] = {parent, false};
      continue;
    }
    auto found = folders_.find(child.folder);
    if (found == folders_.end()) {
      continue;
    }
    const pin_folders::Folder& folder = found->second;
    std::vector<const TabListView::Item*> inside;
    Collect(folder.id, &inside, depth);
    const bool has_selected =
        std::ranges::any_of(inside, &TabListView::Item::selected);
    if (collapsed && !has_selected) {
      continue;
    }
    const bool folded = collapsed || folder.collapsed;
    Row row;
    row.kind = Row::Kind::kFolder;
    row.before_tab = items->size();
    row.id = folder.id;
    row.title = folder.name;
    row.depth = depth;
    row.collapsed = folded;
    row.count = static_cast<int>(inside.size());
    row.tint = pin_folders::ColorValue(folder.color, theme_.is_dark);
    if (folded) {
      for (size_t k = 0; k < inside.size() && k < 3; ++k) {
        row.icons.push_back(inside[k]->data.favicon);
      }
      row.active = has_selected;
      // 收着的标签里有出声的、智能体在操作的：挂在这一行上（留在下面的当前
      // 标签自己显示）。
      for (const TabListView::Item* item : inside) {
        if (!item->selected) {
          row.audible |= IsAudible(item->data);
          row.agent |= agent_tabs_.contains(item->handle);
        }
      }
    } else if (auto own = levels_.find(folder.id); own != levels_.end()) {
      // 当前标签直接在这个文件夹里：它的层级线变亮。
      row.active =
          std::ranges::any_of(own->second, [&](const pin_folders::Child& kid) {
            auto pin = pin_items_.find(kid.tab);
            return !kid.is_folder() && pin != pin_items_.end() &&
                   pin->second.selected;
          });
    }
    rows->push_back(std::move(row));
    if (folded) {
      // 折叠时当前标签留在文件夹下面，切走了才收进去。
      for (const TabListView::Item* item : inside) {
        if (item->selected) {
          items->push_back(*item);
          items->back().depth = depth + 1;
          (*shown)[item->handle] = {folder.id, true};
        }
      }
      continue;
    }
    const size_t before = rows->size() + items->size();
    EmitTree(folder.id, depth + 1, collapsed, nullptr, rows, items, shown);
    if (rows->size() + items->size() == before) {
      Row empty;
      empty.kind = Row::Kind::kEmpty;
      empty.before_tab = items->size();
      empty.id = folder.id + "/empty";
      empty.title = L("tabs.folderEmpty");
      empty.depth = depth + 1;
      rows->push_back(std::move(empty));
    }
  }
}

void TabsView::BuildPinRows(bool collapsed,
                            std::vector<TabListRowData>* rows,
                            std::vector<TabListView::Item>* items) {
  using Row = TabListRowData;
  shown_tabs_.clear();

  // 一个固定标签、文件夹都没有时，固定区不占位置；标签拖到列表顶上时露出一个
  // 落点。
  const bool has_pins = !pin_items_.empty() || !folders_.empty();
  const bool reveal = !has_pins && drag_ && drag_->reveal_pins;
  if (has_pins) {
    Row label;
    label.kind = Row::Kind::kLabel;
    label.id = "@pins";
    label.title = L("tabs.pinnedSection");
    label.collapsed = collapsed;
    label.count = static_cast<int>(pin_items_.size());
    rows->push_back(std::move(label));
  }
  EmitTree(std::string(), 0, collapsed, nullptr, rows, items, &shown_tabs_);

  if (reveal) {
    Row empty;
    empty.kind = Row::Kind::kEmpty;
    empty.before_tab = items->size();
    empty.id = "@pins/empty";
    empty.title = L("tabs.folderEmpty");
    rows->push_back(std::move(empty));
  }
  if (has_pins || reveal) {
    Row divider;
    divider.kind = Row::Kind::kDivider;
    divider.before_tab = items->size();
    divider.id = "@divider";
    rows->push_back(std::move(divider));
  }
}

void TabsView::BuildTopPins(std::vector<TabListRowData>* rows,
                            std::vector<TabListView::Item>* items) {
  using Row = TabListRowData;
  shown_tabs_.clear();
  overflow_.clear();
  // 最外层的一项：散着的固定标签，或者文件夹的胶囊（当前标签在里面时挂在
  // 后面）。
  struct Unit {
    pin_folders::Child child;
    std::vector<Row> rows;
    std::vector<TabListView::Item> items;
    bool selected = false;
  };
  std::vector<Unit> units;
  if (auto level = levels_.find(std::string()); level != levels_.end()) {
    for (const pin_folders::Child& child : level->second) {
      Unit unit;
      unit.child = child;
      if (!child.is_folder()) {
        auto pin = pin_items_.find(child.tab);
        if (pin == pin_items_.end()) {
          continue;
        }
        TabListView::Item item = pin->second;
        item.wide = item.selected;
        item.width = kTopPinWideWidth;
        unit.selected = item.selected;
        unit.items.push_back(std::move(item));
        units.push_back(std::move(unit));
        continue;
      }
      auto found = folders_.find(child.folder);
      if (found == folders_.end()) {
        continue;
      }
      std::vector<const TabListView::Item*> inside;
      Collect(child.folder, &inside, 0);
      Row chip;
      chip.kind = Row::Kind::kChip;
      chip.id = child.folder;
      chip.title = found->second.name;
      chip.count = static_cast<int>(inside.size());
      chip.open = IsCardOpen(child.folder);
      chip.tint = pin_folders::ColorValue(found->second.color, theme_.is_dark);
      for (const TabListView::Item* inner : inside) {
        if (!inner->selected) {
          chip.audible |= IsAudible(inner->data);
          chip.agent |= agent_tabs_.contains(inner->handle);
        }
      }
      for (const TabListView::Item* inner : inside) {
        if (inner->selected) {
          TabListView::Item item = *inner;
          item.wide = true;
          item.width = kTopPinWideWidth;
          item.tray = child.folder;
          item.depth = 0;
          chip.active = true;
          unit.selected = true;
          unit.items.push_back(std::move(item));
          break;
        }
      }
      unit.rows.push_back(std::move(chip));
      units.push_back(std::move(unit));
    }
  }

  std::vector<bool> keep(units.size(), true);
  bool compact = false;
  bool more = false;
  auto flatten = [&](std::vector<Row>* out_rows,
                     std::vector<TabListView::Item>* out_items) {
    for (size_t i = 0; i < units.size(); ++i) {
      if (!keep[i]) {
        continue;
      }
      for (Row row : units[i].rows) {
        row.before_tab = out_items->size();
        row.compact = compact;
        out_rows->push_back(std::move(row));
      }
      out_items->insert(out_items->end(), units[i].items.begin(),
                        units[i].items.end());
    }
    if (more) {
      Row row;
      row.kind = Row::Kind::kChip;
      row.id = kMorePinsId;
      row.title = L("tabs.morePins");
      row.compact = true;
      row.open = IsCardOpen(kMorePinsId);
      row.before_tab = out_items->size();
      out_rows->push_back(std::move(row));
    }
  };
  auto measure = [&]() {
    std::vector<Row> measured_rows;
    std::vector<TabListView::Item> measured_items;
    flatten(&measured_rows, &measured_items);
    return TabListView::LayoutTopPins(measured_rows, measured_items, nullptr);
  };
  strip_full_width_ = measure();
  compact = true;
  strip_compact_width_ = measure();
  compact = false;
  // 放不下：先把胶囊收成图标，还放不下就从后往前收进最后的「…」（当前标签
  // 所在的那一项一直留着）。
  const int limit = PinStripLimit();
  if (limit > 0 && measure() > limit) {
    compact = true;
    if (measure() > limit) {
      more = true;
      for (size_t i = units.size(); i-- > 0 && measure() > limit;) {
        if (!units[i].selected) {
          keep[i] = false;
        }
      }
    }
  }
  for (size_t i = 0; i < units.size(); ++i) {
    if (!keep[i]) {
      overflow_.push_back(units[i].child);
    }
  }
  more = !overflow_.empty();
  flatten(rows, items);
  for (const TabListView::Item& item : *items) {
    shown_tabs_[item.handle] = {item.tray, !item.tray.empty()};
  }
}

bool TabsView::BuildCard(const std::string& root,
                         std::u16string* title,
                         std::vector<TabListRowData>* rows,
                         std::vector<TabListView::Item>* items) {
  card_shown_.clear();
  if (auto favorite = fav_folders_.find(root); favorite != fav_folders_.end()) {
    *title = favorite->second.name;
    for (const pin_folders::Child& child : fav_levels_[root]) {
      auto item = fav_items_.find(child.tab);
      if (item == fav_items_.end()) {
        continue;
      }
      items->push_back(item->second);
      items->back().depth = 0;
      card_shown_[child.tab] = {root, false};
    }
    if (items->empty()) {
      TabListRowData empty;
      empty.kind = TabListRowData::Kind::kEmpty;
      empty.id = root + "/empty";
      empty.title = L("tabs.folderEmpty");
      rows->push_back(std::move(empty));
    }
    return true;
  }
  if (root == kMorePinsId) {
    if (overflow_.empty()) {
      return false;
    }
    *title = L("tabs.morePins");
    EmitTree(std::string(), 0, false, &overflow_, rows, items, &card_shown_);
    return true;
  }
  auto found = folders_.find(root);
  if (found == folders_.end()) {
    return false;
  }
  *title = found->second.name;
  EmitTree(root, 0, false, nullptr, rows, items, &card_shown_);
  if (rows->empty() && items->empty()) {
    TabListRowData empty;
    empty.kind = TabListRowData::Kind::kEmpty;
    empty.id = root + "/empty";
    empty.title = L("tabs.folderEmpty");
    rows->push_back(std::move(empty));
  }
  return true;
}

std::vector<TabListView*> TabsView::Lists() const {
  std::vector<TabListView*> lists{pinned_.get(), list_.get()};
  if (pins_) {
    lists.push_back(pins_.get());
  }
  if (TabListView* card = CardList()) {
    lists.push_back(card);
  }
  return lists;
}

TabListView* TabsView::ListFor(DropTarget::Area area) const {
  switch (area) {
    case DropTarget::Area::kGrid:
      return pinned_;
    case DropTarget::Area::kList:
      return list_;
    case DropTarget::Area::kPins:
      return pins_;
    case DropTarget::Area::kCard:
      return CardList();
  }
}

std::string TabsView::FolderParent(const std::string& id) const {
  auto found = folder_parents_.find(id);
  return found == folder_parents_.end() ? std::string() : found->second;
}

tabs::TabInterface* TabsView::TabFor(TabCell* cell) const {
  return cell->handle().Get();
}

int TabsView::ModelIndexFor(TabCell* cell) const {
  tabs::TabInterface* tab = TabFor(cell);
  return tab ? model_->GetIndexOfTab(tab) : TabStripModel::kNoTab;
}

void TabsView::TabCellClicked(TabCell* cell, bool reset, bool keep_current) {
  TabListView* card_list = CardList();
  const bool in_card = card_list && cell->parent() == card_list;
  ActivateCell(cell, reset, keep_current);
  if (in_card) {
    CloseCard();
  }
}

void TabsView::ActivateCell(TabCell* cell, bool reset, bool keep_current) {
  const int index = ModelIndexFor(cell);
  if (index == TabStripModel::kNoTab) {
    return;
  }
  auto* tab = TabFor(cell);
  if (reset && tab->IsPinned() &&
      pinned_tabs::PinnedUrl(tab) != tab->GetContents()->GetVisibleURL()) {
    pinned_tabs::Reset(tab, keep_current);
    return;
  }
  cell->Bounce();
  model_->ActivateTabAt(index,
                        TabStripUserGestureDetails(
                            TabStripUserGestureDetails::GestureType::kMouse));
}

void TabsView::TabCellCloseRequested(TabCell* cell, bool button) {
  auto* tab = TabFor(cell);
  if (!tab) {
    return;
  }
  // 树里折着的父标签（设置是一起关时展开着的也是）：整枝一起关。
  if (TreeShown() && !tab->IsPinned() &&
      tab_tree::ClosesBranch(model_, model_->GetIndexOfTab(tab),
                             /*from_sidebar=*/true)) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<BrowserWindowInterface> browser,
               tabs::TabHandle handle) {
              auto* tab = handle.Get();
              if (!browser || !tab ||
                  tab->GetBrowserWindowInterface() != browser.get()) {
                return;
              }
              TabStripModel* model = browser->GetTabStripModel();
              tab_tree::CloseBranch(model, model->GetIndexOfTab(tab));
            },
            browser_->GetWeakPtr(), tab->GetHandle()));
    return;
  }
  const bool archive =
      button && tab->IsPinned() && tab->GetContents()->WasDiscarded();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser,
                        tabs::TabHandle handle, bool archive) {
                       auto* tab = handle.Get();
                       if (!browser || !tab ||
                           tab->GetBrowserWindowInterface() != browser.get()) {
                         return;
                       }
                       if (!archive && pinned_tabs::Close(browser.get(), tab)) {
                         return;
                       }
                       // 点 ✕ 关掉休眠的固定标签：不进标签管理的归档。
                       pinned_tabs::Archive(tab, /*to_tab_archive=*/false);
                     },
                     browser_->GetWeakPtr(), tab->GetHandle(), archive));
}

void TabsView::TabCellSuggestionAccepted(TabCell* cell) {
  tabs::TabInterface* tab = TabFor(cell);
  if (!tab) {
    return;
  }
  // 标签要挪到固定区：晚一步做，不在点击的回调里改列表。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser,
                        tabs::TabHandle handle) {
                       tabs::TabInterface* tab = handle.Get();
                       if (browser && tab &&
                           tab->GetBrowserWindowInterface() == browser.get()) {
                         tab_organizer::AcceptSuggestion(tab);
                       }
                     },
                     browser_->GetWeakPtr(), tab->GetHandle()));
}

void TabsView::TabCellTreeToggled(TabCell* cell, bool all) {
  const int index = ModelIndexFor(cell);
  if (index == TabStripModel::kNoTab || !TreeShown()) {
    return;
  }
  tab_tree::SetCollapsed(model_, index, !cell->tree_collapsed(), all);
}

void TabsView::TabCellMuteToggled(TabCell* cell) {
  tabs::TabInterface* tab = TabFor(cell);
  if (!tab) {
    return;
  }
  content::WebContents* contents = tab->GetContents();
  SetTabAudioMuted(contents, !contents->IsAudioMuted(),
                   TabMutedReason::kAudioIndicator, std::string());
}

void TabsView::TabCellHoverChanged(TabCell* cell, bool hovered) {
  // 树：祖先的层级线从上面亮到它。
  if (cell->parent() == list_.get() && TreeShown() &&
      (hovered || list_->hovered_tab() == cell->handle())) {
    list_->SetHoveredTab(hovered && !drag_ ? cell->handle()
                                           : tabs::TabHandle());
  }
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  tabs::TabInterface* tab = TabFor(cell);
  if (!view || !view->has_starry_chrome() || !tab || drag_) {
    return;
  }
  TabPreview* preview = view->tab_preview();
  const gfx::Rect anchor =
      views::View::ConvertRectToTarget(cell, preview, cell->ContentRect());
  preview->Hover(tab, anchor, PreviewEdge(preview), hovered);
}

bool TabsView::TabCellShowsTooltip() {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  return !view || !view->tab_preview() || !view->tab_preview()->Enabled();
}

void TabsView::TabFolderAction(const std::string& id,
                               std::optional<gfx::Point> menu_point) {
  if (id == "@tabs") {
    if (auto* view = StarryBrowserView::From(browser_)) {
      view->NewTab();
    }
    return;
  }
  if (id == "@organize") {
    OpenOrganize(browser_);
    return;
  }
  if (id == kTreeHoistId) {
    if (!menu_point) {
      LeaveHoist(/*focus=*/false);
    }
    return;
  }
  if (id == "@newFolder" || (id == "@pins" && menu_point)) {
    pin_folders::Create(browser_, {});
    return;
  }
  // 常用图块的文件夹：点它落下（收起）卡片；菜单是图块文件夹的菜单。
  if (auto favorite = fav_folders_.find(id); favorite != fav_folders_.end()) {
    if (!menu_point) {
      ToggleCard(id);
      return;
    }
    auto* view = StarryBrowserView::From(browser_);
    if (!view) {
      return;
    }
    folder_menu_ = std::make_unique<pin_folders::Menu>(
        browser_, favorite->second, /*favorites=*/true);
    gfx::Point point = *menu_point;
    views::View::ConvertPointFromScreen(view, &point);
    MenuOptions options;
    options.destructive =
        base::BindRepeating(&pin_folders::Menu::IsDestructive);
    view->ShowMenu(folder_menu_->model(), point, base::DoNothing(),
                   std::move(options));
    return;
  }
  if (!menu_point) {
    // Preference observers rebuild the section buttons.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<BrowserWindowInterface> browser,
                          std::string folder) {
                         if (browser) {
                           if (folder == "@pins") {
                             auto* settings = browser->GetProfile()->GetPrefs();
                             settings->SetBoolean(
                                 prefs::kPinsCollapsed,
                                 !settings->GetBoolean(prefs::kPinsCollapsed));
                           } else {
                             pin_folders::Toggle(browser->GetProfile(), folder);
                           }
                         }
                       },
                       browser_->GetWeakPtr(), id));
    return;
  }
  auto* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  for (const auto& folder : pin_folders::Read(browser_->GetProfile())) {
    if (folder.id != id) {
      continue;
    }
    folder_menu_ = std::make_unique<pin_folders::Menu>(browser_, folder);
    views::View::ConvertPointFromScreen(view, &*menu_point);
    MenuOptions options;
    options.destructive =
        base::BindRepeating(&pin_folders::Menu::IsDestructive);
    view->ShowMenu(folder_menu_->model(), *menu_point, base::DoNothing(),
                   std::move(options));
    break;
  }
}

void TabsView::TabFolderOpenAll(const std::string& id) {
  if (fav_folders_.contains(id)) {
    pin_folders::OpenTabs(favorite_folders::TabsIn(browser_, id));
    return;
  }
  if (folders_.contains(id)) {
    pin_folders::OpenAll(browser_, id);
  }
}

void TabsView::TabFolderToggleTree(const std::string& id) {
  auto found = folders_.find(id);
  if (found == folders_.end()) {
    return;
  }
  // 行会重排：晚一步做，不在点击回调里改。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser,
                        std::string folder, bool collapsed) {
                       if (browser) {
                         pin_folders::SetTreeCollapsed(browser->GetProfile(),
                                                       folder, collapsed);
                       }
                     },
                     browser_->GetWeakPtr(), id, !found->second.collapsed));
}

std::vector<TabListView*> TabsView::FocusChain(TabListView* list) const {
  if (list == CardList()) {
    return {list};
  }
  std::vector<TabListView*> chain{pinned_.get()};
  if (pins_) {
    chain.push_back(pins_.get());
  }
  chain.push_back(list_.get());
  return chain;
}

void TabsView::MoveFocus(TabListView* list, views::View* from, int direction) {
  const std::vector<views::View*> order = list->FocusOrder();
  if (auto it = std::ranges::find(order, from); it != order.end()) {
    const auto index = (it - order.begin()) + direction;
    if (index >= 0 && index < static_cast<int>(order.size())) {
      order[index]->RequestFocus();
      return;
    }
  }
  // 到头了：换到前一个、后一个列表（空着、藏着的跳过）。
  const std::vector<TabListView*> chain = FocusChain(list);
  auto at = std::ranges::find(chain, list);
  if (at == chain.end()) {
    return;
  }
  for (int index = static_cast<int>(at - chain.begin()) + direction;
       index >= 0 && index < static_cast<int>(chain.size());
       index += direction) {
    const std::vector<views::View*> next = chain[index]->FocusOrder();
    if (!next.empty()) {
      (direction > 0 ? next.front() : next.back())->RequestFocus();
      return;
    }
  }
}

bool TabsView::FocusFolderRow(TabListView* list, const std::string& id) {
  for (TabListRow* row : list->rows()) {
    if (row->data().id == id && row->IsFocusable()) {
      row->RequestFocus();
      return true;
    }
  }
  return false;
}

bool TabsView::TabListKeyPressed(views::View* item, const ui::KeyEvent& event) {
  auto* list = item->parent() ? views::AsViewClass<TabListView>(item->parent())
                              : nullptr;
  if (!list) {
    return false;
  }
  const ui::KeyboardCode key = event.key_code();
  // 聚焦一组时列表里的 Esc：回到整棵树，焦点在那个标签上。
  if (key == ui::VKEY_ESCAPE && list == list_.get() && HoistedIndex() >= 0) {
    LeaveHoist(/*focus=*/true);
    return true;
  }
  if (key == ui::VKEY_ESCAPE) {
    // 卡片里的 Esc：收起卡片，焦点回到胶囊（晚一步，卡片里的视图还在处理这个
    // 按键）。别处焦点回到网页。
    if (list == CardList()) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<TabsView> self, std::string id) {
                if (!self || id.empty() || !self->IsCardOpen(id)) {
                  return;
                }
                self->CloseCard();
                if (!self->pins_ || !self->FocusFolderRow(self->pins_, id)) {
                  self->FocusFolderRow(self->pinned_, id);
                }
              },
              weak_factory_.GetWeakPtr(), OpenCardId()));
      return true;
    }
    if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
      tab->GetContents()->Focus();
      return true;
    }
    return false;
  }
  auto* row = views::AsViewClass<TabListRow>(item);
  auto* cell = views::AsViewClass<TabCell>(item);
  const bool grid = list->style() == TabListView::Style::kGrid;
  const bool vertical = !list->horizontal() && !grid;

  // 图块文件夹：回车、空格（顶部标签行里还有 ↓）落下卡片，焦点进到卡片里；
  // 卡片开着时回车、空格收起。
  if (row && row->data().kind == TabListRowData::Kind::kTile &&
      (key == ui::VKEY_RETURN || key == ui::VKEY_SPACE ||
       (key == ui::VKEY_DOWN && list->horizontal()))) {
    const std::string id = row->data().id;
    if (IsCardOpen(id) && key != ui::VKEY_DOWN) {
      CloseCard();
      return true;
    }
    if (!IsCardOpen(id)) {
      OpenCard(id, /*sprung=*/false);
    }
    if (TabListView* card = CardList()) {
      if (const std::vector<views::View*> inside = card->FocusOrder();
          !inside.empty()) {
        inside.front()->RequestFocus();
      }
    }
    return true;
  }

  // 竖着的列表里 → 展开（展开着就往下走），← 折叠（折叠着、或者是里面的
  // 标签，就回到上一层的文件夹）。
  if (vertical && (key == ui::VKEY_RIGHT || key == ui::VKEY_LEFT)) {
    const bool right = key == ui::VKEY_RIGHT;
    // 面包屑：← 回到整棵树，→ 往下走。
    if (row && row->data().kind == TabListRowData::Kind::kHoist) {
      if (right) {
        MoveFocus(list, item, 1);
      } else {
        LeaveHoist(/*focus=*/true);
      }
      return true;
    }
    if (row && (row->data().kind == TabListRowData::Kind::kFolder ||
                row->data().kind == TabListRowData::Kind::kLabel)) {
      const TabListRowData& data = row->data();
      if (right == data.collapsed) {
        PrefService* settings = browser_->GetProfile()->GetPrefs();
        if (data.id == "@pins") {
          settings->SetBoolean(prefs::kPinsCollapsed, !right);
        } else {
          pin_folders::SetCollapsed(browser_->GetProfile(), data.id, !right);
        }
      } else if (right) {
        MoveFocus(list, item, 1);
      } else if (data.kind == TabListRowData::Kind::kFolder) {
        FocusFolderRow(list, FolderParent(data.id));
      }
      return true;
    }
    // 树里的标签：→ 展开（展开着就往下走），← 折叠（没有展开的子标签就回到父标签），
    // 按着 ⌥ 连同里面的各层（同文件夹）。⇧⌥ → 作为上一个兄弟的子标签，⇧⌥ ← 提升
    // 一级，焦点留在它身上。
    if (cell && list == list_.get() && TreeShown() && TabFor(cell) &&
        !TabFor(cell)->IsPinned()) {
      const int index = ModelIndexFor(cell);
      if (event.IsShiftDown() && event.IsAltDown()) {
        const tabs::TabHandle handle = cell->handle();
        if (index != TabStripModel::kNoTab) {
          if (right) {
            tab_tree::Indent(model_, index);
          } else {
            tab_tree::Outdent(model_, index);
          }
        }
        if (TabCell* moved = list->CellFor(handle);
            moved && !moved->HasFocus()) {
          moved->RequestFocus();
        }
        return true;
      }
      if (cell->tree_parent() && right == cell->tree_collapsed()) {
        tab_tree::SetCollapsed(model_, index, !right, event.IsAltDown());
      } else if (right) {
        if (cell->tree_parent()) {
          MoveFocus(list, item, 1);
        }
      } else if (index != TabStripModel::kNoTab) {
        const std::vector<tab_tree::Node> nodes = tab_tree::Compute(model_);
        // 聚焦着的一组里最外层的：父标签是面包屑，回到整棵树。
        if (nodes[index].parent >= 0 && nodes[index].parent == HoistedIndex()) {
          LeaveHoist(/*focus=*/true);
          return true;
        }
        if (nodes[index].parent >= 0) {
          if (TabCell* up = list->CellFor(
                  model_->GetTabAtIndex(nodes[index].parent)->GetHandle())) {
            up->RequestFocus();
          }
        }
      }
      return true;
    }
    if (cell && !right) {
      const auto& shown = list == CardList() ? card_shown_ : shown_tabs_;
      if (auto found = shown.find(cell->handle());
          found != shown.end() && !found->second.under.empty()) {
        FocusFolderRow(list, found->second.under);
      }
    }
    return true;
  }

  const std::vector<views::View*> order = list->FocusOrder();
  switch (key) {
    case ui::VKEY_HOME:
    case ui::VKEY_END:
      if (!order.empty()) {
        (key == ui::VKEY_HOME ? order.front() : order.back())->RequestFocus();
      }
      return true;
    case ui::VKEY_UP:
    case ui::VKEY_DOWN: {
      const int direction = key == ui::VKEY_DOWN ? 1 : -1;
      if (vertical) {
        MoveFocus(list, item, direction);
        return true;
      }
      // 顶部标签行的胶囊：↓ 落下卡片，焦点进到卡片里。
      if (row && row->data().kind == TabListRowData::Kind::kChip &&
          direction > 0) {
        if (!IsCardOpen(row->data().id)) {
          TabFolderAction(row->data().id, std::nullopt);
        }
        if (TabListView* card = CardList()) {
          if (const std::vector<views::View*> inside = card->FocusOrder();
              !inside.empty()) {
            inside.front()->RequestFocus();
          }
        }
        return true;
      }
      if (!grid) {
        return false;
      }
      // 图块：上一行、下一行里横着离得最近的那个；没有就换到前后的列表。
      views::View* best = nullptr;
      int best_rows = 0;
      int best_dx = 0;
      const int center = item->bounds().CenterPoint().x();
      for (views::View* other : order) {
        const int rows = (other->y() - item->y()) * direction;
        if (rows <= 0) {
          continue;
        }
        const int dx = std::abs(other->bounds().CenterPoint().x() - center);
        if (!best || rows < best_rows || (rows == best_rows && dx < best_dx)) {
          best = other;
          best_rows = rows;
          best_dx = dx;
        }
      }
      if (best) {
        best->RequestFocus();
      } else {
        MoveFocus(list, direction > 0 ? order.back() : order.front(),
                  direction);
      }
      return true;
    }
    case ui::VKEY_LEFT:
    case ui::VKEY_RIGHT:
      MoveFocus(list, item, key == ui::VKEY_RIGHT ? 1 : -1);
      return true;
    default:
      return false;
  }
}

void TabsView::TabFolderAddCurrent(const std::string& id) {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  if (!tab) {
    return;
  }
  if (fav_folders_.contains(id)) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<BrowserWindowInterface> browser,
               tabs::TabHandle handle, std::string folder) {
              tabs::TabInterface* tab = handle.Get();
              if (browser && tab &&
                  tab->GetBrowserWindowInterface() == browser.get()) {
                favorite_folders::Place(
                    browser.get(), pin_folders::Child{handle, std::string()},
                    folder);
              }
            },
            browser_->GetWeakPtr(), tab->GetHandle(), id));
    return;
  }
  // 标签换了位置，行会重排：晚一步做，不在按钮的点击回调里改。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser,
                        tabs::TabHandle handle, std::string folder) {
                       tabs::TabInterface* tab = handle.Get();
                       if (browser && tab &&
                           tab->GetBrowserWindowInterface() == browser.get()) {
                         tab_organizer::NoteManualMove(
                             tab,
                             tab->IsPinned() && !pinned_tabs::IsFavorite(tab)
                                 ? pinned_tabs::Folder(tab)
                                 : std::string(),
                             folder);
                         pinned_tabs::SetLocation(tab, false, folder);
                       }
                     },
                     browser_->GetWeakPtr(), tab->GetHandle(), id));
}

bool TabsView::BeginCardRename(const std::string& id) {
  PinFolderCard* card = this->card();
  if (!card || !IsCardOpen(id)) {
    return false;
  }
  card->BeginRename();
  return card->renaming();
}

bool TabsView::BeginFolderRename(const std::string& id) {
  return BeginFolderRename(id, NewFolder());
}

bool TabsView::BeginFolderRename(const std::string& id, NewFolder fresh) {
  if (!list_->BeginRename(id)) {
    return false;
  }
  fresh.id = fresh.id.empty() ? std::string() : id;
  new_folder_ = std::move(fresh);
  return true;
}

void TabsView::TabFolderRenameFinished(const std::string& id,
                                       std::optional<std::u16string> name) {
  NewFolder fresh;
  if (new_folder_.id == id) {
    fresh = std::exchange(new_folder_, NewFolder());
  }
  Profile* profile = browser_->GetProfile();
  std::u16string trimmed;
  if (name) {
    base::TrimWhitespace(*name, base::TRIM_ALL, &trimmed);
  }
  if (fav_folders_.contains(id)) {
    // 图块文件夹（卡片上面的名字）：Esc 留着原来的名字。
    if (!trimmed.empty()) {
      favorite_folders::SetName(profile, id, trimmed);
    }
  } else if (!trimmed.empty()) {
    pin_folders::Rename(profile, id, trimmed);
  } else if (!name && fresh.id == id) {
    // 新建到一半按了 Esc：文件夹不要了，建的时候放进去的标签回到原来的地方
    // （固定标签回到文件夹占着的那个位置上）。
    BeginBatch();
    if (tabs::TabInterface* tab = fresh.moved.Get();
        tab && tab->GetBrowserWindowInterface() == browser_) {
      if (fresh.was_pinned && !fresh.was_favorite) {
        pin_folders::Place(
            browser_, pin_folders::Child{fresh.moved, std::string()},
            fresh.old_folder, pin_folders::Child{tabs::TabHandle(), id});
      } else if (fresh.was_pinned) {
        pinned_tabs::SetLocation(tab, true, std::string());
      } else if (tab->IsPinned()) {
        model_->SetTabPinned(model_->GetIndexOfTab(tab), false);
      }
    }
    pin_folders::Remove(profile, id);
    EndBatch(TabListView::Change::kFold);
  }
  if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
    tab->GetContents()->Focus();
  }
}

void TabsView::TabListGapHovered(bool hovered) {
  if (StarryBrowserView* view = StarryBrowserView::From(browser_);
      view && view->tab_preview()) {
    view->tab_preview()->HoverGap(hovered);
  }
}

void TabsView::TabCellDragStarted(TabCell* cell, const gfx::Point& location) {
  if (drag_ || !TabFor(cell)) {
    return;
  }
  if (StarryBrowserView* view = StarryBrowserView::From(browser_);
      view && view->tab_preview()) {
    view->tab_preview()->Dismiss(cell->handle(), /*cooling=*/true);
  }
  Drag drag;
  drag.handle = cell->handle();
  // 从常用的图块里拖出来的（顶部标签行里散着的固定标签也是图块）。
  drag.from_pinned = cell->parent() == pinned_.get();
  drag.grab_offset = location.OffsetFromOrigin();
  drag_ = drag;
  InvalidateLayout();
  cell->SetLifted(true);
  // 树里：拖的是整枝，它下面显示着的一起变淡，小条上写下面还有几个。按着 ⌥
  // 开始拖的只拖它一个：子标签马上上移一级留在原处，取消、放回原处时挂回去。
  int extra = 0;
  if (TreeShown() && cell->parent() == list_.get() &&
      !TabFor(cell)->IsPinned()) {
    const int index = ModelIndexFor(cell);
    if (cell->drag_alone() && cell->tree_parent()) {
      drag_->detached = tab_tree::DetachChildren(model_, index);
    }
    const int end = tab_tree::BranchEnd(model_, index);
    extra = end - index - 1;
    for (int i = index + 1; i < end; ++i) {
      if (TabCell* kid =
              list_->CellFor(model_->GetTabAtIndex(i)->GetHandle())) {
        kid->SetLifted(true);
      }
    }
  }
  drag_pill_->Show(cell->data(), theme_, extra);
  DragStateChanged(true);
}

void TabsView::TabCellDragged(TabCell* cell, const gfx::Point& location) {
  if (!drag_ || drag_->handle != cell->handle()) {
    return;
  }
  gfx::Point point = location;
  views::View::ConvertPointToTarget(cell, this, &point);
  DragTo(point);
}

void TabsView::TabCellDragEnded(TabCell* cell, bool cancelled) {
  if (!drag_ || drag_->handle != cell->handle()) {
    return;
  }
  FinishDrag(cancelled);
}

void TabsView::TabRowDragStarted(TabListRow* row, const gfx::Point& location) {
  const TabListRowData& data = row->data();
  const bool tile = data.kind == TabListRowData::Kind::kTile;
  if (drag_ || data.id == kMorePinsId ||
      (data.kind != TabListRowData::Kind::kFolder &&
       data.kind != TabListRowData::Kind::kChip && !tile)) {
    return;
  }
  Drag drag;
  (tile ? drag.tile : drag.folder) = data.id;
  drag.grab_offset = location.OffsetFromOrigin();
  drag_ = drag;
  if (tile) {
    // 拖着图块走：它的卡片收起来。
    if (IsCardOpen(data.id)) {
      CloseCard();
    }
    row->SetLifted(true);
  } else {
    LiftFolder(drag.folder);
  }
  drag_pill_->ShowFolder(data.title, data.count, data.tint, theme_);
  DragStateChanged(true);
}

void TabsView::TabRowDragged(TabListRow* row, const gfx::Point& location) {
  if (!drag_ ||
      (drag_->tile.empty() ? drag_->folder : drag_->tile) != row->data().id) {
    return;
  }
  gfx::Point point = location;
  views::View::ConvertPointToTarget(row, this, &point);
  DragTo(point);
}

void TabsView::TabRowDragEnded(TabListRow* row, bool cancelled) {
  if (!drag_ ||
      (drag_->tile.empty() ? drag_->folder : drag_->tile) != row->data().id) {
    return;
  }
  FinishDrag(cancelled);
}

void TabsView::DragTo(const gfx::Point& point) {
  drag_->point = point;
  // 小条保持按下时的偏移（太靠右时让小条留在鼠标左边一点）。顶部标签行里
  // 小条挂在鼠标下面，不盖住要放进去的胶囊和标签。
  int grab_x = std::min(drag_->grab_offset.x(), drag_pill_->pill_width() - 12);
  int grab_y = std::min(drag_->grab_offset.y(), kPillHeight - 6);
  // 侧栏的常用图块上也挂在鼠标下面：不盖住要放进去、要合成的图块。
  gfx::Rect grid = pinned_->bounds();
  grid.Outset(gfx::Outsets::VH(8, 24));
  if (list_->horizontal() ||
      (pinned_->slot_count() > 0 && grid.Contains(point))) {
    grab_x = std::min(grab_x, kTopPillGrabX);
    grab_y = -kTopPillDrop;
  }
  drag_->grab_x = grab_x;
  drag_pill_->MoveTo(ToPillParent(point - gfx::Vector2d(grab_x, grab_y)));
  // 标签拖到内容区上：分屏的落点预览（不在标签里排序）。文件夹不分屏。
  drag_->over_content = false;
  if (is_dragging_tab()) {
    if (SplitOverlay* overlay = SplitOverlayFor(browser_);
        overlay && IsOverContent(point)) {
      gfx::Point local = point;
      views::View::ConvertPointToTarget(this, overlay, &local);
      drag_->over_content = overlay->DragTabTo(drag_->handle.Get(), local);
    } else if (SplitOverlay* other = SplitOverlayFor(browser_)) {
      other->DragTabTo(nullptr, gfx::Point());
    }
  }
  // 侧栏的固定区空着：标签拖到列表顶上（「新标签页」那一行和它上面）时露出落点。
  // 一开始拖就露出来的话，只是想给普通标签排序时整个列表也要往下跳一截。
  if (!drag_->reveal_pins && !drag_->over_content && is_dragging_tab() &&
      !list_->horizontal() && levels_.empty()) {
    gfx::Point local = point;
    views::View::ConvertPointToTarget(this, list_, &local);
    if (local.x() >= -kPinRevealMargin &&
        local.x() <= list_->width() + kPinRevealMargin &&
        local.y() < kPinRevealZone) {
      drag_->reveal_pins = true;
      Sync(TabListView::Change::kFold);
    }
  }
  UpdateDropTarget();
  UpdateAutoScroll();
}

void TabsView::UpdateDropTarget() {
  if (!drag_ || !drag_->point) {
    return;
  }
  ShowDropTarget(drag_->over_content ? std::nullopt
                                     : DropTargetAt(*drag_->point));
  TrackSprungCard();
}

void TabsView::TrackSprungCard() {
  if (!drag_ || !drag_->point || !card_sprung_ || !card()) {
    card_leave_timer_.Stop();
    return;
  }
  // 卡片、挂着它的图块（胶囊）和它们之间的空：从图块移到卡片上不算离开。
  gfx::Rect keep = CardBounds();
  if (PopoverHost* host = popover_host()) {
    if (const std::optional<CardAnchor> anchor = CardAnchorFor(card_id_)) {
      keep.Union(views::View::ConvertRectToTarget(host, this, anchor->rect));
    }
  }
  keep.Outset(kSprungCardSlop);
  if (keep.Contains(*drag_->point)) {
    card_leave_timer_.Stop();
    return;
  }
  if (!card_leave_timer_.IsRunning()) {
    card_leave_timer_.Start(
        FROM_HERE,
        base::Milliseconds(static_cast<int>(motion::kFolderSpringLeave * 1000)),
        this, &TabsView::CloseSprungCard);
  }
}

void TabsView::CloseSprungCard() {
  if (drag_ && card_sprung_) {
    CloseCard();
  }
}

void TabsView::LiftFolder(const std::string& id) {
  for (TabListView* list : Lists()) {
    const auto& rows = list->rows();
    std::optional<int> depth;
    for (const TabListView::Entry& entry : list->entries()) {
      if (depth) {
        if (entry.depth <= *depth) {
          break;
        }
      } else if (entry.row && rows[entry.index]->data().id == id) {
        depth = entry.depth;
      } else {
        continue;
      }
      if (entry.row) {
        rows[entry.index]->SetLifted(true);
      } else if (TabCell* cell = list->CellAt(entry.index)) {
        cell->SetLifted(true);
      }
    }
  }
  // 顶部标签行：挂在这枚胶囊后面的当前标签也变淡。
  if (pins_) {
    for (size_t i = 0; i < pins_->count(); ++i) {
      TabCell* cell = pins_->CellAt(i);
      auto shown = shown_tabs_.find(cell->handle());
      if (shown != shown_tabs_.end() && shown->second.kept &&
          shown->second.under == id) {
        cell->SetLifted(true);
      }
    }
  }
}

void TabsView::TabCellContextMenu(TabCell* cell,
                                  const gfx::Point& screen_point,
                                  ui::mojom::MenuSourceType source) {
  tabs::TabInterface* tab = TabFor(cell);
  if (!tab) {
    return;
  }
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  context_menu_ = std::make_unique<TabMenu>(browser_, tab);
  gfx::Point point = screen_point;
  views::View::ConvertPointFromScreen(view, &point);
  view->ShowMenu(context_menu_->model(), point, base::DoNothing());
}

pin_folders::Child TabsView::DraggedItem() const {
  pin_folders::Child item;
  if (drag_) {
    item.tab = drag_->handle;
    item.folder = drag_->tile.empty() ? drag_->folder : drag_->tile;
  }
  return item;
}

bool TabsView::DropChangesOrder(const DropTarget& target) const {
  if (!drag_) {
    return true;
  }
  if (target.merge != tabs::TabHandle()) {
    return true;
  }
  if (target.pinned) {
    // 常用：已经在那一层的这个位置上（在停着的那个图块文件夹里）就是没变。
    const pin_folders::Child item = DraggedItem();
    std::optional<std::string> current;
    for (const auto& [level, children] : fav_levels_) {
      if (std::ranges::find(children, item) != children.end()) {
        current = level;
      }
    }
    if (!current) {
      return true;
    }
    if (!target.onto.empty() || *current != target.folder) {
      return *current != target.folder;
    }
    if (target.before == item) {
      return false;
    }
    const std::vector<pin_folders::Child>& siblings = fav_levels_.at(*current);
    auto at = std::ranges::find(siblings, item);
    return at + 1 == siblings.end() ? !target.before.empty()
                                    : target.before != *(at + 1);
  }
  // 图块文件夹只能在常用图块里排序。
  if (!drag_->tile.empty()) {
    return false;
  }
  if (target.ordered) {
    // 固定区的树：已经在这一层的这个位置上就是没变。
    const pin_folders::Child item = DraggedItem();
    if (target.before == item) {
      return false;
    }
    auto level = levels_.find(target.folder);
    if (level == levels_.end()) {
      return true;
    }
    const std::vector<pin_folders::Child>& siblings = level->second;
    auto at = std::ranges::find(siblings, item);
    if (at == siblings.end()) {
      return true;
    }
    return at + 1 == siblings.end() ? !target.before.empty()
                                    : target.before != *(at + 1);
  }
  // 文件夹只能放在侧栏的固定区里。
  if (!drag_->folder.empty()) {
    return false;
  }
  if (drag_->from_pinned) {
    return true;
  }
  auto* tab = drag_->handle.Get();
  if (!tab) {
    return false;
  }
  if (target.tree && !tab->IsPinned()) {
    // 树：父标签和后面紧挨着的那一个都没变就是没变。
    const int index = model_->GetIndexOfTab(tab);
    const std::vector<tab_tree::Node> nodes = tab_tree::Compute(model_);
    const int end = tab_tree::BranchEnd(model_, index);
    const tabs::TabHandle next = end < model_->count()
                                     ? model_->GetTabAtIndex(end)->GetHandle()
                                     : tabs::TabHandle();
    const int parent = nodes[index].parent;
    const tabs::TabHandle up = parent >= 0
                                   ? model_->GetTabAtIndex(parent)->GetHandle()
                                   : tabs::TabHandle();
    return target.tree_before != next || target.tree_parent != up;
  }
  if (tab->IsPinned() && !pinned_tabs::IsFavorite(tab)) {
    return true;
  }
  auto source = list_->IndexOf(drag_->handle);
  if (!source) {
    return true;
  }
  // 树里拖的是整枝：放在它自己这一枝里面就是没变。
  size_t span = 1;
  if (TreeShown() && !tab->IsPinned()) {
    const int index = model_->GetIndexOfTab(tab);
    const int end = tab_tree::BranchEnd(model_, index);
    for (int i = index + 1; i < end; ++i) {
      span += list_->CellFor(model_->GetTabAtIndex(i)->GetHandle()) ? 1 : 0;
    }
  }
  return target.index < *source || target.index > *source + span;
}

void TabsView::ShowDropTarget(std::optional<DropTarget> target) {
  if (drag_) {
    drag_->target = target;
  }
  const bool changes = target && DropChangesOrder(*target);
  TabListView* zone = changes ? ListFor(target->area) : nullptr;
  std::optional<TabListView::DropMarker> line;
  std::string onto;
  std::string spring;
  tabs::TabHandle merge;
  if (zone) {
    if (!target->onto.empty()) {
      onto = target->onto;
      spring = target->spring;
    } else if (target->merge != tabs::TabHandle()) {
      merge = target->merge;
    } else if (target->tree_into != tabs::TabHandle()) {
      // 树：放进这个标签当子标签，它描边；折着的停一会儿就展开。
      merge = target->tree_into;
      spring = target->spring;
    } else {
      line = TabListView::DropMarker{target->index};
      if (target->area == DropTarget::Area::kPins) {
        line->x = target->line_x;
      } else if (target->ordered) {
        line->y = target->line_y;
        line->depth = target->line_depth;
      } else if (target->tree) {
        line->tree_depth = target->line_depth;
        line->tree_min = target->line_min;
        line->tree_max = target->line_max;
      }
    }
  }
  for (TabListView* list :
       {pinned_.get(), list_.get(), pins_.get(), CardList()}) {
    if (list) {
      list->SetDropMarker(list == zone ? line : std::nullopt);
      list->SetDropRow(list == zone ? onto : std::string());
      list->SetDropCell(list == zone ? merge : tabs::TabHandle());
    }
  }
  if (spring != spring_id_) {
    spring_id_ = spring;
    spring_timer_.Stop();
    if (!spring.empty()) {
      spring_timer_.Start(FROM_HERE,
                          base::Milliseconds(static_cast<int>(
                              motion::kFolderSpringLoad * 1000)),
                          this, &TabsView::SpringOpen);
    }
  }
}

PopoverHost* TabsView::popover_host() const {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  return view && view->has_starry_chrome() ? view->popover_host() : nullptr;
}

PinFolderCard* TabsView::card() const {
  return static_cast<PinFolderCard*>(card_tracker_.view());
}

TabListView* TabsView::CardList() const {
  PinFolderCard* card = this->card();
  return card ? card->list() : nullptr;
}

std::string TabsView::OpenCardId() const {
  return card() ? card_id_ : std::string();
}

bool TabsView::IsCardOpen(const std::string& id) const {
  return card() && card_id_ == id;
}

gfx::Rect TabsView::CardBounds() const {
  PinFolderCard* card = this->card();
  return card ? views::View::ConvertRectToTarget(card, this,
                                                 card->GetLocalBounds())
              : gfx::Rect();
}

std::optional<TabsView::CardAnchor> TabsView::CardAnchorFor(
    const std::string& id) const {
  PopoverHost* host = popover_host();
  TabListRow* tile = TileFor(id);
  if (!host || !tile) {
    return std::nullopt;
  }
  CardAnchor anchor;
  // 用图块不带变换的位置（刚拖成的图块可能还在放大入场）。
  const gfx::Rect bounds = tile->bounds();
  gfx::Rect tile_rect = tile->TileRect();
  tile_rect.Offset(bounds.OffsetFromOrigin());
  anchor.grow = PopoverHost::Grow{
      views::View::ConvertRectToTarget(pinned_.get(), host, tile_rect),
      radius::kRow};
  if (list_->horizontal()) {
    anchor.rect = views::View::ConvertRectToTarget(pinned_.get(), host, bounds);
    return anchor;
  }
  // 侧栏：和常用图块一样宽、左边对齐，挂在这一排图块（连名字）下面。
  gfx::Rect rect = bounds;
  rect.set_x(0);
  rect.set_width(pinned_->width());
  anchor.rect = views::View::ConvertRectToTarget(pinned_.get(), host, rect);
  anchor.width = pinned_->width();
  return anchor;
}

void TabsView::ToggleCard(const std::string& id) {
  if (IsCardOpen(id)) {
    CloseCard();
    return;
  }
  if (id == card_closed_id_ &&
      base::TimeTicks::Now() - card_closed_at_ < kReopenGuard) {
    return;
  }
  OpenCard(id, /*sprung=*/false);
}

void TabsView::OpenCard(const std::string& id, bool sprung) {
  if (IsCardOpen(id)) {
    return;
  }
  PopoverHost* host = popover_host();
  const std::optional<CardAnchor> anchor = CardAnchorFor(id);
  if (!host || !anchor) {
    return;
  }
  // 换一个文件夹：前一张直接换掉（PopoverHost 一次只有一张）。
  if (PinFolderCard* old = card()) {
    old->Detach();
    card_tracker_.SetView(nullptr);
  }
  auto card = std::make_unique<PinFolderCard>(this, id);
  PinFolderCard* raw = card.get();
  if (anchor->width > 0) {
    raw->SetCardWidth(anchor->width);
  }
  card_id_ = id;
  // 先填好内容，PopoverHost 按它的大小放。
  if (!FillCard(raw, TabListView::Change::kNone)) {
    card_id_.clear();
    return;
  }
  host->Show(std::move(card), anchor->rect, anchor->placement, anchor->grow);
  card_tracker_.SetView(raw);
  card_sprung_ = sprung;
  host->SetClosedCallback(base::BindOnce(
      &TabsView::OnCardClosed, weak_factory_.GetWeakPtr(), ++card_serial_));
  if (drag_) {
    PlacePill(/*above_card=*/true);
  }
  // 图块、胶囊显示「开着」；拖着东西时落点可能落进卡片里了。
  Sync(TabListView::Change::kNone);
}

void TabsView::CloseCard() {
  PopoverHost* host = popover_host();
  if (PinFolderCard* card = this->card();
      card && host && host->content() == card) {
    host->Close();
  }
}

void TabsView::OnCardClosed(int serial) {
  if (serial != card_serial_) {
    return;
  }
  if (PinFolderCard* card = this->card()) {
    card->Detach();
  }
  card_tracker_.SetView(nullptr);
  card_closed_id_ = std::exchange(card_id_, std::string());
  card_closed_at_ = base::TimeTicks::Now();
  card_sprung_ = false;
  // 图块、胶囊不再显示「开着」：不在收起卡片的调用里重排。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<TabsView> self) {
                       if (self) {
                         self->Sync(TabListView::Change::kNone);
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

bool TabsView::FillCard(PinFolderCard* card, TabListView::Change change) {
  std::u16string title;
  std::vector<TabListRowData> rows;
  std::vector<TabListView::Item> items;
  if (!BuildCard(card_id_, &title, &rows, &items)) {
    return false;
  }
  card->Update(title, std::move(rows), items, theme_, change);
  return true;
}

void TabsView::UpdateCard(TabListView::Change change) {
  PinFolderCard* card = this->card();
  if (!card) {
    return;
  }
  if (!FillCard(card, change)) {
    CloseCard();
    return;
  }
  if (PopoverHost* host = popover_host()) {
    host->ContentSizeChanged();
  }
}

bool TabsView::CardDropTargetAt(const gfx::Point& point,
                                std::optional<DropTarget>* target) const {
  PinFolderCard* card = this->card();
  if (!card || !CardBounds().Contains(point)) {
    return false;
  }
  gfx::Point local = point;
  views::View::ConvertPointToTarget(this, card->list(), &local);
  *target = CardDropTarget(card->id(), local);
  return true;
}

void TabsView::DragStateChanged(bool dragging) {
  // 拖着的时候不吸顶（落点按列表里的行算）。
  UpdateSticky();
  if (dragging) {
    // 小条在松手后可能还在弹出框那一层上面飞向文件夹：开始拖时再放好。
    PlacePill(/*above_card=*/card() != nullptr);
    return;
  }
  // 停在图块、胶囊上落下来的卡片：放完就收起来。
  if (card_sprung_) {
    CloseCard();
  }
}

void TabsView::PlacePill(bool above_card) {
  PopoverHost* popover = popover_host();
  views::View* target =
      above_card && popover && popover->parent() ? popover->parent() : this;
  views::View* parent = drag_pill_->parent();
  if (parent != target) {
    gfx::Point origin = drag_pill_->origin();
    views::View::ConvertPointToTarget(parent, target, &origin);
    std::unique_ptr<TabDragPill> pill =
        parent->RemoveChildViewT(drag_pill_.get());
    drag_pill_ = target->AddChildView(std::move(pill));
    drag_pill_->SetPosition(origin);
  }
  if (target == this) {
    ReorderChildView(drag_pill_, children().size());
    return;
  }
  // 紧跟在弹出框那一层后面（菜单、对话框还在它上面）。
  const size_t popover_index = *target->GetIndexOf(popover);
  const size_t pill_index = *target->GetIndexOf(drag_pill_);
  target->ReorderChildView(drag_pill_, pill_index < popover_index
                                           ? popover_index
                                           : popover_index + 1);
}

gfx::Point TabsView::ToPillParent(const gfx::Point& point) const {
  gfx::Point converted = point;
  if (drag_pill_->parent() && drag_pill_->parent() != this) {
    views::View::ConvertPointToTarget(this, drag_pill_->parent(), &converted);
  }
  return converted;
}

void TabsView::VisibilityChanged(views::View* starting_from, bool is_visible) {
  views::View::VisibilityChanged(starting_from, is_visible);
  // 换了布局、侧栏收起来了：卡片收起来。
  if (!is_visible) {
    CloseCard();
  }
}

int TabsView::PinStripLimit() const {
  return 0;
}

void TabsView::SpringOpen() {
  const std::string id = std::exchange(spring_id_, std::string());
  if (!id.empty() && CardAnchorFor(id)) {
    OpenCard(id, /*sprung=*/true);
    return;
  }
  // 设置一改列表就重排，落点跟着重新算（Sync 的末尾）。
  Profile* profile = browser_->GetProfile();
  if (id.starts_with(kTreeSpringPrefix)) {
    // 树里折着的父标签：展开（树变了列表就重排）。
    for (int i = 0; i < model_->count(); ++i) {
      if (kTreeSpringPrefix +
              base::NumberToString(
                  model_->GetTabAtIndex(i)->GetHandle().raw_value()) ==
          id) {
        tab_tree::SetCollapsed(model_, i, /*collapsed=*/false, /*all=*/false);
        break;
      }
    }
    return;
  }
  if (id == "@pins") {
    profile->GetPrefs()->SetBoolean(prefs::kPinsCollapsed, false);
  } else if (!id.empty()) {
    pin_folders::SetCollapsed(profile, id, false);
  }
}

void TabsView::UpdateAutoScroll() {
  int step = 0;
  if (drag_ && drag_->point && !drag_->over_content && !list_->horizontal()) {
    const gfx::Rect view = scroll_->bounds();
    const gfx::Point& point = *drag_->point;
    if (point.x() >= view.x() && point.x() <= view.right()) {
      const int top = point.y() - view.y();
      const int bottom = view.bottom() - point.y();
      if (top >= 0 && top < kAutoScrollEdge) {
        step = -(1 + kAutoScrollMaxStep * (kAutoScrollEdge - top) /
                         kAutoScrollEdge);
      } else if (bottom < kAutoScrollEdge && bottom > -kAutoScrollEdge) {
        step = 1 + kAutoScrollMaxStep *
                       std::min(kAutoScrollEdge, kAutoScrollEdge - bottom) /
                       kAutoScrollEdge;
      }
    }
  }
  autoscroll_step_ = step;
  if (step == 0) {
    autoscroll_timer_.Stop();
  } else if (!autoscroll_timer_.IsRunning()) {
    autoscroll_timer_.Start(FROM_HERE, base::Milliseconds(kAutoScrollFrameMs),
                            this, &TabsView::AutoScrollStep);
  }
}

void TabsView::AutoScrollStep() {
  if (!drag_) {
    autoscroll_timer_.Stop();
    return;
  }
  const gfx::Rect visible = scroll_->GetVisibleRect();
  const int limit = std::max(0, list_->height() - visible.height());
  const int next = std::clamp(visible.y() + autoscroll_step_, 0, limit);
  if (next == visible.y()) {
    return;
  }
  scroll_->ScrollToOffset(gfx::PointF(0, next));
  UpdateDropTarget();
}

void TabsView::FinishDrag(bool cancelled) {
  if (!drag_) {
    return;
  }
  const Drag drag = *drag_;
  const bool changes =
      !cancelled && drag.target && DropChangesOrder(*drag.target);
  ShowDropTarget(std::nullopt);
  drag_.reset();
  autoscroll_timer_.Stop();
  card_leave_timer_.Stop();
  InvalidateLayout();
  for (TabListView* list : Lists()) {
    for (size_t i = 0; i < list->count(); ++i) {
      list->CellAt(i)->SetLifted(false);
    }
    for (TabListRow* row : list->rows()) {
      row->SetLifted(false);
    }
  }
  // 放进了文件夹：小条缩进文件夹的图标里。
  bool flew = false;
  TabListView* zone = changes && !drag.target->onto.empty()
                          ? ListFor(drag.target->area)
                          : nullptr;
  if (zone) {
    for (TabListRow* row : zone->rows()) {
      if (row->data().id == drag.target->onto &&
          (row->data().kind == TabListRowData::Kind::kFolder ||
           row->data().kind == TabListRowData::Kind::kChip ||
           row->data().kind == TabListRowData::Kind::kTile)) {
        gfx::Point icon = row->IconCenter();
        views::View::ConvertPointToTarget(row, drag_pill_->parent(), &icon);
        drag_pill_->FlyTo(icon);
        flew = true;
        break;
      }
    }
  }
  // 树里放进折着的父标签：小条缩进它的图标里（放下的那一枝藏进了折叠里）。
  if (changes && !drag.target->spring.empty() &&
      drag.target->tree_into != tabs::TabHandle()) {
    if (TabCell* cell = list_->CellFor(drag.target->tree_into)) {
      gfx::Point icon = cell->IconCenter();
      views::View::ConvertPointToTarget(cell, drag_pill_->parent(), &icon);
      drag_pill_->FlyTo(icon);
      flew = true;
    }
  }
  // 和一个常用标签合成文件夹：小条缩进那个图块里。
  if (changes && drag.target->merge != tabs::TabHandle()) {
    if (TabListView* list = ListFor(drag.target->area)) {
      if (TabCell* cell = list->CellFor(drag.target->merge)) {
        gfx::Point icon = cell->GetLocalBounds().CenterPoint();
        views::View::ConvertPointToTarget(cell, drag_pill_->parent(), &icon);
        drag_pill_->FlyTo(icon);
        flew = true;
      }
    }
  }
  if (!flew) {
    drag_pill_->Hide();
  }
  // 位置、排序值、文件夹一起改，列表只重排一次；侧栏里为拖动露出的落点也收回去。
  BeginBatch();
  batch_dirty_ |= !list_->horizontal();
  Drop(drag, cancelled, changes);
  // ⌥ 拖一个没挤地方：子标签挂回去。
  if (drag.detached && !changes) {
    tab_tree::ReattachChildren(model_, *drag.detached);
  }
  EndBatch(TabListView::Change::kInsert);
  if (changes) {
    for (TabListView* list : Lists()) {
      list->SnapInPlace(drag.handle,
                        drag.tile.empty() ? drag.folder : drag.tile);
    }
  }
  DragStateChanged(false);
}

void TabsView::Drop(const Drag& drag, bool cancelled, bool changes) {
  if (drag.folder.empty() && drag.tile.empty()) {
    if (SplitOverlay* overlay = SplitOverlayFor(browser_)) {
      // 在内容区上松手：按预览分屏。
      const bool split = overlay->EndTabDrag(!cancelled && drag.over_content);
      if (split || drag.over_content) {
        return;
      }
    }
  }
  if (!changes) {
    return;
  }
  const DropTarget& target = *drag.target;
  if (!drag.tile.empty()) {
    if (target.pinned && target.area == DropTarget::Area::kGrid) {
      favorite_folders::Place(browser_,
                              pin_folders::Child{tabs::TabHandle(), drag.tile},
                              std::string(), target.before);
    }
    return;
  }
  if (!drag.folder.empty()) {
    if (target.ordered) {
      pin_folders::Place(browser_,
                         pin_folders::Child{tabs::TabHandle(), drag.folder},
                         target.folder, target.before);
    }
    return;
  }
  tabs::TabInterface* tab = drag.handle.Get();
  if (!tab) {
    return;
  }

  if (target.merge != tabs::TabHandle()) {
    // 和图块那一层的一个常用标签合成新的图块文件夹：卡片从新的图块长出来，
    // 上面的名字等着改（等这次拖动收完尾再开）。
    if (tab->GetSplit() && !drag.from_pinned) {
      split::RemoveFromSplit(browser_, tab);
    }
    const std::string id =
        favorite_folders::Merge(browser_, target.merge, tab->GetHandle());
    if (id.empty()) {
      return;
    }
    if (tab->IsPinned()) {
      MoveToFavoriteOrder(tab);
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<TabsView> self, std::string id) {
                         if (!self) {
                           return;
                         }
                         self->OpenCard(id, /*sprung=*/false);
                         self->BeginCardRename(id);
                       },
                       weak_factory_.GetWeakPtr(), id));
    return;
  }
  if (target.pinned) {
    // 常用图块、图块文件夹（停在图块上、卡片里）：放进那一层的那个位置，那一
    // 层重新编排序值。从别处拖来的分屏里的标签移出分屏。
    if (tab->GetSplit() && !drag.from_pinned) {
      split::RemoveFromSplit(browser_, tab);
    }
    favorite_folders::Place(browser_,
                            pin_folders::Child{tab->GetHandle(), std::string()},
                            target.folder, target.before);
    if (tab->IsPinned()) {
      MoveToFavoriteOrder(tab);
    }
    return;
  }

  int index = model_->GetIndexOfTab(tab);
  // 侧栏的列表、顶部的普通标签：排在显示着的第 index 个之前。
  TabListView* list =
      target.area == DropTarget::Area::kList ? list_.get() : nullptr;
  auto* anchor = list ? list->TabAt(target.index) : nullptr;
  const auto anchor_handle = anchor ? anchor->GetHandle() : tabs::TabHandle();
  int final_index = anchor ? model_->GetIndexOfTab(anchor) : model_->count();
  if (final_index > index) {
    --final_index;
  }
  // 分屏里的标签拖到了别处（离开自己那一组）：移出分屏。
  if (const std::optional<split_tabs::SplitTabId> split = tab->GetSplit()) {
    const bool within_split =
        list && !drag.from_pinned && model_->ContainsIndex(final_index) &&
        model_->GetTabAtIndex(final_index)->GetSplit() == split;
    if (!within_split) {
      split::RemoveFromSplit(browser_, tab);
      index = model_->GetIndexOfTab(tab);
    }
  }
  // 用户自己挪的：记住放进了哪个文件夹、从哪个文件夹拿出来（整理的习惯）。
  const std::string from_folder =
      tab->IsPinned() && !pinned_tabs::IsFavorite(tab)
          ? pinned_tabs::Folder(tab)
          : std::string();
  if (target.ordered) {
    // 固定区的树：放进那一层的那个位置，那一层重新编排序值。
    pin_folders::Place(browser_,
                       pin_folders::Child{tab->GetHandle(), std::string()},
                       target.folder, target.before);
    // 空白标签没有网址可记，固定不了。
    if (tab->IsPinned()) {
      tab_organizer::NoteManualMove(tab, from_folder, target.folder);
      MoveToTreeOrder(tab);
    }
    return;
  }
  if (tab->IsPinned()) {
    tab_organizer::NoteManualMove(tab, from_folder, std::string());
    model_->SetTabPinned(index, false);
  }
  // 模型里也挪到显示在它后面的那个标签之前（⌘1…9、切到下一个标签按模型的顺序）。
  index = model_->GetIndexOfTab(tab);
  auto* next = anchor_handle.Get();
  if (TreeShown() && list && target.tree) {
    // 树：整枝挪到 tree_before 前面，挂到 tree_parent 下（空的是最外层）。
    tabs::TabInterface* before = target.tree_before.Get();
    tabs::TabInterface* parent = target.tree_parent.Get();
    if (target.tree_parent != tabs::TabHandle() && !parent) {
      return;
    }
    const int to = before ? model_->GetIndexOfTab(before) : model_->count();
    tab_tree::MoveBranch(model_, index,
                         std::max(to, model_->IndexOfFirstNonPinnedTab()),
                         parent ? model_->GetIndexOfTab(parent) : -1);
    return;
  }
  if (TreeShown() && list) {
    // 树里：整枝一起挪，放下以后和下面那一行同级。
    const int to = next ? model_->GetIndexOfTab(next) : model_->count();
    tab_tree::MoveBranch(model_, index,
                         std::max(to, model_->IndexOfFirstNonPinnedTab()));
    return;
  }
  final_index = next ? model_->GetIndexOfTab(next) : model_->count();
  if (final_index > index) {
    --final_index;
  }
  final_index = std::max(final_index, model_->IndexOfFirstNonPinnedTab());
  if (index != final_index) {
    model_->MoveWebContentsAt(index, final_index, /*select_after_move=*/false);
  }
}

void TabsView::MoveToFavoriteOrder(tabs::TabInterface* tab) {
  // 常用图块按图块文件夹展开排成一列，挪到排在它后面的第一个常用标签之前。
  const pin_folders::Levels levels = favorite_folders::Layout(browser_);
  std::vector<tabs::TabHandle> order;
  if (auto grid = levels.find(std::string()); grid != levels.end()) {
    for (const pin_folders::Child& child : grid->second) {
      if (!child.is_folder()) {
        order.push_back(child.tab);
        continue;
      }
      if (auto inside = levels.find(child.folder); inside != levels.end()) {
        for (const pin_folders::Child& kid : inside->second) {
          order.push_back(kid.tab);
        }
      }
    }
  }
  auto it = std::ranges::find(order, tab->GetHandle());
  if (it == order.end()) {
    return;
  }
  tabs::TabInterface* next = nullptr;
  for (++it; it != order.end() && !next; ++it) {
    next = it->Get();
  }
  if (!next) {
    return;
  }
  const int index = model_->GetIndexOfTab(tab);
  const int boundary = model_->IndexOfFirstNonPinnedTab();
  int final_index = model_->GetIndexOfTab(next);
  if (final_index > index) {
    --final_index;
  }
  final_index = std::clamp(final_index, 0, std::max(0, boundary - 1));
  if (index != final_index) {
    model_->MoveWebContentsAt(index, final_index, /*select_after_move=*/false);
  }
}

std::optional<TabsView::DropTarget> TabsView::ListDropTarget(
    const gfx::Point& point) const {
  using Kind = TabListRowData::Kind;
  const std::vector<TabListView::Entry>& entries = list_->entries();
  const std::vector<TabListRowData>& rows = list_->row_data();
  // 固定区到分隔线为止（线的上半截还算固定区的末尾）；再往下是普通标签。
  std::optional<size_t> divider;
  for (size_t i = 0; i < entries.size() && !divider; ++i) {
    if (entries[i].row && rows[entries[i].index].kind == Kind::kDivider) {
      divider = i;
    }
  }
  if (!divider || *divider == 0 ||
      point.y() >= entries[*divider].y + entries[*divider].height / 2) {
    if (drag_ && !drag_->folder.empty()) {
      return std::nullopt;
    }
    // 放在分隔线下面：模型里排在固定区所有标签的后面。
    size_t pinned_tabs = 0;
    for (size_t k = 0; divider && k <= *divider; ++k) {
      pinned_tabs += entries[k].row ? 0 : 1;
    }
    if (TreeShown() && drag_ && drag_->handle.Get()) {
      return TreeListDropTarget(point, pinned_tabs);
    }
    DropTarget target;
    target.index = std::max(pinned_tabs, list_->DropIndexAt(point));
    return target;
  }
  return TreeDropTarget(list_, point, *divider, std::string(),
                        pin_folders::Child(), shown_tabs_,
                        DropTarget::Area::kList);
}

std::optional<TabsView::DropTarget> TabsView::TreeListDropTarget(
    const gfx::Point& point,
    size_t first) const {
  const int count = static_cast<int>(list_->count());
  const int begin_row = static_cast<int>(first);
  if (begin_row >= count) {
    DropTarget target;
    target.index = first;
    return target;
  }
  const std::vector<tab_tree::Node> nodes = tab_tree::Compute(model_);
  // 列表里第 i 个标签在模型里的下标。
  auto model_index = [&](int i) {
    tabs::TabInterface* tab = list_->TabAt(static_cast<size_t>(i));
    return tab ? model_->GetIndexOfTab(tab) : TabStripModel::kNoTab;
  };
  // 拖着的那一枝在模型里是 [begin, end)；从固定区拖来的是空的。
  int begin = -1;
  int end = -1;
  tabs::TabInterface* dragged_tab = drag_ ? drag_->handle.Get() : nullptr;
  if (dragged_tab && !dragged_tab->IsPinned()) {
    begin = model_->GetIndexOfTab(dragged_tab);
    end = tab_tree::BranchEnd(model_, begin);
  }
  auto dragged = [&](int m) { return m >= begin && m < end; };
  // 放下以后最深在第几层（层数上限）。
  const int limit = tab_tree::MaxDepth(browser_->GetProfile());
  const int max_depth = limit > 0 ? limit - 1 : std::numeric_limits<int>::max();
  auto handle_at = [&](int m) { return model_->GetTabAtIndex(m)->GetHandle(); };
  // 聚焦一组时：列表里的第 0 层是模型里的第 base 层（聚焦的那个标签的子标签），
  // 最外面一层和过了最后一行都还在这一组里。
  const int hoisted = HoistedIndex();
  const int base = hoisted >= 0 ? nodes[hoisted].depth + 1 : 0;
  const int hoist_end =
      hoisted >= 0 ? tab_tree::BranchEnd(model_, hoisted) : -1;

  // 放在第 line 行上面的那条缝里（line 是 count 时是最后一行下面）。上下两行跳过
  // 拖着的那一枝。层级从「下面那一行的层级」到「上面那一行的层级 + 1」（折着的
  // 是同一层），按小条的左边选，每层至少移 kTreeDragStep；shallow 时取最外面的。
  auto gap = [&](int line, bool shallow) {
    int upper = line - 1;
    while (upper >= begin_row && dragged(model_index(upper))) {
      --upper;
    }
    int lower = line;
    while (lower < count && dragged(model_index(lower))) {
      ++lower;
    }
    const int up = upper >= begin_row ? model_index(upper) : -1;
    const int down = lower < count ? model_index(lower) : -1;
    int deepest = base;
    if (up >= 0) {
      deepest = nodes[up].collapsed ? nodes[up].depth : nodes[up].depth + 1;
    }
    const int shallowest = down >= 0 ? nodes[down].depth : base;
    deepest = std::max(shallowest, std::min(deepest, max_depth));
    int depth = shallowest;
    if (!shallow) {
      const int step = std::max(list_->TreeStep(list_->width()), kTreeDragStep);
      const int left = point.x() - (drag_ ? drag_->grab_x : 0);
      depth = std::clamp((left + step / 2) / step + base, shallowest, deepest);
    }
    DropTarget target;
    target.tree = true;
    target.index = static_cast<size_t>(line);
    target.line_depth = depth - base;
    target.line_min = shallowest - base;
    target.line_max = shallow ? shallowest - base : deepest - base;
    // 新的父标签：上面那一行（或者它的祖先）里在第 depth - 1 层的那个；聚焦一组时
    // 最上面的缝是聚焦的那个标签。
    int parent = up >= 0 ? up : hoisted;
    while (parent >= 0 && nodes[parent].depth > depth - 1) {
      parent = nodes[parent].parent;
    }
    if (depth > 0 && parent >= 0) {
      target.tree_parent = handle_at(parent);
    }
    if (down >= 0) {
      target.tree_before = handle_at(down);
    } else if (hoist_end >= 0 && hoist_end < model_->count()) {
      target.tree_before = handle_at(hoist_end);
    }
    return target;
  };
  // 放进第 i 行当它的最后一个子标签；到了层数上限不行。
  auto into = [&](int i) -> std::optional<DropTarget> {
    const int m = model_index(i);
    if (nodes[m].depth + 1 > max_depth) {
      return std::nullopt;
    }
    DropTarget target;
    target.tree = true;
    target.index = static_cast<size_t>(i);
    target.line_depth = nodes[m].depth + 1 - base;
    target.tree_into = target.tree_parent = handle_at(m);
    if (nodes[m].collapsed) {
      target.spring = kTreeSpringPrefix +
                      base::NumberToString(target.tree_into.raw_value());
    }
    // 排在它那一枝的后面（拖着的那一枝不算）。
    int after = m + 1;
    while (after < model_->count() && nodes[after].depth > nodes[m].depth) {
      ++after;
    }
    if (dragged(after)) {
      after = end;
    }
    if (after < model_->count()) {
      target.tree_before = handle_at(after);
    }
    return target;
  };

  // 鼠标在哪一行、在这一行里从上到下的几分之几。
  int at = begin_row;
  for (int i = begin_row; i < count; ++i) {
    if (list_->SlotRect(i).y() <= point.y()) {
      at = i;
    }
  }
  const gfx::Rect row = list_->SlotRect(at);
  const float where =
      row.height() > 0 ? static_cast<float>(point.y() - row.y()) / row.height()
                       : 0;
  // 过了最后一行：放在最外层的最后。
  if (at + 1 == count && point.y() > row.bottom() + row.height() / 2) {
    return gap(count, /*shallow=*/true);
  }
  // 在拖着的那一枝上：还在原处，左右移是换层级。
  if (dragged(model_index(at))) {
    return gap(static_cast<int>(list_->IndexOf(drag_->handle).value_or(at)),
               false);
  }
  if (where < 0.25f) {
    return gap(at, false);
  }
  if (where > 0.75f) {
    return gap(at + 1, false);
  }
  if (std::optional<DropTarget> target = into(at)) {
    return target;
  }
  return gap(where < 0.5f ? at : at + 1, false);
}

std::optional<TabsView::DropTarget> TabsView::CardDropTarget(
    const std::string& root,
    const gfx::Point& point) const {
  TabListView* list = CardList();
  if (!list || (drag_ && !drag_->tile.empty())) {
    return std::nullopt;
  }
  // 图块文件夹的卡片：一列常用标签，排在鼠标下面那一个之前；空的文件夹放在
  // 「拖入标签页」上。
  if (fav_folders_.contains(root)) {
    tabs::TabInterface* tab = drag_ ? drag_->handle.Get() : nullptr;
    if (!is_dragging_tab() || !tab) {
      return std::nullopt;
    }
    const bool inside = pinned_tabs::IsFavorite(tab) &&
                        pinned_tabs::FavoriteFolder(tab) == root;
    if (!inside && !pinned_tabs::HasRoomForFavorite(browser_, root)) {
      return std::nullopt;
    }
    DropTarget target;
    target.pinned = true;
    target.area = DropTarget::Area::kCard;
    target.folder = root;
    if (list->count() == 0) {
      target.onto = root + "/empty";
      return target;
    }
    target.index = list->DropIndexAt(point);
    if (tabs::TabInterface* next = list->TabAt(target.index)) {
      target.before = pin_folders::Child{next->GetHandle(), std::string()};
    }
    return target;
  }
  // 「…」的卡片是最外层收不下的那些。
  const bool more = root == kMorePinsId;
  pin_folders::Child first;
  if (more) {
    if (!overflow_.empty()) {
      first = overflow_.front();
    }
  } else if (auto level = levels_.find(root);
             level != levels_.end() && !level->second.empty()) {
    first = level->second.front();
  }
  return TreeDropTarget(list, point, list->entries().size(),
                        more ? std::string() : root, first, card_shown_,
                        DropTarget::Area::kCard);
}

bool TabsView::InsideDragged(std::string id) const {
  const bool folder_drag = drag_ && !drag_->folder.empty();
  for (int guard = 0; folder_drag && !id.empty() && guard < kMaxFolderDepth;
       ++guard) {
    if (id == drag_->folder) {
      return true;
    }
    id = FolderParent(id);
  }
  return false;
}

std::optional<TabsView::DropTarget> TabsView::TreeDropTarget(
    const TabListView* list,
    const gfx::Point& point,
    size_t end,
    const std::string& root,
    const pin_folders::Child& first,
    const std::map<tabs::TabHandle, ShownTab>& shown_tabs,
    DropTarget::Area area) const {
  using Kind = TabListRowData::Kind;
  const std::vector<TabListView::Entry>& entries = list->entries();
  const std::vector<TabListRowData>& rows = list->row_data();
  end = std::min(end, entries.size());
  if (end == 0) {
    return std::nullopt;
  }
  auto row_at = [&](size_t i) -> const TabListRowData* {
    return entries[i].row ? &rows[entries[i].index] : nullptr;
  };
  // 到第 i 项为止有几个标签：放在它下面的东西，模型里排在这第几个标签之前。
  auto tabs_through = [&](size_t i) {
    size_t count = 0;
    for (size_t k = 0; k <= i && k < entries.size(); ++k) {
      count += entries[k].row ? 0 : 1;
    }
    return count;
  };

  // 鼠标所在的那一项，和在它里面从上到下的几分之几。
  size_t at = 0;
  for (size_t i = 0; i < end; ++i) {
    if (entries[i].y <= point.y()) {
      at = i;
    }
  }
  const TabListView::Entry& entry = entries[at];
  const float where =
      entry.height > 0 ? static_cast<float>(point.y() - entry.y) / entry.height
                       : 0;
  // 过了最后一项（侧栏里是到了分隔线跟前）：放在最外层的最后。
  const bool past_end = at + 1 == end && point.y() > entry.y + entry.height + 1;

  // 文件夹那一行下面显示着的东西到第几项为止。
  auto subtree_end = [&](size_t i) {
    size_t last = i;
    while (last + 1 < end && entries[last + 1].depth > entries[i].depth) {
      ++last;
    }
    return last;
  };
  // 放进 folder（空的是最外层）的最后，row 那一行描边；last 是它下面显示着的
  // 最后一项。
  auto into = [&](const std::string& folder, const TabListRowData& row,
                  size_t last) -> std::optional<DropTarget> {
    if (InsideDragged(folder)) {
      return std::nullopt;
    }
    DropTarget target;
    target.ordered = true;
    target.area = area;
    target.folder = folder;
    target.onto = row.id;
    if (row.collapsed) {
      target.spring = row.id;
    }
    target.index = tabs_through(last);
    return target;
  };
  // 放在第 line 项下面的那条缝里。同一条缝可以是几个层级（一个文件夹的最后一行
  // 下面：还在文件夹里，或者在它外面的哪一层），按鼠标的左右选，shallow 时取最
  // 外面的。
  auto gap = [&](size_t line, bool shallow) -> std::optional<DropTarget> {
    size_t above = line;
    const TabListRowData* row = row_at(above);
    // 空文件夹下面的落点算在文件夹上。
    if (row && row->kind == Kind::kEmpty && above > 0) {
      --above;
      row = row_at(above);
    }
    const TabListView::Entry& upper = entries[above];
    const bool folder_row = row && row->kind == Kind::kFolder;
    tabs::TabHandle handle;
    ShownTab shown;
    if (!row) {
      if (tabs::TabInterface* tab = list->TabAt(upper.index)) {
        handle = tab->GetHandle();
      }
      if (auto found = shown_tabs.find(handle); found != shown_tabs.end()) {
        shown = found->second;
      }
    }
    // 最深：展开的文件夹下面是它的第一项；折叠的文件夹、标签下面是同一层
    // （留在折叠的文件夹下面的当前标签算文件夹那一层）。最浅：下面那一项的层级。
    int deepest = 0;
    if (folder_row) {
      deepest = row->collapsed ? upper.depth : upper.depth + 1;
    } else if (!row) {
      deepest = shown.kept ? upper.depth - 1 : upper.depth;
    }
    const int shallowest = line + 1 < end ? entries[line + 1].depth : 0;
    if (shallowest > deepest) {
      // 折叠的文件夹和留在它下面的当前标签之间：放进这个文件夹。
      return folder_row ? into(row->id, *row, subtree_end(above))
                        : std::nullopt;
    }
    const int depth = shallow ? shallowest
                              : std::clamp(point.x() / TabListRow::IndentFor(1),
                                           shallowest, deepest);

    DropTarget target;
    target.ordered = true;
    target.area = area;
    target.line_y = entries[line].y + entries[line].height + 1;
    target.line_depth = depth;
    target.index = tabs_through(line);
    // 这条缝上面、在 depth 这一层的那一项：新来的排在它后面。小标题下面、
    // 展开的文件夹下面是那一层的第一个。
    pin_folders::Child node;
    bool head = false;
    if (row && row->kind == Kind::kLabel) {
      head = true;
    } else if (folder_row && depth == upper.depth + 1) {
      target.folder = row->id;
      head = true;
    } else {
      int level = upper.depth;
      std::string parent;
      if (folder_row) {
        node.folder = row->id;
        parent = FolderParent(row->id);
      } else if (shown.kept) {
        node.folder = shown.under;
        parent = FolderParent(shown.under);
        --level;
      } else {
        node.tab = handle;
        parent = shown.under;
      }
      for (; level > depth && !parent.empty(); --level) {
        node = pin_folders::Child{tabs::TabHandle(), parent};
        parent = FolderParent(parent);
      }
      target.folder = parent;
    }
    if (InsideDragged(target.folder)) {
      return std::nullopt;
    }
    if (auto level = levels_.find(target.folder); level != levels_.end()) {
      const std::vector<pin_folders::Child>& siblings = level->second;
      if (head) {
        if (!siblings.empty()) {
          target.before = siblings.front();
        }
      } else if (auto found = std::ranges::find(siblings, node);
                 found != siblings.end() && found + 1 != siblings.end()) {
        target.before = *(found + 1);
      }
    }
    return target;
  };

  const TabListRowData* row = row_at(at);
  // 列表最上面没有小标题（卡片）：第一项的上半截是排在这一层的最前面。
  const bool top_edge =
      at == 0 &&
      !(row && (row->kind == Kind::kLabel || row->kind == Kind::kEmpty)) &&
      where < (row && row->kind == Kind::kFolder ? 0.25f : 0.5f);
  if (top_edge) {
    if (InsideDragged(root)) {
      return std::nullopt;
    }
    DropTarget target;
    target.ordered = true;
    target.area = area;
    target.folder = root;
    target.before = first;
    target.line_y = entries[0].y - 1;
    return target;
  }
  if (!row) {
    // 标签：上半截是排在它前面，下半截是排在它后面。
    return gap(where < 0.5f && at > 0 ? at - 1 : at, past_end);
  }
  switch (row->kind) {
    case Kind::kLabel:
      // 整个区折叠着：放在最外层的最后，停一会儿就展开。
      if (row->collapsed) {
        return into(std::string(), *row, end - 1);
      }
      return gap(at, past_end);
    case Kind::kEmpty: {
      // 落点的 ID 是「文件夹 ID/empty」，最外层的是 "@pins/empty"。
      std::string folder = row->id.substr(0, row->id.rfind('/'));
      if (folder == "@pins") {
        folder.clear();
      }
      return into(folder, *row, at);
    }
    case Kind::kFolder:
      // 文件夹：上面四分之一是排在它前面，下面四分之一是排在它后面（展开着的是
      // 放在里面的最前面），中间是放进去。
      if (where < 0.25f && at > 0) {
        return gap(at - 1, false);
      }
      if (where > 0.75f) {
        return gap(at, past_end);
      }
      return into(row->id, *row, subtree_end(at));
    case Kind::kDivider:
    case Kind::kNewTab:
    case Kind::kChip:
    case Kind::kTile:
    case Kind::kHoist:
      break;
  }
  return std::nullopt;
}

std::optional<TabsView::DropTarget> TabsView::GridDropTarget(
    const gfx::Point& point) const {
  // 固定区的文件夹不能放进常用图块。
  if (drag_ && !drag_->folder.empty()) {
    return std::nullopt;
  }
  const std::vector<TabListView::Entry>& entries = pinned_->entries();
  const std::vector<TabListRowData>& rows = pinned_->row_data();
  // 拖着的东西现在在常用的哪一层（不是常用的是空的）。
  const pin_folders::Child item = DraggedItem();
  std::optional<std::string> current;
  for (const auto& [level, children] : fav_levels_) {
    if (std::ranges::find(children, item) != children.end()) {
      current = level;
    }
  }
  auto node_of = [&](size_t slot) -> pin_folders::Child {
    const TabListView::Entry& entry = entries[slot];
    if (entry.row) {
      return pin_folders::Child{tabs::TabHandle(), rows[entry.index].id};
    }
    tabs::TabInterface* other = pinned_->TabAt(entry.index);
    return other ? pin_folders::Child{other->GetHandle(), std::string()}
                 : pin_folders::Child();
  };
  DropTarget target;
  target.pinned = true;
  target.area = DropTarget::Area::kGrid;
  // 图块那一层满了：只能放进还有空的图块文件夹。
  const bool room =
      current == std::string() || pinned_tabs::HasRoomForFavorite(browser_);
  if (entries.empty()) {
    return room ? std::optional<DropTarget>(target) : std::nullopt;
  }
  // 离鼠标最近的那一格。
  size_t at = 0;
  int best = -1;
  for (size_t i = 0; i < entries.size(); ++i) {
    const TabListView::Entry& entry = entries[i];
    const int dx =
        std::max({entry.x - point.x(), 0, point.x() - (entry.x + entry.width)});
    const int dy = std::max(
        {entry.y - point.y(), 0, point.y() - (entry.y + entry.height)});
    const int distance = dx * dx + dy * dy;
    if (best < 0 || distance < best) {
      best = distance;
      at = i;
    }
  }
  const TabListView::Entry& entry = entries[at];
  const float where =
      entry.width > 0 ? static_cast<float>(point.x() - entry.x) / entry.width
                      : 0;
  // 标签停在图块那一层的一个常用标签的中间：和它合成一个新的图块文件夹（它
  // 进了文件夹，图块那一层不会多出一格）。
  if (!entry.row && is_dragging_tab() && where >= 0.25f && where <= 0.75f &&
      point.y() >= entry.y && point.y() <= entry.y + entry.height) {
    tabs::TabInterface* other = pinned_->TabAt(entry.index);
    if (other && other->GetHandle() != drag_->handle) {
      target.merge = other->GetHandle();
      target.index = at;
      return target;
    }
  }
  // 标签停在图块文件夹的中间：放进去，停一会儿落下卡片。
  if (entry.row && is_dragging_tab() && where >= 0.25f && where <= 0.75f) {
    const std::string& id = rows[entry.index].id;
    if (current == id || pinned_tabs::HasRoomForFavorite(browser_, id)) {
      target.folder = id;
      target.onto = id;
      target.spring = id;
      target.index = at;
      return target;
    }
  }
  if (!room) {
    return std::nullopt;
  }
  const size_t slot = where < 0.5f ? at : at + 1;
  target.index = slot;
  if (slot < entries.size()) {
    target.before = node_of(slot);
  }
  return target;
}

std::optional<TabsView::DropTarget> TabsView::StripDropTarget(
    const gfx::Point& point) const {
  if (!pins_ || (drag_ && !drag_->tile.empty())) {
    return std::nullopt;
  }
  const std::vector<TabListView::Entry>& entries = pins_->entries();
  const std::vector<TabListRowData>& rows = pins_->row_data();
  const std::vector<pin_folders::Child>* root = nullptr;
  if (auto level = levels_.find(std::string()); level != levels_.end()) {
    root = &level->second;
  }
  // 一项在最外层是哪一个：挂在胶囊后面的标签算那个文件夹，「…」不算。
  auto node_of = [&](size_t i) -> pin_folders::Child {
    const TabListView::Entry& entry = entries[i];
    if (entry.row) {
      const std::string& id = rows[entry.index].id;
      return id == kMorePinsId ? pin_folders::Child()
                               : pin_folders::Child{tabs::TabHandle(), id};
    }
    tabs::TabInterface* tab = pins_->TabAt(entry.index);
    if (!tab) {
      return pin_folders::Child();
    }
    if (auto shown = shown_tabs_.find(tab->GetHandle());
        shown != shown_tabs_.end() && shown->second.kept) {
      return pin_folders::Child{tabs::TabHandle(), shown->second.under};
    }
    return pin_folders::Child{tab->GetHandle(), std::string()};
  };
  // 托盘的头尾：线画在托盘外面。
  auto left_edge = [&](size_t i) {
    const TabListView::Entry& entry = entries[i];
    const bool head = entry.row && rows[entry.index].active;
    return entry.x - (head ? TabListView::kTrayPad : 0);
  };
  auto right_edge = [&](size_t i) {
    const TabListView::Entry& entry = entries[i];
    const bool tail = !entry.row && node_of(i).is_folder();
    return entry.x + entry.width + (tail ? TabListView::kTrayPad : 0);
  };
  // 第 i 项前面、后面那条缝的正中。
  auto line_before = [&](size_t i) {
    return i == 0 ? left_edge(0) - kStripLineGap
                  : (right_edge(i - 1) + left_edge(i)) / 2;
  };
  auto line_after = [&](size_t i) {
    return i + 1 == entries.size() ? right_edge(i) + kStripLineGap
                                   : (right_edge(i) + left_edge(i + 1)) / 2;
  };
  // 排在 after（空的是最前面）后面。
  auto gap = [&](const pin_folders::Child& after, int x) {
    DropTarget target;
    target.area = DropTarget::Area::kPins;
    target.ordered = true;
    target.line_x = x;
    if (root) {
      if (after.empty()) {
        if (!root->empty()) {
          target.before = root->front();
        }
      } else if (auto found = std::ranges::find(*root, after);
                 found != root->end() && found + 1 != root->end()) {
        target.before = *(found + 1);
      }
    }
    return target;
  };
  // 放进文件夹 id 的最后；停一会儿落下它的卡片。
  auto into = [&](const std::string& id) -> std::optional<DropTarget> {
    if (InsideDragged(id)) {
      return std::nullopt;
    }
    DropTarget target;
    target.area = DropTarget::Area::kPins;
    target.ordered = true;
    target.folder = id;
    target.onto = id;
    target.spring = id;
    return target;
  };
  if (entries.empty()) {
    return gap(pin_folders::Child(), 0);
  }
  if (point.x() < entries.front().x) {
    return gap(pin_folders::Child(), line_before(0));
  }
  size_t at = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (left_edge(i) <= point.x()) {
      at = i;
    }
  }
  const TabListView::Entry& entry = entries[at];
  const float where =
      entry.width > 0 ? static_cast<float>(point.x() - entry.x) / entry.width
                      : 0;
  auto previous = [&](size_t i) {
    return i == 0 ? pin_folders::Child() : node_of(i - 1);
  };
  if (entry.row) {
    const TabListRowData& row = rows[entry.index];
    if (row.id == kMorePinsId) {
      // 「…」：放在最外层的最后，停一会儿落下收不下的那些。
      DropTarget target;
      target.area = DropTarget::Area::kPins;
      target.ordered = true;
      target.onto = kMorePinsId;
      target.spring = kMorePinsId;
      return target;
    }
    // 胶囊：左边四分之一是排在它前面，右边四分之一（后面没挂着标签时）是排在
    // 它后面，中间是放进去。
    if (where < 0.25f) {
      return gap(previous(at), line_before(at));
    }
    if (!row.active && where > 0.75f) {
      return gap(node_of(at), line_after(at));
    }
    return into(row.id);
  }
  const pin_folders::Child node = node_of(at);
  if (node.is_folder()) {
    // 挂在胶囊后面的当前标签：左半边算放进文件夹，右半边是排在托盘后面。
    if (where < 0.5f) {
      return into(node.folder);
    }
    return gap(node, line_after(at));
  }
  if (where < 0.5f) {
    return gap(previous(at), line_before(at));
  }
  return gap(node, line_after(at));
}

void TabsView::MoveToTreeOrder(tabs::TabInterface* tab) {
  // 固定区按树的先序排成一列，挪到排在它后面的第一个固定标签之前。
  const pin_folders::Levels levels = pin_folders::Layout(browser_);
  std::vector<tabs::TabHandle> order;
  auto walk = [&](auto&& self, const std::string& parent, int depth) -> void {
    auto level = levels.find(parent);
    if (level == levels.end() || depth > kMaxFolderDepth) {
      return;
    }
    for (const pin_folders::Child& child : level->second) {
      if (child.is_folder()) {
        self(self, child.folder, depth + 1);
      } else {
        order.push_back(child.tab);
      }
    }
  };
  walk(walk, std::string(), 0);
  auto it = std::ranges::find(order, tab->GetHandle());
  if (it == order.end()) {
    return;
  }
  tabs::TabInterface* next = nullptr;
  for (++it; it != order.end() && !next; ++it) {
    next = it->Get();
  }
  const int index = model_->GetIndexOfTab(tab);
  const int boundary = model_->IndexOfFirstNonPinnedTab();
  int final_index = next ? model_->GetIndexOfTab(next) : boundary;
  if (final_index > index) {
    --final_index;
  }
  final_index = std::clamp(final_index, 0, std::max(0, boundary - 1));
  if (index != final_index) {
    model_->MoveWebContentsAt(index, final_index, /*select_after_move=*/false);
  }
}

std::string TabsView::DropStateForTesting() const {
  if (!drag_) {
    return "none";
  }
  if (drag_->over_content) {
    return "content";
  }
  if (!drag_->target) {
    return "no target";
  }
  const DropTarget& target = *drag_->target;
  std::map<std::string, std::string> names;
  for (const auto& folder : pin_folders::Read(browser_->GetProfile())) {
    names[folder.id] = base::UTF16ToUTF8(folder.name);
  }
  for (const auto& [id, folder] : fav_folders_) {
    names[id] = base::UTF16ToUTF8(folder.name);
  }
  std::string text;
  if (target.pinned) {
    // 常用图块（grid）、图块文件夹的卡片（card）。
    text = target.area == DropTarget::Area::kCard ? "favcard " : "grid ";
    if (tabs::TabInterface* other = target.merge.Get()) {
      text += "merge " + base::UTF16ToUTF8(pinned_tabs::DisplayName(other));
    } else if (!target.onto.empty()) {
      text += "onto [" + names[target.folder] + "]";
    } else {
      text += "index=" + base::NumberToString(target.index) + " before=";
      if (target.before.is_folder()) {
        text += "[" + names[target.before.folder] + "]";
      } else if (tabs::TabInterface* tab = target.before.tab.Get()) {
        text += base::UTF16ToUTF8(pinned_tabs::DisplayName(tab));
      } else {
        text += "end";
      }
      if (!target.folder.empty()) {
        text += " folder=[" + names[target.folder] + "]";
      }
    }
    if (!target.spring.empty()) {
      text += " spring";
    }
  } else if (target.tree) {
    // 树：放进哪个标签（into）或者缝里的第几层，挂在谁下面、排在谁前面。
    auto title = [](tabs::TabHandle handle, const char* none) {
      tabs::TabInterface* tab = handle.Get();
      return tab ? base::UTF16ToUTF8(tab->GetContents()->GetTitle())
                 : std::string(none);
    };
    text = target.tree_into != tabs::TabHandle()
               ? "tree into"
               : "tree line depth=" + base::NumberToString(target.line_depth) +
                     " range=" + base::NumberToString(target.line_min) + "-" +
                     base::NumberToString(target.line_max);
    text += " parent=" + title(target.tree_parent, "root") +
            " before=" + title(target.tree_before, "end");
    if (!target.spring.empty()) {
      text += " spring";
    }
  } else if (!target.ordered) {
    text = "tabs index=" + base::NumberToString(target.index);
  } else {
    // 顶部标签行的固定区、文件夹的卡片在前面标出来。
    if (target.area == DropTarget::Area::kPins) {
      text = "strip ";
    } else if (target.area == DropTarget::Area::kCard) {
      text = "card ";
    }
    if (!target.onto.empty()) {
      text += target.onto == kMorePinsId ? "onto more" : "onto";
    } else if (target.area == DropTarget::Area::kPins) {
      text += "line x=" + base::NumberToString(target.line_x);
    } else {
      text += "line y=" + base::NumberToString(target.line_y) +
              " depth=" + base::NumberToString(target.line_depth);
    }
    text += " parent=" +
            (target.folder.empty() ? "root" : "[" + names[target.folder] + "]");
    text += " before=";
    if (target.before.is_folder()) {
      text += "[" + names[target.before.folder] + "]";
    } else if (tabs::TabInterface* tab = target.before.tab.Get()) {
      text += base::UTF16ToUTF8(pinned_tabs::DisplayName(tab));
    } else {
      text += "end";
    }
    if (!target.spring.empty()) {
      text += " spring";
    }
  }
  return text + (DropChangesOrder(target) ? "" : " (same place)");
}

void TabsView::UpdateLoadingTimer() {
  bool loading = false;
  for (int i = 0; i < model_->count() && !loading; ++i) {
    loading = model_->GetWebContentsAt(i)->IsLoading();
  }
  if (loading && !loading_timer_.IsRunning()) {
    loading_start_ = base::TimeTicks::Now();
    loading_timer_.Start(FROM_HERE, base::Milliseconds(kLoadingFrameMs), this,
                         &TabsView::StepLoadingAnimations);
  }
}

void TabsView::StepLoadingAnimations() {
  const base::TimeDelta elapsed = base::TimeTicks::Now() - loading_start_;
  bool any = false;
  for (TabListView* list : Lists()) {
    any |= list->StepLoadingAnimations(elapsed);
  }
  if (!any) {
    loading_timer_.Stop();
  }
}

BEGIN_METADATA(TabsView)
END_METADATA

}  // namespace starry
