// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/pin_folder_card.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/motion/elastic_scroll_view.h"
#include "starry/browser/ui/tabs/tabs_view.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"

namespace starry {

namespace {

// 标题和操作按钮与列表行的图标、按钮对齐。
constexpr int kCardWidth = 264;
constexpr int kPadding = 6;
constexpr int kBottomPadding = 4;
constexpr int kHeaderHeight = 26;
constexpr int kTitleInset = 10;
constexpr int kButtonInset = 6;
constexpr int kButtonSize = 22;
constexpr int kButtonGap = 2;
// 列表最多这么高，再多就在卡片里滚动。
constexpr int kMaxListHeight = 420;
constexpr int kFieldHeight = 18;
constexpr int kFieldOutsetX = 6;
constexpr int kFieldOutsetY = 3;

class CardButton : public GlyphButton {
  METADATA_HEADER(CardButton, GlyphButton)

 public:
  using GlyphButton::GlyphButton;

  void StateChanged(ButtonState old_state) override {
    GlyphButton::StateChanged(old_state);
    SetShowsFill(GetState() == STATE_HOVERED || GetState() == STATE_PRESSED);
  }
};

BEGIN_METADATA(CardButton)
END_METADATA

class CardTextfield : public views::Textfield {
  METADATA_HEADER(CardTextfield, views::Textfield)

 public:
  explicit CardTextfield(PinFolderCard* card) : card_(card) {}

  void Detach() { card_ = nullptr; }

  void OnBlur() override {
    views::Textfield::OnBlur();
    if (card_) {
      card_->OnFieldBlurred();
    }
  }

 private:
  raw_ptr<PinFolderCard> card_;
};

BEGIN_METADATA(CardTextfield)
END_METADATA

}  // namespace

PinFolderCard::PinFolderCard(TabsView* owner, std::string id)
    : owner_(owner), id_(std::move(id)) {
  if (id_ != kMorePinsId) {
    add_button_ = AddChildView(std::make_unique<CardButton>(
        base::BindRepeating(
            [](PinFolderCard* card) {
              if (card->owner_) {
                card->owner_->TabFolderAddCurrent(card->id_);
              }
            },
            base::Unretained(this)),
        icons::kPlus, 11, kButtonSize));
    add_button_->SetTooltipText(L("tabs.addCurrentToFolder"));
    add_button_->GetViewAccessibility().SetName(L("tabs.addCurrentToFolder"));
    add_button_->SetFillRadius(radius::kButton);
    more_button_ = AddChildView(std::make_unique<CardButton>(
        base::BindRepeating(&PinFolderCard::ShowMenu, base::Unretained(this)),
        icons::kEllipsis, 13, kButtonSize));
    more_button_->SetTooltipText(L("common.moreActions"));
    more_button_->GetViewAccessibility().SetName(L("common.moreActions"));
    more_button_->SetFillRadius(radius::kButton);
  }
  scroll_ = AddChildView(std::make_unique<ElasticScrollView>(
      ElasticScrollView::Axis::kVertical,
      ElasticScrollView::Elasticity::kWhenScrollable));
  scroll_->SetBackgroundColor(std::nullopt);
  scroll_->SetDrawOverflowIndicator(false);
  scroll_->SetHorizontalScrollBarMode(
      views::ScrollView::ScrollBarMode::kDisabled);
  list_ = scroll_->SetContents(
      std::make_unique<TabListView>(TabListView::Style::kList, this));
  GetViewAccessibility().SetRole(ax::mojom::Role::kGroup);
}

PinFolderCard::~PinFolderCard() = default;

void PinFolderCard::Detach() {
  owner_ = nullptr;
}

void PinFolderCard::Update(const std::u16string& title,
                           std::vector<TabListRowData> rows,
                           const std::vector<TabListView::Item>& items,
                           const Theme& theme,
                           TabListView::Change change) {
  title_ = title;
  theme_ = theme;
  GetViewAccessibility().SetName(title_);
  for (GlyphButton* button : {add_button_.get(), more_button_.get()}) {
    if (button) {
      button->SetColors(theme_.on_surface_variant, theme_.OnSurface(0.1f));
    }
  }
  list_->SetRows(std::move(rows));
  list_->Update(items, theme_, change, std::nullopt);
  InvalidateLayout();
  SchedulePaint();
}

gfx::Size PinFolderCard::ContentSize() const {
  const int list_height =
      std::min(list_->GetPreferredSize().height(), kMaxListHeight);
  return gfx::Size(card_width_ > 0 ? card_width_ : kCardWidth,
                   kPadding + kHeaderHeight + list_height + kBottomPadding);
}

void PinFolderCard::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

gfx::Rect PinFolderCard::TitleRect() const {
  const int left = kPadding + kTitleInset;
  const int right = add_button_ ? add_button_->x() - kButtonGap
                                : width() - kPadding - kTitleInset;
  return gfx::Rect(left, kPadding, std::max(0, right - left), kHeaderHeight);
}

void PinFolderCard::BeginRename() {
  if (field_ || id_ == kMorePinsId || !owner_) {
    return;
  }
  auto field = std::make_unique<CardTextfield>(this);
  field->SetController(this);
  field->SetBackgroundEnabled(false);
  field->SetBorder(nullptr);
  field->SetFontList(UiFont(12, gfx::Font::Weight::MEDIUM));
  field->SetColor(theme_.on_surface);
  field->SetText(title_);
  field->GetViewAccessibility().SetName(L("tabs.renameFolder"));
  field_ = AddChildView(std::move(field));
  InvalidateLayout();
  SchedulePaint();
  field_->SelectAll(false);
  field_->RequestFocus();
}

void PinFolderCard::FinishRenameForTesting(std::optional<std::u16string> text) {
  if (!field_) {
    return;
  }
  if (text) {
    field_->SetText(*text);
  }
  FinishRename(text.has_value());
}

void PinFolderCard::OnFieldBlurred() {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&PinFolderCard::FinishRename,
                                weak_factory_.GetWeakPtr(), true));
}

