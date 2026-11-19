// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tab_list_row.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "starry/browser/ui/tabs/tab_list_view.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/textfield/textfield.h"

namespace starry {

namespace {

// 和标签的行对齐：左 10 右 6，图标 16，图标和名字之间 10。
constexpr int kLeading = 10;
constexpr int kTrailing = 6;
constexpr int kSpacing = 10;
constexpr int kIconSize = 16;
constexpr int kChevronSize = 12;
// 每一层往右缩这么多：比图标的中线（18）多 2，层级线和里面的行之间留一点空。
constexpr int kIndent = 20;
constexpr int kPlusSize = 14;
constexpr int kActionSize = 22;
constexpr int kActionGap = 2;
// 折叠时右侧叠着的网站图标：14 大，后一个压住前一个 5，之间留一圈 1.5 的空。
constexpr int kStackIcon = 14;
constexpr int kStackStep = 9;
constexpr size_t kStackMax = 3;
constexpr float kStackGap = 1.5f;
constexpr int kStackCountGap = 4;
constexpr int kLabelHeight = 28;
constexpr int kLabelTextHeight = 22;
constexpr int kEmptyHeight = 30;
constexpr int kDividerHeight = 13;
constexpr int kDividerInset = 10;
constexpr int kFieldHeight = 18;
constexpr int kFieldOutsetX = 6;
constexpr int kFieldOutsetY = 3;
// 按住移动超过这么多就是拖动（同 TabCell）。
constexpr int kDragThreshold = 4;
// 胶囊：左 8 右 9（托盘里 6），图标、名字、数量之间 6；只有图标时左右各 8。
// 名字太长时整枚最宽 160。
constexpr int kChipLeading = 8;
constexpr int kChipTrailing = 9;
constexpr int kChipTrayTrailing = 6;
constexpr int kChipGap = 6;
constexpr int kChipMaxWidth = 160;
constexpr int kChipMoreGlyph = 14;
constexpr int kChipFontSize = 12;
constexpr int kIndicatorSize = 11;
constexpr int kIndicatorGap = 4;
// 图块里的小图标：2×2，侧栏里 13、顶部 11，之间 2，圆角 3。右上角的声音、
// 智能体小图标 10（顶部 9），离边 3。
constexpr int kTileIcon = 13;
constexpr int kTopTileIcon = 11;
constexpr int kTileIconGap = 2;
constexpr float kTileIconRadius = 3;
constexpr int kTileBadge = 10;
constexpr int kTopTileBadge = 9;
constexpr int kTileBadgeInset = 3;
// 面包屑：高 30；返回的箭头占 22（左边留 4），箭头、图标、名字、数字之间 7；
// 行宽到 240 才在名字前面写上一层。
constexpr int kHoistHeight = 30;
constexpr int kHoistLeading = 4;
constexpr int kHoistBack = 22;
constexpr int kHoistBackIcon = 14;
constexpr int kHoistGap = 7;
constexpr int kHoistTrailing = 8;
constexpr int kHoistCrumbMinWidth = 240;
constexpr int kHoistCountHeight = 16;
constexpr int kHoistCountMinWidth = 18;

SkColor WithAlpha(SkColor color, float alpha) {
  return SkColorSetA(color, static_cast<U8CPU>(SkColorGetA(color) *
                                               std::clamp(alpha, 0.0f, 1.0f)));
}

// 拖着的东西会放进来：垫一层强调色的底，描一圈边。
void PaintDropOutline(gfx::Canvas* canvas,
                      const Theme& theme,
                      gfx::RectF rect,
                      float radius,
                      float amount) {
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(WithAlpha(theme.accent, 0.12f * amount));
  canvas->DrawRoundRect(rect, radius, fill);
  cc::PaintFlags outline;
  outline.setAntiAlias(true);
  outline.setStyle(cc::PaintFlags::kStroke_Style);
  outline.setStrokeWidth(1.5f);
  outline.setColor(WithAlpha(theme.accent, 0.7f * amount));
  rect.Inset(0.75f);
  canvas->DrawRoundRect(rect, std::max(0.0f, radius - 0.75f), outline);
}

void PaintIconAt(gfx::Canvas* canvas,
                 const gfx::VectorIcon& icon,
                 int size,
                 SkColor color,
                 const gfx::Point& origin) {
  canvas->Save();
  canvas->Translate(origin.OffsetFromOrigin());
  gfx::PaintVectorIcon(canvas, icon, size, color);
  canvas->Restore();
}

class RowActionButton : public GlyphButton {
  METADATA_HEADER(RowActionButton, GlyphButton)

 public:
  using GlyphButton::GlyphButton;

  void StateChanged(ButtonState old_state) override {
    GlyphButton::StateChanged(old_state);
    SetShowsFill(GetState() == STATE_HOVERED || GetState() == STATE_PRESSED);
  }
};

BEGIN_METADATA(RowActionButton)
END_METADATA

class RowTextfield : public views::Textfield {
  METADATA_HEADER(RowTextfield, views::Textfield)

 public:
  explicit RowTextfield(TabListRow* row) : row_(row) {}

  void Detach() { row_ = nullptr; }

  void OnBlur() override {
    views::Textfield::OnBlur();
    if (row_) {
      row_->OnFieldBlurred();
    }
  }

 private:
  raw_ptr<TabListRow> row_;
};

BEGIN_METADATA(RowTextfield)
END_METADATA

}  // namespace

TabListRow::TabListRow(TabCellDelegate* delegate)
    : AnimationDelegateViews(this), delegate_(delegate) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetNotifyEnterExitOnChild(true);
  set_context_menu_controller(this);
  SetWindowDraggable(this, false);
  hover_animation_.SetSlideDuration(
      base::Milliseconds(motion::kHoverDuration * 1000));
  hover_animation_.SetTweenType(gfx::Tween::EASE_OUT);
  drop_animation_.SetSlideDuration(
      base::Milliseconds(motion::kHoverDuration * 1000));
  drop_animation_.SetTweenType(gfx::Tween::EASE_OUT);
}

TabListRow::~TabListRow() = default;

