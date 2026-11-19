// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/menu/menu_host_view.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/case_conversion.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/models/menu_model.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/background.h"
#include "ui/views/focus/focus_manager.h"

namespace starry {

namespace {

constexpr int kCardPadding = 5;
constexpr int kRowHeight = 28;
constexpr int kHeaderHeight = 24;
constexpr int kSeparatorHeight = 9;
constexpr int kMinWidth = 176;
constexpr int kMaxWidth = 360;
constexpr int kRowInset = 9;
constexpr int kIconColumn = 22;
constexpr int kTrailingColumn = 18;
constexpr int kWindowInset = 6;
constexpr int kSubmenuOverlap = 3;
// 挂在按钮上的菜单和按钮之间的空。
constexpr int kAnchorGap = 4;
// 菜单里矢量图标的大小：Starry 自己的和 Chrome 的都用菜单的文字颜色画。
constexpr int kMenuIconSize = 14;

SkColor WithAlpha(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(255 * opacity));
}

bool IsSubmenuType(ui::MenuModel::ItemType type) {
  return type == ui::MenuModel::TYPE_SUBMENU ||
         type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU;
}

// 鼠标正朝子菜单移动吗：point 在 apex（上一次的位置）和子菜单靠近父菜单那条边组成的
// 三角形里。
bool IsHeading(const gfx::Point& apex,
               const gfx::Point& point,
               const gfx::Rect& submenu,
               bool grows_right) {
  const float edge_x = grows_right ? submenu.x() : submenu.right();
  const gfx::PointF a(edge_x, submenu.y());
  const gfx::PointF b(edge_x, submenu.bottom());
  const gfx::PointF p(point);
  const gfx::PointF c(apex);
  auto sign = [](const gfx::PointF& p1, const gfx::PointF& p2,
                 const gfx::PointF& p3) {
    return (p1.x() - p3.x()) * (p2.y() - p3.y()) -
           (p2.x() - p3.x()) * (p1.y() - p3.y());
  };
  const float d1 = sign(p, c, a);
  const float d2 = sign(p, a, b);
  const float d3 = sign(p, b, c);
  const bool negative = d1 < 0 || d2 < 0 || d3 < 0;
  const bool positive = d1 > 0 || d2 > 0 || d3 > 0;
  return !(negative && positive);
}

}  // namespace

// 菜单里的一项。
struct MenuEntry {
  enum class Kind { kRow, kHeader, kSeparator };
  Kind kind = Kind::kRow;
  size_t model_index = 0;
  std::u16string title;
  std::u16string shortcut;
  ui::ImageModel icon;
  bool enabled = true;
  bool checked = false;
  bool submenu = false;
  // 危险的操作（结束进程）：文字、图标和高亮用危险色。
  bool destructive = false;
  int y = 0;
  int height = 0;
};

// 卡片里画行的那一层（在高亮上面）。
class MenuRowsView : public views::View {
  METADATA_HEADER(MenuRowsView, views::View)

 public:
  MenuRowsView() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void Set(const std::vector<MenuEntry>* entries,
           const Theme& theme,
           bool leading,
           bool trailing,
           int shortcut_width) {
    entries_ = entries;
    theme_ = theme;
    leading_ = leading;
    trailing_ = trailing;
    shortcut_width_ = shortcut_width;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    if (!entries_) {
      return;
    }
    for (const MenuEntry& entry : *entries_) {
      const gfx::Rect row(kCardPadding, entry.y,
                          std::max(0, width() - 2 * kCardPadding),
                          entry.height);
      switch (entry.kind) {
        case MenuEntry::Kind::kSeparator:
          canvas->FillRect(gfx::Rect(row.x() + 4, row.CenterPoint().y(),
                                     std::max(0, row.width() - 8), 1),
                           theme_.OnSurface(0.09f));
          break;
        case MenuEntry::Kind::kHeader:
          canvas->DrawStringRectWithFlags(
              entry.title, UiFont(11, gfx::Font::Weight::MEDIUM),
              theme_.on_surface_variant,
              gfx::Rect(row.x() + kRowInset, row.y() + 1,
                        std::max(0, row.width() - 2 * kRowInset),
                        row.height()),
              gfx::Canvas::NO_SUBPIXEL_RENDERING);
          break;
        case MenuEntry::Kind::kRow:
          PaintRow(canvas, entry, row);
          break;
      }
    }
  }