void PinFolderCard::FinishRename(bool commit) {
  if (!field_ || finishing_rename_) {
    return;
  }
  finishing_rename_ = true;
  std::optional<std::u16string> name;
  if (commit) {
    name = std::u16string(field_->GetText());
    // 先换上新的名字，不等设置同步过来。
    std::u16string trimmed;
    base::TrimWhitespace(*name, base::TRIM_ALL, &trimmed);
    if (!trimmed.empty()) {
      title_ = trimmed;
      GetViewAccessibility().SetName(title_);
    }
  }
  auto* field = static_cast<CardTextfield*>(field_.get());
  field_ = nullptr;
  field->Detach();
  field->SetController(nullptr);
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, RemoveChildViewT(field));
  finishing_rename_ = false;
  SchedulePaint();
  if (owner_) {
    owner_->TabFolderRenameFinished(id_, std::move(name));
  }
}

void PinFolderCard::WillClose() {
  FinishRename(/*commit=*/true);
}

bool PinFolderCard::HandlesEscape() const {
  return field_ != nullptr;
}

bool PinFolderCard::HandleKeyEvent(views::Textfield* sender,
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
      FROM_HERE, base::BindOnce(&PinFolderCard::FinishRename,
                                weak_factory_.GetWeakPtr(), commit));
  return true;
}

bool PinFolderCard::OnMousePressed(const ui::MouseEvent& event) {
  if (event.IsOnlyLeftMouseButton() && event.GetClickCount() == 2 && !field_ &&
      TitleRect().Contains(event.location())) {
    BeginRename();
    return true;
  }
  return PopoverContent::OnMousePressed(event);
}

void PinFolderCard::Layout(PassKey) {
  const int right = width() - kPadding - kButtonInset;
  const int button_y = kPadding + (kHeaderHeight - kButtonSize) / 2;
  if (more_button_) {
    more_button_->SetBounds(right - kButtonSize, button_y, kButtonSize,
                            kButtonSize);
    add_button_->SetBounds(right - 2 * kButtonSize - kButtonGap, button_y,
                           kButtonSize, kButtonSize);
  }
  scroll_->SetBounds(
      kPadding, kPadding + kHeaderHeight, std::max(0, width() - 2 * kPadding),
      std::max(0, height() - kPadding - kHeaderHeight - kBottomPadding));
  if (field_) {
    const gfx::Rect title = TitleRect();
    field_->SetBounds(title.x(),
                      title.y() + (title.height() - kFieldHeight) / 2,
                      std::max(0, title.width() - kFieldOutsetX), kFieldHeight);
  }
}