// static
int TabListRow::HeightFor(TabListRowData::Kind kind) {
  switch (kind) {
    case TabListRowData::Kind::kLabel:
      return kLabelHeight;
    case TabListRowData::Kind::kFolder:
    case TabListRowData::Kind::kNewTab:
      return metrics::kTabRowHeight;
    case TabListRowData::Kind::kEmpty:
      return kEmptyHeight;
    case TabListRowData::Kind::kDivider:
      return kDividerHeight;
    case TabListRowData::Kind::kChip:
      return metrics::kTopTabHeight;
    case TabListRowData::Kind::kTile:
      return metrics::kPinnedTileHeight + kTileCaptionHeight;
    case TabListRowData::Kind::kHoist:
      return kHoistHeight;
  }
}

// static
int TabListRow::ChipWidth(const TabListRowData& data) {
  if (data.compact || data.id == kMorePinsId) {
    return 2 * kChipLeading + kIconSize;
  }
  int width = kChipLeading + kIconSize + kChipGap +
              gfx::GetStringWidth(data.title, UiFont(kChipFontSize));
  if (const int indicators = IndicatorWidth(data); indicators > 0) {
    width += kChipGap + indicators;
  }
  // 托盘里（当前标签挂在后面）不写数量。
  if (!data.active && data.count > 0) {
    width += kChipGap + gfx::GetStringWidth(base::NumberToString16(data.count),
                                            UiFont(11));
  }
  width += data.active ? kChipTrayTrailing : kChipTrailing;
  return std::min(width, kChipMaxWidth);
}

gfx::Rect TabListRow::TileRect() const {
  if (data_.compact) {
    return GetLocalBounds();
  }
  return gfx::Rect(0, 0, width(),
                   std::min(height(), metrics::kPinnedTileHeight));
}

gfx::Point TabListRow::IconCenter() const {
  if (data_.kind == TabListRowData::Kind::kTile) {
    return TileRect().CenterPoint();
  }
  if (data_.kind == TabListRowData::Kind::kChip) {
    return data_.compact || data_.id == kMorePinsId
               ? gfx::Point(width() / 2, height() / 2)
               : gfx::Point(kChipLeading + kIconSize / 2, height() / 2);
  }
  return gfx::Point(kLeading + kIconSize / 2, height() / 2);
}

// static
int TabListRow::IndentFor(int depth) {
  return kIndent * std::clamp(depth, 0, kMaxIndentDepth);
}

void TabListRow::SetData(const TabListRowData& data, const Theme& theme) {
  data_ = data;
  theme_ = theme;
  EnsureButtons();
  UpdateButtons();
  SetCanProcessEventsWithinSubtree(interactive() && !leaving_);
  SetFocusBehavior(interactive() ? FocusBehavior::ALWAYS
                                 : FocusBehavior::NEVER);

  views::ViewAccessibility& ax = GetViewAccessibility();
  switch (data_.kind) {
    case TabListRowData::Kind::kLabel:
    case TabListRowData::Kind::kFolder:
      ax.SetRole(ax::mojom::Role::kButton);
      if (data_.collapsed) {
        ax.SetIsCollapsed();
      } else {
        ax.SetIsExpanded();
      }
      break;
    case TabListRowData::Kind::kNewTab:
    case TabListRowData::Kind::kHoist:
      ax.SetRole(ax::mojom::Role::kButton);
      break;
    case TabListRowData::Kind::kChip:
    case TabListRowData::Kind::kTile:
      ax.SetRole(ax::mojom::Role::kPopUpButton);
      if (data_.open) {
        ax.SetIsExpanded();
      } else {
        ax.SetIsCollapsed();
      }
      break;
    case TabListRowData::Kind::kEmpty:
      ax.SetRole(ax::mojom::Role::kStaticText);
      break;
    case TabListRowData::Kind::kDivider:
      ax.SetRole(ax::mojom::Role::kSplitter);
      break;
  }
  if (!data_.title.empty()) {
    ax.SetName(data_.title);
  }
  // 读屏：「工作，文件夹，已折叠，6 个标签页」（有声音、智能体时接着说）。
  const bool folder =
      data_.kind == TabListRowData::Kind::kFolder ||
      data_.kind == TabListRowData::Kind::kTile ||
      (data_.kind == TabListRowData::Kind::kChip && data_.id != kMorePinsId);
  if (folder) {
    ax.SetRoleDescription(L("tabs.folder.role"));
    std::vector<std::u16string> parts{
        L("tabs.folder.count", {{"count", data_.count}})};
    if (data_.audible) {
      parts.push_back(L("tabs.folder.audible"));
    }
    if (data_.agent) {
      parts.push_back(L("tabs.folder.agent"));
    }
    ax.SetDescription(l10n::JoinList(parts));
  } else if (data_.kind == TabListRowData::Kind::kHoist) {
    // 读屏：「维基百科，按钮，只看这一组，4 个标签页」，按下是显示全部。
    ax.SetRoleDescription(L("tabs.tree.hoisted"));
    ax.SetDescription(
        l10n::JoinList({L("tabs.folder.count", {{"count", data_.count}}),
                        L("tabs.tree.showAll")}));
  } else {
    ax.RemoveRoleDescription();
    ax.RemoveDescription();
  }
  if (field_) {
    field_->SetColor(theme_.on_surface);
  }
  InvalidateLayout();
  SchedulePaint();
}

void TabListRow::BeginEdit() {
  if (field_ || leaving_ || data_.kind != TabListRowData::Kind::kFolder) {
    return;
  }
  auto field = std::make_unique<RowTextfield>(this);
  field->SetController(this);
  field->SetBackgroundEnabled(false);
  field->SetBorder(nullptr);
  field->SetFontList(UiFont(13, gfx::Font::Weight::MEDIUM));
  field->SetColor(theme_.on_surface);
  field->SetText(data_.title);
  field->GetViewAccessibility().SetName(L("tabs.renameFolder"));
  field_ = AddChildView(std::move(field));
  UpdateButtons();
  InvalidateLayout();
  SchedulePaint();
  field_->SelectAll(false);
  field_->RequestFocus();
}

void TabListRow::FinishEditForTesting(std::optional<std::u16string> text) {
  if (!field_) {
    return;
  }
  if (text) {
    field_->SetText(*text);
  }
  FinishEdit(text.has_value());
}

void TabListRow::SetLeaving() {
  leaving_ = true;
  SetCanProcessEventsWithinSubtree(false);
  // 有焦点时交给下一个。
  SetFocusBehavior(FocusBehavior::NEVER);
  // 行没了：输入到一半的名字不要了。
  if (field_) {
    auto* field = static_cast<RowTextfield*>(field_.get());
    field_ = nullptr;
    field->Detach();
    field->SetController(nullptr);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, RemoveChildViewT(field));
  }
  SetHovered(false);
  UpdateButtons();
}