 private:
  void PaintRow(gfx::Canvas* canvas,
                const MenuEntry& entry,
                const gfx::Rect& row) {
    const SkColor foreground = !entry.enabled      ? theme_.OnSurface(0.34f)
                               : entry.destructive ? theme_.danger()
                                                   : theme_.on_surface;
    const SkColor secondary = entry.enabled && !entry.destructive
                                  ? theme_.on_surface_variant
                                  : foreground;
    int x = row.x() + kRowInset;
    const int mid_y = row.CenterPoint().y();
    if (leading_) {
      const gfx::Rect icon(x - 1, mid_y - 8, 16, 16);
      if (entry.icon.IsVectorIcon() &&
          entry.icon.GetVectorIcon().vector_icon()) {
        const int size = kMenuIconSize;
        canvas->Save();
        canvas->Translate(icon.OffsetFromOrigin() +
                          gfx::Vector2d((16 - size) / 2, (16 - size) / 2));
        gfx::PaintVectorIcon(canvas,
                             *entry.icon.GetVectorIcon().vector_icon(), size,
                             foreground);
        canvas->Restore();
      } else if (!entry.icon.IsEmpty() && GetColorProvider()) {
        const gfx::ImageSkia image = entry.icon.Rasterize(GetColorProvider());
        canvas->DrawImageInt(image, 0, 0, image.width(), image.height(),
                             icon.x(), icon.y(), icon.width(), icon.height(),
                             true);
      } else if (entry.checked) {
        canvas->Save();
        canvas->Translate(icon.OffsetFromOrigin() + gfx::Vector2d(2, 2));
        gfx::PaintVectorIcon(canvas, icons::kCheckmark, 12, foreground);
        canvas->Restore();
      }
      x += kIconColumn;
    }
    int right = row.right() - kRowInset;
    if (trailing_) {
      if (entry.submenu) {
        canvas->Save();
        canvas->Translate(gfx::Vector2d(right - 10, mid_y - 5));
        gfx::PaintVectorIcon(canvas, icons::kChevronRight, 10, secondary);
        canvas->Restore();
      } else if (entry.checked && !entry.icon.IsEmpty()) {
        canvas->Save();
        canvas->Translate(gfx::Vector2d(right - 12, mid_y - 6));
        gfx::PaintVectorIcon(canvas, icons::kCheckmark, 12, foreground);
        canvas->Restore();
      }
      right -= kTrailingColumn;
    }
    if (!entry.shortcut.empty()) {
      const int width = shortcut_width_ + 2;
      canvas->DrawStringRectWithFlags(
          entry.shortcut, UiFont(12), secondary,
          gfx::Rect(right - width, row.y(), width, row.height()),
          gfx::Canvas::TEXT_ALIGN_RIGHT | gfx::Canvas::NO_SUBPIXEL_RENDERING);
      right -= width + 12;
    }
    canvas->DrawStringRectWithFlags(
        entry.title, UiFont(13), foreground,
        gfx::Rect(x, row.y(), std::max(0, right - x), row.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  raw_ptr<const std::vector<MenuEntry>> entries_ = nullptr;
  Theme theme_ = Theme::DarkBase();
  bool leading_ = false;
  bool trailing_ = false;
  int shortcut_width_ = 0;
};

BEGIN_METADATA(MenuRowsView)
END_METADATA

// 卡片的面板色和细描边（画在玻璃上，行的下面）。
class MenuCardBackground : public views::View {
  METADATA_HEADER(MenuCardBackground, views::View)

 public:
  MenuCardBackground() { SetCanProcessEventsWithinSubtree(false); }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    // 比命令栏的面板实一点：菜单的字直接盖在网页上，什么底都要读得清。
    fill.setColor(
        WithAlpha(theme_.surface_panel, theme_.is_dark ? 0.62f : 0.72f));
    canvas->DrawRoundRect(bounds, radius::kCard, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius::kCard - 0.5f, border);
  }

 private:
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(MenuCardBackground)
END_METADATA

// 一张菜单卡片：一个 MenuModel（根菜单或子菜单）。
class MenuCard : public GlassView {
  METADATA_HEADER(MenuCard, GlassView)

 public:
  MenuCard(ui::MenuModel* model,
           const Theme& theme,
           base::RepeatingCallback<bool(int command_id)> destructive)
      : model_(model), theme_(theme), destructive_(std::move(destructive)) {
    SetCanProcessEventsWithinSubtree(false);
    SetCornerRadius(radius::kCard, true);
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);

    background_ = AddChildView(std::make_unique<MenuCardBackground>());
    background_->SetTheme(theme);
    clip_ = AddChildView(std::make_unique<views::View>());
    clip_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    clip_->layer()->SetMasksToBounds(true);
    clip_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(radius::kCard));
    clip_->SetCanProcessEventsWithinSubtree(false);
    highlight_ = clip_->AddChildView(std::make_unique<views::View>());
    highlight_->SetPaintToLayer();
    highlight_->layer()->SetFillsBoundsOpaquely(false);
    highlight_->layer()->SetOpacity(0);
    rows_ = clip_->AddChildView(std::make_unique<MenuRowsView>());
    BuildEntries();
  }

  ui::MenuModel* model() const { return model_; }
  const std::vector<MenuEntry>& entries() const { return entries_; }
  std::optional<size_t> selected() const { return selected_; }
  std::optional<size_t> open_submenu() const { return open_submenu_; }
  void set_open_submenu(std::optional<size_t> entry) { open_submenu_ = entry; }
  bool grows_right() const { return grows_right_; }
  LayerMotion& motion() { return motion_; }

  gfx::Size ContentSize() const { return content_size_; }

  // 模型里的勾选、标题变了（选了不关菜单的一项）：重新读一遍，位置和选中的行不变。
  void Refresh() {
    BuildEntries();
    const gfx::Rect card = glass_bounds();
    Place(gfx::Rect(card.origin(),
                    gfx::Size(card.width(),
                              std::min(card.height(), content_size_.height()))),
          grows_right_, grows_down_);
    if (selected_ && !IsSelectable(*selected_)) {
      SetSelected(std::nullopt);
    }
  }

  // 放到 card（父视图坐标）；高度超过时可以滚动。
  void Place(const gfx::Rect& card, bool grows_right, bool grows_down) {
    grows_right_ = grows_right;
    grows_down_ = grows_down;
    SetGlassBounds(card);
    const gfx::Rect rect = glass_rect();
    background_->SetBoundsRect(rect);
    clip_->SetBoundsRect(rect);
    rows_->SetBounds(0, -scroll_, rect.width(), content_size_.height());
    highlight_->SetBounds(kCardPadding, -scroll_,
                          std::max(0, rect.width() - 2 * kCardPadding),
                          kRowHeight);
    if (selected_ && IsSelectable(*selected_)) {
      LayerPose pose;
      pose.offset.set_y(entries_[*selected_].y);
      highlight_motion_.SetTransform(highlight_->layer(), pose);
    }
  }

  // 打开时从哪个角放大（玻璃视图自己的坐标）。
  gfx::PointF OpenOrigin() const {
    const gfx::Rect rect = glass_rect();
    return gfx::PointF(grows_right_ ? rect.x() : rect.right(),
                       grows_down_ ? rect.y() : rect.bottom());
  }

  // point（父视图坐标）在哪一项上；在卡片里但不在行上时返回 nullopt。
  bool Contains(const gfx::Point& point) const {
    return glass_bounds().Contains(point);
  }

  std::optional<size_t> EntryAt(const gfx::Point& point) const {
    if (!Contains(point)) {
      return std::nullopt;
    }
    const int y = point.y() - glass_bounds().y() + scroll_;
    for (size_t i = 0; i < entries_.size(); ++i) {
      const MenuEntry& entry = entries_[i];
      if (y >= entry.y && y < entry.y + entry.height) {
        return i;
      }
    }
    return std::nullopt;
  }

  // 一项在父视图里的位置。
  gfx::Rect EntryRectInParent(size_t index) const {
    const MenuEntry& entry = entries_[index];
    return gfx::Rect(glass_bounds().x(),
                     glass_bounds().y() + entry.y - scroll_,
                     glass_bounds().width(), entry.height);
  }

  bool IsSelectable(size_t index) const {
    return index < entries_.size() &&
           entries_[index].kind == MenuEntry::Kind::kRow &&
           entries_[index].enabled;
  }

  void SetSelected(std::optional<size_t> index) {
    ui::Layer* layer = highlight_->layer();
    const MotionCurve fade = MotionCurve::EaseOut(motion::kMenuHighlightFade);
    if (!index || !IsSelectable(*index)) {
      selected_.reset();
      // 分隔线不能执行命令，但经过它时保留高亮，下一行从原位置接着滑。
      if (index && *index < entries_.size() &&
          entries_[*index].kind == MenuEntry::Kind::kSeparator) {
        return;
      }
      if (layer->GetTargetOpacity() > 0) {
        highlight_motion_.AnimateOpacity(layer, 0, fade);
      }
      return;
    }
    if (selected_ == index) {
      return;
    }
    selected_ = index;
    const MenuEntry& entry = entries_[*index];
    highlight_color_ =
        entry.destructive
            ? WithAlpha(theme_.danger(), theme_.is_dark ? 0.24f : 0.14f)
            : WithAlpha(theme_.accent, theme_.is_dark ? 0.17f : 0.1f);
    LayerPose target;
    target.offset.set_y(entry.y);
    if (motion::ReduceMotion() || layer->GetTargetOpacity() == 0) {
      highlight_motion_.SetTransform(layer, target);
    } else {
      highlight_motion_.AnimateTransform(
          layer, target, MotionCurve::Of(motion::kMenuHighlight));
    }
    highlight_motion_.AnimateOpacity(layer, 1, fade);
    PaintHighlight();
    ScrollToVisible(*index);
  }

  // 选中一项时高亮闪一下。
  void Flash() {
    highlight_motion_.AnimateOpacityFrom(
        highlight_->layer(), 0.35f, 1, MotionCurve::EaseOut(motion::kMenuFlash));
  }

  bool ScrollBy(int dy) {
    const int max_scroll =
        std::max(0, content_size_.height() - glass_rect().height());
    const int scroll = std::clamp(scroll_ + dy, 0, max_scroll);
    if (scroll == scroll_) {
      return false;
    }
    scroll_ = scroll;
    rows_->SetY(-scroll_);
    highlight_->SetY(-scroll_);
    return true;
  }

 private:
  void BuildEntries() {
    entries_.clear();
    int y = kCardPadding;
    int title_width = 0;
    int shortcut_width = 0;
    bool leading = false;
    bool trailing = false;
    bool last_separator = true;
    for (size_t i = 0; i < model_->GetItemCount(); ++i) {
      if (!model_->IsVisibleAt(i)) {
        continue;
      }
      const ui::MenuModel::ItemType type = model_->GetTypeAt(i);
      MenuEntry entry;
      entry.model_index = i;
      if (type == ui::MenuModel::TYPE_SEPARATOR) {
        // 开头、连着的分隔线不要。
        if (last_separator) {
          continue;
        }
        entry.kind = MenuEntry::Kind::kSeparator;
        entry.height = kSeparatorHeight;
        last_separator = true;
      } else if (type == ui::MenuModel::TYPE_TITLE) {
        entry.kind = MenuEntry::Kind::kHeader;
        entry.title = gfx::RemoveAccelerator(model_->GetLabelAt(i));
        entry.height = kHeaderHeight;
        title_width = std::max(
            title_width, gfx::GetStringWidth(
                             entry.title, UiFont(11, gfx::Font::Weight::MEDIUM)));
        last_separator = false;
      } else {
        entry.kind = MenuEntry::Kind::kRow;
        entry.title = gfx::RemoveAccelerator(model_->GetLabelAt(i));
        entry.enabled = model_->IsEnabledAt(i);
        entry.submenu = IsSubmenuType(type);
        entry.checked = (type == ui::MenuModel::TYPE_CHECK ||
                         type == ui::MenuModel::TYPE_RADIO) &&
                        model_->IsItemCheckedAt(i);
        entry.icon = model_->GetIconAt(i);
        entry.destructive =
            destructive_ && destructive_.Run(model_->GetCommandIdAt(i));
        ui::Accelerator accelerator;
        if (!entry.submenu && model_->GetAcceleratorAt(i, &accelerator)) {
          entry.shortcut = accelerator.GetShortcutText();
        }
        entry.height = kRowHeight;
        title_width =
            std::max(title_width, gfx::GetStringWidth(entry.title, UiFont(13)));
        if (!entry.shortcut.empty()) {
          shortcut_width = std::max(
              shortcut_width, gfx::GetStringWidth(entry.shortcut, UiFont(12)));
        }
        leading |= !entry.icon.IsEmpty() ||
                   type == ui::MenuModel::TYPE_CHECK ||
                   type == ui::MenuModel::TYPE_RADIO;
        // 有图标的勾选项：勾画在右边那一栏。
        trailing |= entry.submenu || ((type == ui::MenuModel::TYPE_CHECK ||
                                       type == ui::MenuModel::TYPE_RADIO) &&
                                      !entry.icon.IsEmpty());
        last_separator = false;
      }
      entry.y = y;
      y += entry.height;
      entries_.push_back(std::move(entry));
    }
    // 结尾的分隔线不要。
    while (!entries_.empty() &&
           entries_.back().kind == MenuEntry::Kind::kSeparator) {
      y -= entries_.back().height;
      entries_.pop_back();
    }
    y += kCardPadding;

    int width = kCardPadding * 2 + kRowInset * 2 + title_width + 6;
    if (leading) {
      width += kIconColumn;
    }
    if (trailing) {
      width += kTrailingColumn;
    }
    if (shortcut_width > 0) {
      width += 24 + shortcut_width;
    }
    content_size_ = gfx::Size(std::clamp(width, kMinWidth, kMaxWidth), y);
    rows_->Set(&entries_, theme_, leading, trailing, shortcut_width);
  }

  void PaintHighlight() {
    highlight_->SetBackground(
        views::CreateRoundedRectBackground(highlight_color_, radius::kMenu));
  }

  void ScrollToVisible(size_t index) {
    const MenuEntry& entry = entries_[index];
    const int view_height = glass_rect().height();
    if (entry.y - kCardPadding < scroll_) {
      ScrollBy(entry.y - kCardPadding - scroll_);
    } else if (entry.y + entry.height + kCardPadding > scroll_ + view_height) {
      ScrollBy(entry.y + entry.height + kCardPadding - scroll_ - view_height);
    }
  }

  const raw_ptr<ui::MenuModel> model_;
  const Theme theme_;
  const base::RepeatingCallback<bool(int)> destructive_;
  std::vector<MenuEntry> entries_;
  gfx::Size content_size_;
  raw_ptr<MenuCardBackground> background_ = nullptr;
  raw_ptr<views::View> clip_ = nullptr;
  raw_ptr<views::View> highlight_ = nullptr;
  raw_ptr<MenuRowsView> rows_ = nullptr;
  SkColor highlight_color_ = SK_ColorTRANSPARENT;
  std::optional<size_t> selected_;
  std::optional<size_t> open_submenu_;
  bool grows_right_ = true;
  bool grows_down_ = true;
  int scroll_ = 0;
  LayerMotion motion_;
  LayerMotion highlight_motion_;
};

BEGIN_METADATA(MenuCard)
END_METADATA

MenuHostView::MenuHostView() {
  // 卡片把鼠标交给本视图。Aura 用命中视图的图层判断点击是否该发给下面的网页。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetVisible(false);
  SetFocusBehavior(FocusBehavior::ALWAYS);
}

MenuHostView::~MenuHostView() {
  // 菜单在 OnWidgetDestroying 里已经关了。这里不再碰模型：它们可能已经不在了。
  cards_.clear();
}

void MenuHostView::AddedToWidget() {
  widget_observation_.Reset();
  widget_observation_.Observe(GetWidget());
}

void MenuHostView::RemovedFromWidget() {
  widget_observation_.Reset();
}

void MenuHostView::OnWidgetDestroying(views::Widget* widget) {
  widget_observation_.Reset();
  if (!cards_.empty()) {
    Finish(/*animated=*/false, /*notify=*/true);
  }
}

MenuOptions::MenuOptions() = default;
MenuOptions::MenuOptions(const MenuOptions&) = default;
MenuOptions& MenuOptions::operator=(const MenuOptions&) = default;
MenuOptions::~MenuOptions() = default;

void MenuHostView::ShowMenuAt(ui::MenuModel* model,
                              const gfx::Point& point,
                              base::OnceClosure closed,
                              MenuOptions options) {
  Finish(/*animated=*/false);
  RemoveCards();
  options_ = std::move(options);
  if (!model || model->GetItemCount() == 0) {
    if (closed) {
      std::move(closed).Run();
    }
    return;
  }
  closed_ = std::move(closed);
  SetVisible(true);
  SetCanProcessEventsWithinSubtree(true);

  model->MenuWillShow();
  auto* card = AddChildView(
      std::make_unique<MenuCard>(model, theme_, options_.destructive));
  cards_.push_back(card);

  // 左上角在鼠标处，放不下时朝反方向长，再放不下时贴着窗口边。
  gfx::Rect visible = GetLocalBounds();
  visible.Inset(kWindowInset);
  const gfx::Size size = card->ContentSize();
  const int width = std::min(size.width(), visible.width());
  const int height = std::min(size.height(), visible.height());
  bool grows_right = true;
  bool grows_down = true;
  int x = 0;
  int y = 0;
  if (options_.anchor) {
    // 按钮的菜单：在按钮下面左对齐，放不下放上面、右对齐。
    const gfx::Rect& anchor = *options_.anchor;
    grows_right = anchor.x() + width <= visible.right() ||
                  anchor.right() - width < visible.x();
    grows_down = anchor.bottom() + kAnchorGap + height <= visible.bottom() ||
                 anchor.y() - kAnchorGap - height < visible.y();
    x = grows_right ? anchor.x() : anchor.right() - width;
    y = grows_down ? anchor.bottom() + kAnchorGap
                   : anchor.y() - kAnchorGap - height;
  } else {
    grows_right = point.x() + width <= visible.right() ||
                  point.x() - width < visible.x();
    grows_down = point.y() + height <= visible.bottom() ||
                 point.y() - height < visible.y();
    x = grows_right ? point.x() : point.x() - width;
    y = grows_down ? point.y() : point.y() - height;
  }
  x = std::clamp(x, visible.x(), std::max(visible.x(), visible.right() - width));
  y = std::clamp(y, visible.y(),
                 std::max(visible.y(), visible.bottom() - height));
  card->Place(gfx::Rect(x, y, width, height), grows_right, grows_down);

  // 从鼠标那个角由 0.95 放大，0.09 秒淡入。
  LayerPose from;
  if (!motion::ReduceMotion()) {
    from.scale = motion::kMenuOpenScale;
  }
  card->motion().AnimateTransformFrom(card->layer(), from, LayerPose(),
                                      MotionCurve::Of(motion::kMenuOpen),
                                      card->OpenOrigin());
  card->motion().AnimateOpacityFrom(card->layer(), 0, 1,
                                    MotionCurve::EaseOut(motion::kMenuFadeIn));

  views::FocusManager* focus_manager = GetFocusManager();
  previous_focus_.SetView(focus_manager ? focus_manager->GetFocusedView()
                                        : nullptr);
  RequestFocus();
  last_mouse_ = point;
}

void MenuHostView::Close() {
  Finish(/*animated=*/true);
}

base::OnceClosure MenuHostView::GetCloseCallbackFor(ui::MenuModel* model) {
  return base::BindOnce(
      [](base::WeakPtr<MenuHostView> host, base::WeakPtr<ui::MenuModel> model) {
        if (host && host->is_showing() &&
            host->cards_.front()->model() == model.get()) {
          host->Finish(/*animated=*/false);
        }
      },
      weak_factory_.GetWeakPtr(), model->AsWeakPtr());
}

void MenuHostView::SetTheme(const Theme& theme) {
  theme_ = theme;
}

std::vector<std::u16string> MenuHostView::ItemTitlesForTesting(
    size_t depth) const {
  std::vector<std::u16string> titles;
  if (depth < cards_.size()) {
    for (const MenuEntry& entry : cards_[depth]->entries()) {
      titles.push_back(entry.kind == MenuEntry::Kind::kSeparator
                           ? u"—"
                           : entry.title);
    }
  }
  return titles;
}

void MenuHostView::SelectForTesting(size_t depth, size_t row) {
  if (depth < cards_.size()) {
    Select(depth, row, /*from_keyboard=*/true);
  }
}

void MenuHostView::ActivateSelectedForTesting() {
  if (!cards_.empty() && cards_.back()->selected()) {
    Activate(cards_.size() - 1, *cards_.back()->selected());
  }
}

std::optional<MenuHostView::Hit> MenuHostView::HitAt(
    const gfx::Point& point) const {
  // 最上面（最深）的卡片优先。
  for (size_t depth = cards_.size(); depth-- > 0;) {
    if (cards_[depth]->Contains(point)) {
      return Hit{depth, cards_[depth]->EntryAt(point)};
    }
  }
  return std::nullopt;
}

bool MenuHostView::OnMousePressed(const ui::MouseEvent& event) {
  pressed_outside_ = !HitAt(event.location()).has_value();
  if (pressed_outside_) {
    // 点在菜单外面：关掉，这一下不再交给下面的东西（同系统菜单）。
    Finish(/*animated=*/true);
  }
  return true;
}

bool MenuHostView::OnMouseDragged(const ui::MouseEvent& event) {
  Hover(event.location());
  return true;
}

void MenuHostView::OnMouseReleased(const ui::MouseEvent& event) {
  if (pressed_outside_) {
    pressed_outside_ = false;
    return;
  }
  std::optional<Hit> hit = HitAt(event.location());
  if (hit && hit->row) {
    Activate(hit->depth, *hit->row);
  }
}

void MenuHostView::OnMouseMoved(const ui::MouseEvent& event) {
  Hover(event.location());
}

void MenuHostView::OnMouseExited(const ui::MouseEvent& event) {
  if (cards_.size() == 1) {
    Select(0, std::nullopt, false);
  }
}

bool MenuHostView::OnMouseWheel(const ui::MouseWheelEvent& event) {
  std::optional<Hit> hit = HitAt(event.location());
  if (hit) {
    cards_[hit->depth]->ScrollBy(-event.y_offset());
  }
  return true;
}

void MenuHostView::Hover(const gfx::Point& point) {
  const std::optional<gfx::Point> apex = last_mouse_;
  last_mouse_ = point;
  std::optional<Hit> hit = HitAt(point);
  if (!hit) {
    // 在菜单外面：开着的子菜单留着，只有一层时去掉高亮。
    if (cards_.size() == 1) {
      Select(0, std::nullopt, false);
    }
    return;
  }
  // 父菜单上斜着移向开着的子菜单：经过的行先不切换，等一会儿还在才切换。
  if (hit->depth + 1 < cards_.size() && apex) {
    MenuCard* parent = cards_[hit->depth];
    MenuCard* child = cards_[hit->depth + 1];
    if (hit->row != parent->open_submenu() &&
        IsHeading(*apex, point, child->glass_bounds(), child->grows_right())) {
      pending_hover_ = hit;
      grace_timer_.Start(
          FROM_HERE, base::Seconds(motion::kMenuSubmenuGrace),
          base::BindOnce(
              [](MenuHostView* host) {
                if (host->pending_hover_ && host->last_mouse_) {
                  host->Hover(*host->last_mouse_);
                }
              },
              base::Unretained(this)));
      return;
    }
  }
  grace_timer_.Stop();
  pending_hover_.reset();
  if (hit->depth + 1 == cards_.size() ||
      (hit->row && cards_[hit->depth]->entries()[*hit->row].kind ==
                       MenuEntry::Kind::kRow)) {
    Select(hit->depth, hit->row, false);
  }
}

void MenuHostView::Select(size_t depth,
                          std::optional<size_t> row,
                          bool from_keyboard) {
  MenuCard* card = cards_[depth];
  // 换到父菜单的别的行：收起更深的子菜单。
  if (depth + 1 < cards_.size() && row != card->open_submenu()) {
    CloseSubmenus(depth);
  }
  card->SetSelected(row);
  submenu_timer_.Stop();
  if (row && card->IsSelectable(*row) && card->entries()[*row].submenu &&
      card->open_submenu() != row && !from_keyboard) {
    const size_t selected = *row;
    submenu_timer_.Start(
        FROM_HERE, base::Seconds(motion::kMenuSubmenuDelay),
        base::BindOnce(&MenuHostView::OpenSubmenu, base::Unretained(this),
                       depth, selected, /*select_first=*/false));
  }
}

void MenuHostView::OpenSubmenu(size_t depth, size_t row, bool select_first) {
  if (depth >= cards_.size()) {
    return;
  }
  CloseSubmenus(depth);
  MenuCard* parent = cards_[depth];
  const MenuEntry& entry = parent->entries()[row];
  ui::MenuModel* model =
      parent->model()->GetSubmenuModelAt(entry.model_index);
  if (!model) {
    return;
  }
  model->MenuWillShow();
  parent->set_open_submenu(row);
  auto* card = AddChildView(
      std::make_unique<MenuCard>(model, theme_, options_.destructive));
  cards_.push_back(card);

  // 父菜单右边（放不下放左边），第一行和那一行对齐，压住 3。
  gfx::Rect visible = GetLocalBounds();
  visible.Inset(kWindowInset);
  const gfx::Size size = card->ContentSize();
  const int width = std::min(size.width(), visible.width());
  const int height = std::min(size.height(), visible.height());
  const gfx::Rect parent_rect = parent->glass_bounds();
  const gfx::Rect row_rect = parent->EntryRectInParent(row);
  const int right = parent_rect.right() - kSubmenuOverlap;
  const int left = parent_rect.x() + kSubmenuOverlap - width;
  const bool grows_right =
      right + width <= visible.right() || left < visible.x();
  int x = grows_right ? right : left;
  int y = row_rect.y() - kCardPadding;
  x = std::clamp(x, visible.x(), std::max(visible.x(), visible.right() - width));
  y = std::clamp(y, visible.y(),
                 std::max(visible.y(), visible.bottom() - height));
  card->Place(gfx::Rect(x, y, width, height), grows_right, true);

  LayerPose from;
  if (!motion::ReduceMotion()) {
    from.scale = motion::kSubmenuOpenScale;
  }
  card->motion().AnimateTransformFrom(card->layer(), from, LayerPose(),
                                      MotionCurve::Of(motion::kMenuOpen),
                                      card->OpenOrigin());
  card->motion().AnimateOpacityFrom(card->layer(), 0, 1,
                                    MotionCurve::EaseOut(motion::kMenuFadeIn));
  if (select_first) {
    for (size_t i = 0; i < card->entries().size(); ++i) {
      if (card->IsSelectable(i)) {
        card->SetSelected(i);
        break;
      }
    }
  }
}

void MenuHostView::CloseSubmenus(size_t keep_depth) {
  while (cards_.size() > keep_depth + 1) {
    MenuCard* card = cards_.back();
    cards_.pop_back();
    card->model()->MenuWillClose();
    RemoveChildViewT(card);
  }
  if (keep_depth < cards_.size()) {
    cards_[keep_depth]->set_open_submenu(std::nullopt);
  }
}

void MenuHostView::Activate(size_t depth, size_t row) {
  if (depth >= cards_.size()) {
    return;
  }
  MenuCard* card = cards_[depth];
  if (!card->IsSelectable(row)) {
    return;
  }
  const MenuEntry& entry = card->entries()[row];
  if (entry.submenu) {
    submenu_timer_.Stop();
    if (card->open_submenu() != row) {
      card->SetSelected(row);
      OpenSubmenu(depth, row, /*select_first=*/true);
    }
    return;
  }
  card->SetSelected(row);
  card->Flash();
  ui::MenuModel* model = card->model();
  const size_t index = entry.model_index;
  if (options_.keeps_open &&
      options_.keeps_open.Run(model->GetCommandIdAt(index))) {
    // 勾一组开关：菜单留着，重画勾选。
    model->ActivatedAt(index, 0);
    if (depth < cards_.size() && cards_[depth] == card) {
      card->Refresh();
    }
    return;
  }
  // 先收起菜单（模型还在），再执行，最后通知关闭：同 views 的 MenuRunner。
  base::OnceClosure closed = std::move(closed_);
  std::vector<raw_ptr<ui::MenuModel>> models;
  for (MenuCard* open : cards_) {
    models.push_back(open->model());
  }
  closed_ = base::OnceClosure();
  Finish(/*animated=*/true, /*notify=*/false);
  model->ActivatedAt(index, 0);
  for (size_t i = models.size(); i-- > 0;) {
    models[i]->MenuWillClose();
  }
  if (closed) {
    std::move(closed).Run();
  }
}

void MenuHostView::MoveSelection(int delta) {
  if (cards_.empty()) {
    return;
  }
  const size_t depth = cards_.size() - 1;
  MenuCard* card = cards_[depth];
  const size_t count = card->entries().size();
  if (count == 0) {
    return;
  }
  size_t index = card->selected().value_or(delta > 0 ? count - 1 : 0);
  for (size_t step = 0; step < count; ++step) {
    index = (index + count + (delta > 0 ? 1 : count - 1)) % count;
    if (card->IsSelectable(index)) {
      Select(depth, index, /*from_keyboard=*/true);
      return;
    }
  }
}

void MenuHostView::TypeAhead(char16_t ch) {
  if (cards_.empty()) {
    return;
  }
  const size_t depth = cards_.size() - 1;
  MenuCard* card = cards_[depth];
  const std::u16string key = base::i18n::ToLower(std::u16string(1, ch));
  const size_t count = card->entries().size();
  const size_t start = card->selected().value_or(count - 1);
  for (size_t step = 1; step <= count; ++step) {
    const size_t index = (start + step) % count;
    if (!card->IsSelectable(index)) {
      continue;
    }
    const std::u16string title =
        base::i18n::ToLower(card->entries()[index].title);
    if (title.starts_with(key)) {
      Select(depth, index, /*from_keyboard=*/true);
      return;
    }
  }
}

bool MenuHostView::SkipDefaultKeyEventProcessing(const ui::KeyEvent& event) {
  // 菜单开着时按键都归菜单（⌘T 这些快捷键不生效）。
  return is_showing();
}

bool MenuHostView::OnKeyPressed(const ui::KeyEvent& event) {
  if (cards_.empty()) {
    return false;
  }
  const size_t depth = cards_.size() - 1;
  MenuCard* card = cards_[depth];
  switch (event.key_code()) {
    case ui::VKEY_UP:
      MoveSelection(-1);
      return true;
    case ui::VKEY_DOWN:
      MoveSelection(1);
      return true;
    case ui::VKEY_RIGHT:
      if (card->selected() && card->entries()[*card->selected()].submenu) {
        OpenSubmenu(depth, *card->selected(), /*select_first=*/true);
      }
      return true;
    case ui::VKEY_LEFT:
      if (depth > 0) {
        CloseSubmenus(depth - 1);
      }
      return true;
    case ui::VKEY_RETURN:
    case ui::VKEY_SPACE:
      if (card->selected()) {
        Activate(depth, *card->selected());
      }
      return true;
    case ui::VKEY_ESCAPE:
      if (depth > 0) {
        CloseSubmenus(depth - 1);
      } else {
        Finish(/*animated=*/true);
      }
      return true;
    default:
      break;
  }
  const char16_t ch = event.GetCharacter();
  if (ch >= 0x20 && !(event.flags() & (ui::EF_CONTROL_DOWN | ui::EF_COMMAND_DOWN))) {
    TypeAhead(ch);
  }
  return true;
}

void MenuHostView::Finish(bool animated) {
  Finish(animated, /*notify=*/true);
}

void MenuHostView::Finish(bool animated, bool notify) {
  submenu_timer_.Stop();
  grace_timer_.Stop();
  pending_hover_.reset();
  if (cards_.empty()) {
    return;
  }
  std::vector<raw_ptr<MenuCard, VectorExperimental>> cards = std::move(cards_);
  cards_.clear();
  options_ = MenuOptions();
  // 关掉以后不再挡住鼠标。
  SetCanProcessEventsWithinSubtree(false);
  if (previous_focus_.view()) {
    previous_focus_.view()->RequestFocus();
  }
  previous_focus_.SetView(nullptr);

  base::OnceClosure closed = std::move(closed_);
  if (notify) {
    // 不是选了一项关掉的：从最深的子菜单往外通知关闭。
    for (size_t i = cards.size(); i-- > 0;) {
      cards[i]->model()->MenuWillClose();
    }
  }
  for (MenuCard* card : cards) {
    if (!animated) {
      RemoveChildViewT(card);
      continue;
    }
    card->motion().AnimateOpacity(
        card->layer(), 0, MotionCurve::EaseOut(motion::kMenuFadeOut),
        base::BindOnce(
            [](base::WeakPtr<MenuHostView> host, MenuCard* card) {
              if (!host) {
                return;
              }
              for (views::View* child : host->children()) {
                if (child == card) {
                  host->RemoveChildViewT(card);
                  break;
                }
              }
              if (host->children().empty() && host->cards_.empty()) {
                host->SetVisible(false);
              }
            },
            weak_factory_.GetWeakPtr(), base::Unretained(card)));
  }
  if (!animated && cards_.empty()) {
    SetVisible(false);
  }
  if (closed) {
    std::move(closed).Run();
  }
}

void MenuHostView::RemoveCards() {
  // 上一个菜单还在淡出：直接拿掉。
  while (!children().empty()) {
    RemoveChildViewT(children().back());
  }
}

BEGIN_METADATA(MenuHostView)
END_METADATA

}  // namespace starry