void PinFolderCard::OnPaint(gfx::Canvas* canvas) {
  if (field_) {
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
    outline.setColor(SkColorSetA(theme_.accent, 0.6f * 255));
    box.Inset(0.75f);
    canvas->DrawRoundRect(box, radius::kButton - 0.75f, outline);
    return;
  }
  canvas->DrawStringRectWithFlags(title_,
                                  UiFont(11, gfx::Font::Weight::SEMIBOLD),
                                  theme_.on_surface_variant, TitleRect(),
                                  gfx::Canvas::NO_SUBPIXEL_RENDERING);
}

void PinFolderCard::ShowMenu() {
  if (!owner_ || !more_button_) {
    return;
  }
  gfx::Point point(0, more_button_->height() + 2);
  views::View::ConvertPointToScreen(more_button_, &point);
  owner_->TabFolderAction(id_, point);
}

void PinFolderCard::TabCellClicked(TabCell* cell,
                                   bool reset,
                                   bool keep_current) {
  if (owner_) {
    owner_->TabCellClicked(cell, reset, keep_current);
  }
}

void PinFolderCard::TabCellCloseRequested(TabCell* cell, bool button) {
  if (owner_) {
    owner_->TabCellCloseRequested(cell, button);
  }
}

void PinFolderCard::TabCellMuteToggled(TabCell* cell) {
  if (owner_) {
    owner_->TabCellMuteToggled(cell);
  }
}

void PinFolderCard::TabFolderAction(const std::string& id,
                                    std::optional<gfx::Point> menu_point) {
  if (owner_) {
    owner_->TabFolderAction(id, menu_point);
  }
}

void PinFolderCard::TabFolderAddCurrent(const std::string& id) {
  if (owner_) {
    owner_->TabFolderAddCurrent(id);
  }
}

void PinFolderCard::TabFolderOpenAll(const std::string& id) {
  if (owner_) {
    owner_->TabFolderOpenAll(id);
  }
}

void PinFolderCard::TabFolderToggleTree(const std::string& id) {
  if (owner_) {
    owner_->TabFolderToggleTree(id);
  }
}

bool PinFolderCard::TabListKeyPressed(views::View* item,
                                      const ui::KeyEvent& event) {
  return owner_ && owner_->TabListKeyPressed(item, event);
}

void PinFolderCard::TabFolderRenameFinished(
    const std::string& id,
    std::optional<std::u16string> name) {
  if (owner_) {
    owner_->TabFolderRenameFinished(id, std::move(name));
  }
}

void PinFolderCard::TabCellDragStarted(TabCell* cell,
                                       const gfx::Point& location) {
  if (owner_) {
    owner_->TabCellDragStarted(cell, location);
  }
}

void PinFolderCard::TabCellDragged(TabCell* cell, const gfx::Point& location) {
  if (owner_) {
    owner_->TabCellDragged(cell, location);
  }
}

void PinFolderCard::TabCellDragEnded(TabCell* cell, bool cancelled) {
  if (owner_) {
    owner_->TabCellDragEnded(cell, cancelled);
  }
}

void PinFolderCard::TabRowDragStarted(TabListRow* row,
                                      const gfx::Point& location) {
  if (owner_) {
    owner_->TabRowDragStarted(row, location);
  }
}

void PinFolderCard::TabRowDragged(TabListRow* row, const gfx::Point& location) {
  if (owner_) {
    owner_->TabRowDragged(row, location);
  }
}

void PinFolderCard::TabRowDragEnded(TabListRow* row, bool cancelled) {
  if (owner_) {
    owner_->TabRowDragEnded(row, cancelled);
  }
}

void PinFolderCard::TabCellContextMenu(TabCell* cell,
                                       const gfx::Point& screen_point,
                                       ui::mojom::MenuSourceType source) {
  if (owner_) {
    owner_->TabCellContextMenu(cell, screen_point, source);
  }
}

BEGIN_METADATA(PinFolderCard)
END_METADATA

}  // namespace starry