void TabListRow::SetLifted(bool lifted) {
  if (lifted_ == lifted || leaving_) {
    return;
  }
  lifted_ = lifted;
  // 拖起来以后鼠标的进出收不到了：悬停的样子（箭头、按钮）先收掉。
  if (lifted) {
    SetHovered(false);
  }
  motion_.AnimateOpacity(layer(), lifted ? motion::kTabLiftedOpacity : 1,
                         motion::Hover());
}

void TabListRow::SetDropTarget(bool target) {
  if (drop_target_ == target) {
    return;
  }
  drop_target_ = target;
  if (target) {
    drop_animation_.Show();
  } else {
    drop_animation_.Hide();
  }
}

bool TabListRow::interactive() const {
  return data_.kind == TabListRowData::Kind::kLabel ||
         data_.kind == TabListRowData::Kind::kFolder ||
         data_.kind == TabListRowData::Kind::kNewTab ||
         data_.kind == TabListRowData::Kind::kChip ||
         data_.kind == TabListRowData::Kind::kTile ||
         data_.kind == TabListRowData::Kind::kHoist;
}

void TabListRow::EnsureButtons() {
  const SkColor tint = theme_.on_surface_variant;
  const SkColor fill = theme_.OnSurface(0.1f);
  if (data_.kind == TabListRowData::Kind::kLabel && !new_folder_button_) {
    new_folder_button_ = AddChildView(std::make_unique<RowActionButton>(
        base::BindRepeating(
            [](TabListRow* row) {
              row->delegate_->TabFolderAction("@newFolder", std::nullopt);
            },
            base::Unretained(this)),
        icons::kFolderBadgePlus, 14, kActionSize));
    new_folder_button_->SetTooltipText(L("tabs.newFolder"));
    new_folder_button_->GetViewAccessibility().SetName(L("tabs.newFolder"));
    new_folder_button_->SetFillRadius(radius::kButton);
  }
  if (data_.kind == TabListRowData::Kind::kFolder && !add_button_) {
    add_button_ = AddChildView(std::make_unique<RowActionButton>(
        base::BindRepeating(
            [](TabListRow* row) {
              row->delegate_->TabFolderAddCurrent(row->data_.id);
            },
            base::Unretained(this)),
        icons::kPlus, 11, kActionSize));
    add_button_->SetTooltipText(L("tabs.addCurrentToFolder"));
    add_button_->GetViewAccessibility().SetName(L("tabs.addCurrentToFolder"));
    add_button_->SetFillRadius(radius::kButton);
    more_button_ = AddChildView(std::make_unique<RowActionButton>(
        base::BindRepeating(&TabListRow::ShowMoreMenu, base::Unretained(this)),
        icons::kEllipsis, 13, kActionSize));
    more_button_->SetTooltipText(L("common.moreActions"));
    more_button_->GetViewAccessibility().SetName(L("common.moreActions"));
    more_button_->SetFillRadius(radius::kButton);
  }
  for (GlyphButton* button :
       {new_folder_button_.get(), add_button_.get(), more_button_.get()}) {
    if (button) {
      button->SetColors(tint, fill);
    }
  }
}

void TabListRow::UpdateButtons() {
  const float hover = static_cast<float>(hover_animation_.GetCurrentValue());
  const bool shown =
      !leaving_ && !field_ && (hovered_ || hover_animation_.is_animating());
  for (GlyphButton* button :
       {new_folder_button_.get(), add_button_.get(), more_button_.get()}) {
    if (button) {
      button->SetVisible(shown);
      button->SetAlpha(hover);
    }
  }
}

void TabListRow::SetHovered(bool hovered) {
  if (leaving_) {
    hovered = false;
  }
  if (hovered_ == hovered) {
    return;
  }
  hovered_ = hovered;
  if (hovered) {
    hover_animation_.Show();
  } else {
    hover_animation_.Hide();
  }
  UpdateButtons();
  InvalidateLayout();
}

int TabListRow::StackWidth() const {
  const int shown = static_cast<int>(std::min(data_.icons.size(), kStackMax));
  if (shown == 0) {
    return 0;
  }
  int width = kStackIcon + (shown - 1) * kStackStep;
  if (data_.count > shown) {
    width +=
        kStackCountGap +
        gfx::GetStringWidth(u"+" + base::NumberToString16(data_.count - shown),
                            UiFont(10));
  }
  return width;
}

// static
int TabListRow::IndicatorWidth(const TabListRowData& data) {
  const int count = (data.audible ? 1 : 0) + (data.agent ? 1 : 0);
  return count == 0 ? 0 : count * kIndicatorSize + (count - 1) * kIndicatorGap;
}

int TabListRow::FoldedTrailingWidth() const {
  int width = StackWidth();
  if (const int indicators = IndicatorWidth(data_); indicators > 0) {
    width += indicators + (width > 0 ? kStackCountGap + 2 : 0);
  }
  return width;
}

void TabListRow::PaintIndicators(gfx::Canvas* canvas, int x, float alpha) {
  const int top = height() / 2 - kIndicatorSize / 2;
  if (data_.agent) {
    PaintIconAt(canvas, icons::kSparkle, kIndicatorSize,
                WithAlpha(theme_.accent, alpha), gfx::Point(x, top));
    x += kIndicatorSize + kIndicatorGap;
  }
  if (data_.audible) {
    PaintIconAt(canvas, icons::kSpeakerWave, kIndicatorSize,
                WithAlpha(theme_.on_surface_variant, alpha),
                gfx::Point(x, top));
  }
}

void TabListRow::Layout(PassKey) {
  const int mid = height() / 2;
  const int title_x = kLeading + kIconSize + kSpacing;
  switch (data_.kind) {
    case TabListRowData::Kind::kLabel:
      if (new_folder_button_) {
        new_folder_button_->SetBounds(
            width() - 4 - kActionSize,
            height() - kLabelTextHeight + (kLabelTextHeight - kActionSize) / 2,
            kActionSize, kActionSize);
      }
      title_bounds_ = gfx::Rect();
      break;
    case TabListRowData::Kind::kFolder: {
      const int right = width() - kTrailing;
      if (more_button_) {
        more_button_->SetBounds(right - kActionSize, mid - kActionSize / 2,
                                kActionSize, kActionSize);
        add_button_->SetBounds(right - 2 * kActionSize - kActionGap,
                               mid - kActionSize / 2, kActionSize, kActionSize);
      }
      int trailing = 0;
      if (hovered_) {
        trailing = 2 * kActionSize + kActionGap;
      } else if (data_.collapsed) {
        trailing = FoldedTrailingWidth() + 2;
      }
      const int end = right - trailing - (trailing > 0 ? kSpacing - 2 : 0);
      title_bounds_ =
          gfx::Rect(title_x, 0, std::max(0, end - title_x), height());
      if (field_) {
        field_->SetBounds(title_x, mid - kFieldHeight / 2,
                          std::max(0, right - kFieldOutsetX - title_x),
                          kFieldHeight);
      }
      break;
    }
    case TabListRowData::Kind::kNewTab: {
      const gfx::Rect organize = OrganizeRect();
      const int right =
          organize.IsEmpty() ? width() - kTrailing : organize.x() - 6;
      title_bounds_ =
          gfx::Rect(title_x, 0, std::max(0, right - title_x), height());
      break;
    }
    case TabListRowData::Kind::kChip: {
      if (data_.compact || data_.id == kMorePinsId) {
        title_bounds_ = gfx::Rect();
        break;
      }
      int right = width() - (data_.active ? kChipTrayTrailing : kChipTrailing);
      if (!data_.active && data_.count > 0) {
        right -=
            kChipGap + gfx::GetStringWidth(base::NumberToString16(data_.count),
                                           UiFont(11));
      }
      if (const int indicators = IndicatorWidth(data_); indicators > 0) {
        right -= kChipGap + indicators;
      }
      const int x = kChipLeading + kIconSize + kChipGap;
      title_bounds_ = gfx::Rect(x, 0, std::max(0, right - x), height());
      break;
    }
    case TabListRowData::Kind::kTile: {
      const gfx::Rect tile = TileRect();
      title_bounds_ = data_.compact
                          ? gfx::Rect()
                          : gfx::Rect(0, tile.bottom(), width(),
                                      std::max(0, height() - tile.bottom()));
      break;
    }
    case TabListRowData::Kind::kEmpty:
    case TabListRowData::Kind::kDivider:
    case TabListRowData::Kind::kHoist:
      title_bounds_ = gfx::Rect();
      break;
  }
}

void TabListRow::OnPaint(gfx::Canvas* canvas) {
  const float hover = static_cast<float>(hover_animation_.GetCurrentValue());
  const float drop = static_cast<float>(drop_animation_.GetCurrentValue());
  const int mid = height() / 2;
  switch (data_.kind) {
    case TabListRowData::Kind::kLabel: {
      if (drop > 0) {
        PaintDropOutline(canvas, theme_,
                         gfx::RectF(2, height() - kLabelTextHeight,
                                    std::max(0, width() - 4), kLabelTextHeight),
                         radius::kButton, drop);
      }
      std::u16string text = data_.title;
      if (data_.collapsed && data_.count > 0) {
        text += u" · " + base::NumberToString16(data_.count);
      }
      canvas->DrawStringRectWithFlags(
          text, UiFont(11, gfx::Font::Weight::SEMIBOLD),
          Theme::LerpColor(WithAlpha(theme_.on_surface_variant, 0.9f),
                           theme_.on_surface, hover),
          gfx::Rect(kLeading, height() - kLabelTextHeight,
                    std::max(0, width() - kLeading - kActionSize - 8),
                    kLabelTextHeight),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
      break;
    }
    case TabListRowData::Kind::kFolder:
      PaintFolder(canvas, hover);
      break;
    case TabListRowData::Kind::kChip:
      PaintChip(canvas, hover);
      break;
    case TabListRowData::Kind::kTile:
      PaintTile(canvas, hover);
      break;
    case TabListRowData::Kind::kHoist:
      PaintHoist(canvas, hover);
      break;
    case TabListRowData::Kind::kEmpty: {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setStyle(cc::PaintFlags::kStroke_Style);
      flags.setStrokeWidth(1);
      flags.setColor(Theme::LerpColor(theme_.OnSurface(0.1f),
                                      WithAlpha(theme_.accent, 0.7f), drop));
      gfx::RectF stroke(GetLocalBounds());
      stroke.Inset(0.5f);
      if (drop > 0) {
        cc::PaintFlags fill;
        fill.setAntiAlias(true);
        fill.setColor(WithAlpha(theme_.accent, 0.1f * drop));
        canvas->DrawRoundRect(stroke, radius::kRow - 1.5f, fill);
      }
      canvas->DrawRoundRect(stroke, radius::kRow - 1.5f, flags);
      canvas->DrawStringRectWithFlags(
          data_.title, UiFont(12),
          Theme::LerpColor(WithAlpha(theme_.on_surface_variant, 0.85f),
                           theme_.accent, drop),
          gfx::Rect(kLeading, 0, std::max(0, width() - 2 * kLeading), height()),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
      break;
    }
    case TabListRowData::Kind::kDivider:
      canvas->FillRect(gfx::Rect(kDividerInset, mid,
                                 std::max(0, width() - 2 * kDividerInset), 1),
                       theme_.OnSurface(0.08f));
      break;
    case TabListRowData::Kind::kNewTab: {
      if (hover > 0) {
        cc::PaintFlags flags;
        flags.setAntiAlias(true);
        flags.setColor(theme_.OnSurface(0.05f * hover));
        canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kRow,
                              flags);
      }
      PaintIconAt(canvas, icons::kPlus, kPlusSize, theme_.on_surface_variant,
                  gfx::Point(kLeading + (kIconSize - kPlusSize) / 2,
                             mid - kPlusSize / 2));
      canvas->DrawStringRectWithFlags(data_.title, UiFont(13),
                                      theme_.on_surface_variant, title_bounds_,
                                      gfx::Canvas::NO_SUBPIXEL_RENDERING);
      // 「✦ 整理」：标签多了时一直在（强调色的底），平时悬停才淡入。
      if (const gfx::Rect pill = OrganizeRect(); !pill.IsEmpty()) {
        const float shown = data_.organize_hint ? 1.0f : hover;
        cc::PaintFlags flags;
        flags.setAntiAlias(true);
        flags.setColor(data_.organize_hint
                           ? WithAlpha(theme_.accent,
                                       (theme_.is_dark ? 0.16f : 0.12f) +
                                           (organize_hovered_ ? 0.08f : 0.0f))
                           : theme_.OnSurface(
                                 (organize_hovered_ ? 0.11f : 0.07f) * shown));
        canvas->DrawRoundRect(gfx::RectF(pill), pill.height() / 2.0f, flags);
        constexpr int kSparkle = 11;
        PaintIconAt(
            canvas, icons::kSparkle, kSparkle, WithAlpha(theme_.accent, shown),
            gfx::Point(pill.x() + 7, pill.CenterPoint().y() - kSparkle / 2));
        canvas->DrawStringRectWithFlags(
            OrganizeText(), UiFont(11, gfx::Font::Weight::MEDIUM),
            WithAlpha(data_.organize_hint ? theme_.on_surface
                                          : theme_.on_surface_variant,
                      shown),
            gfx::Rect(pill.x() + 7 + kSparkle + 4, pill.y(),
                      std::max(0, pill.width() - 7 - kSparkle - 4 - 8),
                      pill.height()),
            gfx::Canvas::NO_SUBPIXEL_RENDERING);
      }
      break;
    }
  }
  if (HasFocus()) {
    gfx::RectF ring(GetLocalBounds());
    if (data_.kind == TabListRowData::Kind::kLabel) {
      ring = gfx::RectF(2, height() - kLabelTextHeight,
                        std::max(0, width() - 4), kLabelTextHeight);
    } else if (data_.kind == TabListRowData::Kind::kTile) {
      ring = gfx::RectF(TileRect());
    }
    PaintFocusRing(canvas, theme_, ring,
                   data_.kind == TabListRowData::Kind::kLabel ? radius::kButton
                                                              : radius::kRow);
  }
}

void TabListRow::PaintHoist(gfx::Canvas* canvas, float hover) {
  const int mid = height() / 2;
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  // 聚焦着的那个标签是当前标签：底色同选中的高亮。
  fill.setColor(data_.active
                    ? WithAlpha(theme_.accent, theme_.is_dark ? 0.16f : 0.12f)
                    : theme_.OnSurface(0.06f + 0.04f * hover));
  canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kRow, fill);
  PaintIconAt(
      canvas, icons::kChevronLeft, kHoistBackIcon,
      Theme::LerpColor(theme_.on_surface_variant, theme_.on_surface, hover),
      gfx::Point(kHoistLeading + (kHoistBack - kHoistBackIcon) / 2,
                 mid - kHoistBackIcon / 2));
  int x = kHoistLeading + kHoistBack + kHoistGap;
  const ui::ColorProvider* provider = GetColorProvider();
  if (!data_.icons.empty() && !data_.icons[0].IsEmpty() && provider) {
    const gfx::ImageSkia image = data_.icons[0].Rasterize(provider);
    if (!image.isNull()) {
      canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), x,
                           mid - kIconSize / 2, kIconSize, kIconSize, true);
    }
  }
  x += kIconSize + kHoistGap;
  int right = width() - kHoistTrailing;
  if (data_.count > 0) {
    const std::u16string count = base::NumberToString16(data_.count);
    const gfx::FontList font = UiFont(10, gfx::Font::Weight::SEMIBOLD);
    const int count_width =
        std::max(kHoistCountMinWidth, gfx::GetStringWidth(count, font) + 10);
    const gfx::Rect pill(right - count_width, mid - kHoistCountHeight / 2,
                         count_width, kHoistCountHeight);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme_.OnSurface(0.1f));
    canvas->DrawRoundRect(gfx::RectF(pill), kHoistCountHeight / 2.0f, flags);
    canvas->DrawStringRectWithFlags(
        count, font, theme_.on_surface_variant, pill,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    right = pill.x() - kHoistGap;
  }
  // 宽的时候名字前面写上一层（更上面的是「…」）。
  if (!data_.path.empty() && width() >= kHoistCrumbMinWidth) {
    const gfx::FontList font = UiFont(12);
    const std::u16string tail = u" /";
    const std::u16string head = data_.path.size() > 1 ? u"\u2026 / " : u"";
    const int limit = std::max(0, right - x) / 3;
    const int fixed = gfx::GetStringWidth(head + tail, font);
    const std::u16string name = gfx::ElideText(
        data_.path.back(), font, std::max(0, limit - fixed), gfx::ELIDE_TAIL);
    const std::u16string crumb = head + name + tail;
    const int crumb_width = gfx::GetStringWidth(crumb, font);
    canvas->DrawStringRectWithFlags(crumb, font, theme_.on_surface_variant,
                                    gfx::Rect(x, 0, crumb_width, height()),
                                    gfx::Canvas::NO_SUBPIXEL_RENDERING);
    x += crumb_width + 5;
  }
  canvas->DrawStringRectWithFlags(
      data_.title, UiFont(13, gfx::Font::Weight::SEMIBOLD), theme_.on_surface,
      gfx::Rect(x, 0, std::max(0, right - x), height()),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
}

void TabListRow::PaintFolder(gfx::Canvas* canvas, float hover) {
  const bool editing = field_ != nullptr;
  const float drop = static_cast<float>(drop_animation_.GetCurrentValue());
  const int mid = height() / 2;
  if (hover > 0 && !editing) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme_.OnSurface(0.05f * hover));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kRow, flags);
  }
  if (drop > 0) {
    PaintDropOutline(canvas, theme_, gfx::RectF(GetLocalBounds()), radius::kRow,
                     drop);
  }
  // 图标：平时是文件夹（开着的、合着的），悬停时换成箭头；拖着的东西会放进来时
  // 是打开的文件夹，强调色，放大一点。
  const SkColor tint = Theme::LerpColor(
      data_.tint.value_or(theme_.on_surface_variant), theme_.accent, drop);
  const float chevron = editing ? 0 : hover * (1 - drop);
  const gfx::Point icon_origin(kLeading, mid - kIconSize / 2);
  if (chevron < 1) {
    const bool open = !data_.collapsed || drop > 0.5f;
    canvas->Save();
    if (drop > 0) {
      const float scale = 1 + (motion::kFolderDropIconScale - 1) * drop;
      const gfx::Vector2d center(kLeading + kIconSize / 2, mid);
      canvas->Translate(center);
      canvas->Scale(scale, scale);
      canvas->Translate(-center);
    }
    PaintIconAt(canvas, open ? icons::kFolderOpen : icons::kFolder, kIconSize,
                WithAlpha(tint, 1 - chevron), icon_origin);
    canvas->Restore();
  }
  if (chevron > 0) {
    const int inset = (kIconSize - kChevronSize) / 2;
    PaintIconAt(canvas,
                data_.collapsed ? icons::kChevronRight : icons::kChevronDown,
                kChevronSize, WithAlpha(tint, chevron),
                icon_origin + gfx::Vector2d(inset, inset));
  }
  if (editing) {
    gfx::RectF box(field_->bounds());
    box.Outset(gfx::OutsetsF::VH(kFieldOutsetY, kFieldOutsetX));
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(theme_.OnSurface(0.08f));
    canvas->DrawRoundRect(box, radius::kButton, fill);
    cc::PaintFlags outline;
    outline.setAntiAlias(true);
    outline.setStyle(cc::PaintFlags::kStroke_Style);
    outline.setStrokeWidth(1.5f);
    outline.setColor(WithAlpha(theme_.accent, 0.6f));
    box.Inset(0.75f);
    canvas->DrawRoundRect(box, radius::kButton - 0.75f, outline);
    return;
  }
  canvas->DrawStringRectWithFlags(data_.title,
                                  UiFont(13, gfx::Font::Weight::MEDIUM),
                                  theme_.OnSurface(0.92f), title_bounds_,
                                  gfx::Canvas::NO_SUBPIXEL_RENDERING);
  if (data_.collapsed && !data_.icons.empty() && hover < 1) {
    PaintStack(canvas, 1 - hover);
  }
  if (data_.collapsed && hover < 1 && IndicatorWidth(data_) > 0) {
    PaintIndicators(canvas, width() - kTrailing - 2 - FoldedTrailingWidth(),
                    1 - hover);
  }
}

void TabListRow::PaintChip(gfx::Canvas* canvas, float hover) {
  const float drop = static_cast<float>(drop_animation_.GetCurrentValue());
  const bool more = data_.id == kMorePinsId;
  const int mid = height() / 2;
  // 托盘提供普通状态的底色。
  const float fill = std::max(0.08f * hover, data_.open ? 0.1f : 0.0f);
  if (fill > 0) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme_.OnSurface(fill));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kRow, flags);
  }
  if (drop > 0) {
    PaintDropOutline(canvas, theme_, gfx::RectF(GetLocalBounds()), radius::kRow,
                     drop);
  }
  const float strong =
      std::max({hover, data_.open ? 1.0f : 0.0f, data_.active ? 1.0f : 0.0f});
  const SkColor text =
      Theme::LerpColor(theme_.on_surface_variant, theme_.on_surface, strong);
  if (more) {
    PaintIconAt(
        canvas, icons::kEllipsis, kChipMoreGlyph,
        Theme::LerpColor(text, theme_.accent, drop),
        gfx::Point((width() - kChipMoreGlyph) / 2, mid - kChipMoreGlyph / 2));
    return;
  }
  // 图标：拖着的东西会放进来、卡片开着时是打开的文件夹；放进来时强调色、
  // 放大一点。
  const gfx::Point center = IconCenter();
  canvas->Save();
  if (drop > 0) {
    const float scale = 1 + (motion::kFolderDropIconScale - 1) * drop;
    canvas->Translate(center.OffsetFromOrigin());
    canvas->Scale(scale, scale);
    canvas->Translate(-center.OffsetFromOrigin());
  }
  PaintIconAt(canvas,
              data_.open || drop > 0.5f ? icons::kFolderOpen : icons::kFolder,
              kIconSize,
              Theme::LerpColor(data_.tint.value_or(theme_.on_surface_variant),
                               theme_.accent, drop),
              gfx::Point(center.x() - kIconSize / 2, mid - kIconSize / 2));
  canvas->Restore();
  if (title_bounds_.IsEmpty()) {
    return;
  }
  canvas->DrawStringRectWithFlags(data_.title, UiFont(kChipFontSize), text,
                                  title_bounds_,
                                  gfx::Canvas::NO_SUBPIXEL_RENDERING);
  if (const int indicators = IndicatorWidth(data_); indicators > 0) {
    int x = width() - (data_.active ? kChipTrayTrailing : kChipTrailing);
    if (!data_.active && data_.count > 0) {
      x -= kChipGap +
           gfx::GetStringWidth(base::NumberToString16(data_.count), UiFont(11));
    }
    PaintIndicators(canvas, x - indicators, 1);
  }
  if (!data_.active && data_.count > 0) {
    const std::u16string count = base::NumberToString16(data_.count);
    const int count_width = gfx::GetStringWidth(count, UiFont(11));
    canvas->DrawStringRectWithFlags(
        count, UiFont(11), WithAlpha(theme_.on_surface_variant, 0.8f),
        gfx::Rect(width() - kChipTrailing - count_width, 0, count_width,
                  height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
}

void TabListRow::PaintTile(gfx::Canvas* canvas, float hover) {
  const float drop = static_cast<float>(drop_animation_.GetCurrentValue());
  const gfx::Rect tile = TileRect();
  const gfx::RectF box(tile);
  // 底：同常用的图块；选了颜色的染一层淡淡的颜色；当前标签在里面时同选中的
  // 图块，卡片开着时再描一圈强调色。
  SkColor fill = theme_.OnSurface(0.06f + 0.04f * hover);
  if (data_.tint) {
    fill =
        WithAlpha(*data_.tint, (theme_.is_dark ? 0.2f : 0.16f) + 0.06f * hover);
  }
  if (data_.active || data_.open) {
    fill = WithAlpha(theme_.accent, theme_.is_dark ? 0.2f : 0.14f);
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(fill);
  canvas->DrawRoundRect(box, radius::kRow, flags);
  if (data_.open) {
    cc::PaintFlags ring;
    ring.setAntiAlias(true);
    ring.setStyle(cc::PaintFlags::kStroke_Style);
    ring.setStrokeWidth(1.5f);
    ring.setColor(WithAlpha(theme_.accent, 0.5f));
    gfx::RectF stroke = box;
    stroke.Inset(0.75f);
    canvas->DrawRoundRect(stroke, radius::kRow - 0.75f, ring);
  }
  if (drop > 0) {
    PaintDropOutline(canvas, theme_, box, radius::kRow, drop);
  }

  const ui::ColorProvider* provider = GetColorProvider();
  const int size = data_.compact ? kTopTileIcon : kTileIcon;
  const int grid = 2 * size + kTileIconGap;
  const gfx::Point origin(tile.x() + (tile.width() - grid) / 2,
                          tile.y() + (tile.height() - grid) / 2);
  canvas->Save();
  if (drop > 0) {
    const float scale = 1 + (motion::kFolderDropIconScale - 1) * drop;
    const gfx::Vector2d center = tile.CenterPoint().OffsetFromOrigin();
    canvas->Translate(center);
    canvas->Scale(scale, scale);
    canvas->Translate(-center);
  }
  for (size_t i = 0; i < 4; ++i) {
    const gfx::Rect rect(
        origin.x() + static_cast<int>(i % 2) * (size + kTileIconGap),
        origin.y() + static_cast<int>(i / 2) * (size + kTileIconGap), size,
        size);
    gfx::ImageSkia image;
    if (i < data_.icons.size() && !data_.icons[i].IsEmpty() && provider) {
      image = data_.icons[i].Rasterize(provider);
    }
    if (image.isNull()) {
      cc::PaintFlags empty;
      empty.setAntiAlias(true);
      empty.setColor(theme_.OnSurface(i < data_.icons.size() ? 0.22f : 0.08f));
      canvas->DrawRoundRect(gfx::RectF(rect), kTileIconRadius, empty);
      continue;
    }
    canvas->Save();
    canvas->sk_canvas()->clipRRect(
        SkRRect::MakeRectXY(gfx::RectToSkRect(rect), kTileIconRadius,
                            kTileIconRadius),
        true);
    canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), rect.x(),
                         rect.y(), rect.width(), rect.height(), true);
    canvas->Restore();
  }
  canvas->Restore();

  // 右上角：智能体在操作、有声音（同标签的图块，只放一个）。
  if (data_.agent || data_.audible) {
    const int badge = data_.compact ? kTopTileBadge : kTileBadge;
    PaintIconAt(canvas, data_.agent ? icons::kSparkle : icons::kSpeakerWave,
                badge, data_.agent ? theme_.accent : theme_.on_surface_variant,
                gfx::Point(tile.right() - kTileBadgeInset - badge,
                           tile.y() + kTileBadgeInset));
  }
  if (!title_bounds_.IsEmpty()) {
    canvas->DrawStringRectWithFlags(
        data_.title, UiFont(10),
        data_.active || data_.open ? theme_.on_surface
                                   : theme_.on_surface_variant,
        title_bounds_,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
}

void TabListRow::PaintStack(gfx::Canvas* canvas, float alpha) {
  const ui::ColorProvider* provider = GetColorProvider();
  if (!provider) {
    return;
  }
  const int shown = static_cast<int>(std::min(data_.icons.size(), kStackMax));
  const int mid = height() / 2;
  int right = width() - kTrailing - 2;
  const int stack_width = StackWidth();
  // 单独一层：后一个图标外面的一圈空是在这一层上挖掉的，露出侧栏的底。
  canvas->SaveLayerAlpha(
      static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255),
      gfx::Rect(right - stack_width - 2, 0, stack_width + 4, height()));
  if (data_.count > shown) {
    const std::u16string more =
        u"+" + base::NumberToString16(data_.count - shown);
    const int text_width = gfx::GetStringWidth(more, UiFont(10));
    canvas->DrawStringRectWithFlags(
        more, UiFont(10), theme_.on_surface_variant,
        gfx::Rect(right - text_width, 0, text_width, height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    right -= text_width + kStackCountGap;
  }
  int x = right - (kStackIcon + (shown - 1) * kStackStep);
  for (int i = 0; i < shown; ++i, x += kStackStep) {
    const gfx::Rect rect(x, mid - kStackIcon / 2, kStackIcon, kStackIcon);
    if (i > 0) {
      cc::PaintFlags clear;
      clear.setAntiAlias(true);
      clear.setBlendMode(SkBlendMode::kClear);
      gfx::RectF gap(rect);
      gap.Outset(kStackGap);
      canvas->DrawRoundRect(gap, 4 + kStackGap, clear);
    }
    const ui::ImageModel& icon = data_.icons[i];
    const gfx::ImageSkia image =
        icon.IsEmpty() ? gfx::ImageSkia() : icon.Rasterize(provider);
    if (image.isNull()) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(0.22f));
      canvas->DrawRoundRect(gfx::RectF(rect), 4, flags);
      continue;
    }
    canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), rect.x(),
                         rect.y(), rect.width(), rect.height(), true);
  }
  canvas->Restore();
}

bool TabListRow::OnMousePressed(const ui::MouseEvent& event) {
  if (!interactive() || leaving_) {
    return false;
  }
  middle_pressed_ =
      event.IsOnlyMiddleMouseButton() && !field_ &&
      (data_.kind == TabListRowData::Kind::kFolder ||
       data_.kind == TabListRowData::Kind::kTile ||
       (data_.kind == TabListRowData::Kind::kChip && data_.id != kMorePinsId));
  if (middle_pressed_) {
    return true;
  }
  pressed_ = event.IsOnlyLeftMouseButton();
  dragging_ = false;
  press_location_.reset();
  if (pressed_) {
    press_location_ = event.location();
  }
  return pressed_;
}

bool TabListRow::OnMouseDragged(const ui::MouseEvent& event) {
  const bool draggable =
      data_.kind == TabListRowData::Kind::kFolder ||
      data_.kind == TabListRowData::Kind::kTile ||
      (data_.kind == TabListRowData::Kind::kChip && data_.id != kMorePinsId);
  if (!pressed_ || !press_location_ || field_ || !draggable) {
    return pressed_;
  }
  if (!dragging_) {
    const gfx::Vector2d moved = event.location() - *press_location_;
    if (std::abs(moved.x()) < kDragThreshold &&
        std::abs(moved.y()) < kDragThreshold) {
      return true;
    }
    dragging_ = true;
    delegate_->TabRowDragStarted(this, *press_location_);
  }
  delegate_->TabRowDragged(this, event.location());
  return true;
}

void TabListRow::OnMouseReleased(const ui::MouseEvent& event) {
  if (std::exchange(middle_pressed_, false)) {
    if (HitTestPoint(event.location())) {
      delegate_->TabFolderOpenAll(data_.id);
    }
    return;
  }
  if (!pressed_) {
    return;
  }
  pressed_ = false;
  press_location_.reset();
  if (dragging_) {
    dragging_ = false;
    delegate_->TabRowDragEnded(this, /*cancelled=*/false);
    return;
  }
  if (HitTestPoint(event.location())) {
    Activate(event);
  }
}

void TabListRow::OnMouseCaptureLost() {
  pressed_ = false;
  middle_pressed_ = false;
  press_location_.reset();
  if (dragging_) {
    dragging_ = false;
    delegate_->TabRowDragEnded(this, /*cancelled=*/true);
  }
}

void TabListRow::OnMouseEntered(const ui::MouseEvent& event) {
  SetHovered(true);
  OnMouseMoved(event);
}

void TabListRow::OnMouseMoved(const ui::MouseEvent& event) {
  const bool over = OrganizeRect().Contains(event.location());
  if (over != organize_hovered_) {
    organize_hovered_ = over;
    SchedulePaint();
  }
}

void TabListRow::OnMouseExited(const ui::MouseEvent& event) {
  organize_hovered_ = false;
  SetHovered(false);
}

gfx::Rect TabListRow::OrganizeRect() const {
  if (data_.kind != TabListRowData::Kind::kNewTab || data_.organize < 2 ||
      (!data_.organize_hint && !hovered_ && !hover_animation_.is_animating())) {
    return gfx::Rect();
  }
  constexpr int kHeight = 22;
  const int width = 7 + 11 + 4 +
                    gfx::GetStringWidth(OrganizeText(),
                                        UiFont(11, gfx::Font::Weight::MEDIUM)) +
                    8;
  return gfx::Rect(this->width() - kTrailing - width, (height() - kHeight) / 2,
                   width, kHeight);
}

std::u16string TabListRow::OrganizeText() const {
  return data_.organize_hint
             ? L("organize.pill.count", {{"count", data_.organize}})
             : L("organize.pill");
}

std::u16string TabListRow::GetRenderedTooltipText(
    const gfx::Point& point) const {
  // 只有图标的胶囊、图块（侧栏里的名字可能写不下）：提示名字。
  if ((data_.kind == TabListRowData::Kind::kChip &&
       (data_.compact || data_.id == kMorePinsId)) ||
      data_.kind == TabListRowData::Kind::kTile) {
    return data_.title;
  }
  if (OrganizeRect().Contains(point)) {
    return L("organize.pill.help");
  }
  // 面包屑：整条路径，下面一行是点了做什么。
  if (data_.kind == TabListRowData::Kind::kHoist) {
    std::u16string path;
    for (const std::u16string& title : data_.path) {
      path += title + u" / ";
    }
    return path + data_.title + u"\n" + L("tabs.tree.showAll");
  }
  return std::u16string();
}

bool TabListRow::OnKeyPressed(const ui::KeyEvent& event) {
  if (!interactive() || field_ || leaving_) {
    return false;
  }
  // 图块：回车、空格落下卡片，焦点进到卡片里（TabsView 管）。
  if (data_.kind == TabListRowData::Kind::kTile) {
    return delegate_->TabListKeyPressed(this, event);
  }
  if (event.key_code() == ui::VKEY_RETURN ||
      event.key_code() == ui::VKEY_SPACE) {
    delegate_->TabFolderAction(data_.id, std::nullopt);
    return true;
  }
  return delegate_->TabListKeyPressed(this, event);
}

void TabListRow::OnFocus() {
  views::View::OnFocus();
  ScrollViewToVisible();
  SchedulePaint();
}

void TabListRow::OnBlur() {
  views::View::OnBlur();
  SchedulePaint();
}

bool TabListRow::IsGroupFocusTraversable() const {
  return false;
}

views::View* TabListRow::GetSelectedViewForGroup(int group) {
  return TabListView::FocusEntryFor(this);
}

void TabListRow::Activate(const ui::MouseEvent& event) {
  if (field_) {
    return;
  }
  // 双击名字改名：第一下已经开合过了，第二下不再开合。
  if (data_.kind == TabListRowData::Kind::kFolder &&
      event.GetClickCount() == 2 && title_bounds_.Contains(event.location())) {
    BeginEdit();
    return;
  }
  // 「新标签页」那一行右边的「✦ 整理」：翻到整理页。
  if (OrganizeRect().Contains(event.location())) {
    delegate_->TabFolderAction("@organize", std::nullopt);
    return;
  }
  // 按住 ⌥ 点：连同里面的文件夹一起展开、折叠。
  if (data_.kind == TabListRowData::Kind::kFolder && event.IsAltDown()) {
    delegate_->TabFolderToggleTree(data_.id);
    return;
  }
  delegate_->TabFolderAction(data_.id, std::nullopt);
}

void TabListRow::ShowMoreMenu() {
  gfx::Point point(0, more_button_->height() + 2);
  views::View::ConvertPointToScreen(more_button_, &point);
  delegate_->TabFolderAction(data_.id, point);
}

void TabListRow::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::mojom::MenuSourceType source_type) {
  if (leaving_ || field_ || data_.id == kMorePinsId ||
      (data_.kind != TabListRowData::Kind::kFolder &&
       data_.kind != TabListRowData::Kind::kLabel &&
       data_.kind != TabListRowData::Kind::kChip &&
       data_.kind != TabListRowData::Kind::kTile)) {
    return;
  }
  delegate_->TabFolderAction(data_.id, point);
}

void TabListRow::AnimationProgressed(const gfx::Animation* animation) {
  UpdateButtons();
  SchedulePaint();
}

void TabListRow::AnimationEnded(const gfx::Animation* animation) {
  AnimationProgressed(animation);
}

bool TabListRow::HandleKeyEvent(views::Textfield* sender,
                                const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  const bool commit = key_event.key_code() == ui::VKEY_RETURN;
  if (!commit && key_event.key_code() != ui::VKEY_ESCAPE) {
    return false;
  }
  // 输入框还在处理这个按键：回到消息循环再拿掉它。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&TabListRow::FinishEdit,
                                weak_factory_.GetWeakPtr(), commit));
  return true;
}

void TabListRow::OnFieldBlurred() {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&TabListRow::FinishEdit,
                                weak_factory_.GetWeakPtr(), true));
}

void TabListRow::FinishEdit(bool commit) {
  if (!field_ || finishing_edit_) {
    return;
  }
  finishing_edit_ = true;
  std::optional<std::u16string> name;
  if (commit) {
    name = std::u16string(field_->GetText());
  }
  auto* field = static_cast<RowTextfield*>(field_.get());
  field_ = nullptr;
  field->Detach();
  field->SetController(nullptr);
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, RemoveChildViewT(field));
  finishing_edit_ = false;
  UpdateButtons();
  InvalidateLayout();
  SchedulePaint();
  delegate_->TabFolderRenameFinished(data_.id, std::move(name));
}

BEGIN_METADATA(TabListRow)
END_METADATA

}  // namespace starry
