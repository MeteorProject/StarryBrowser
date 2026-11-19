// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/archive/organize_page.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/tab_organizer/organizer_memory.h"
#include "starry/browser/tab_organizer/tab_organizer.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/motion/elastic_scroll_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"

namespace starry {

namespace {

// 最上面一行高 30（按钮 26）；最下面放按钮的一行高 44（按钮 28）。
constexpr int kHeaderHeight = 30;
constexpr int kButtonSize = 26;
constexpr int kFooterHeight = 44;
constexpr int kPillHeight = 28;
constexpr int kListTop = kHeaderHeight + 2;
// 列表：说明一行（可以折行），每组一张卡片（之间 6）：头一行 34，标签一行 28，卡片底下留
// 4；「留在列表里」一行 30，点开后每个 26。
constexpr int kLeadInset = 4;
constexpr int kCardGap = 6;
constexpr int kHeadHeight = 34;
constexpr int kTabHeight = 28;
constexpr int kCardBottom = 4;
constexpr int kLeftoverHeight = 30;
constexpr int kLeftoverTabHeight = 26;
constexpr int kCardRadius = 10;
// 卡片里：文件夹图标 15 在左 10，名字从 32 起；标签的图标 14 在 32，标题从 54 起。
constexpr int kFolderIcon = 15;
constexpr int kNameX = 32;
constexpr int kTabIconX = 32;
constexpr int kTabIcon = 14;
constexpr int kTabTitleX = 54;
// 右边的 ✕：组的 22 见方离右边 6，标签的 20 见方离右边 6。
constexpr int kDropSize = 22;
constexpr int kDropTabSize = 20;
constexpr int kDropRight = 6;
// 名字后面的「新文件夹」「已有的文件夹」：高 16，左右 6，离名字 6。
constexpr int kBadgeHeight = 16;
constexpr int kBadgePadding = 6;
constexpr int kBadgeGap = 6;
// 卡片里的预览最长多久内不重新问（翻走又翻回来）。
constexpr base::TimeDelta kKeepPlan = base::Minutes(2);
// 顶部标签布局的卡片：宽 320，最高 560。
constexpr int kCardWidth = 320;
constexpr int kCardMaxHeight = 560;

SkColor WithAlpha(SkColor color, float alpha) {
  return SkColorSetA(color, static_cast<U8CPU>(SkColorGetA(color) *
                                               std::clamp(alpha, 0.0f, 1.0f)));
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

const gfx::FontList& NameFont() {
  return UiFont(13, gfx::Font::Weight::SEMIBOLD);
}

const gfx::FontList& BadgeFont() {
  return UiFont(10, gfx::Font::Weight::MEDIUM);
}

std::u16string TitleOf(tabs::TabInterface* tab) {
  const std::u16string title = tab->GetContents()->GetTitle();
  return title.empty() ? L("tabs.untitled") : title;
}

// 正在问的时候的占位：三张卡片的样子（头一条、下面几条），图层整个呼吸。
class OrganizeSkeleton : public views::View {
  METADATA_HEADER(OrganizeSkeleton, views::View)

 public:
  OrganizeSkeleton() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags card;
    card.setAntiAlias(true);
    card.setColor(theme_.OnSurface(0.045f));
    cc::PaintFlags bar;
    bar.setAntiAlias(true);
    bar.setColor(theme_.OnSurface(0.09f));
    int y = 0;
    for (const int tabs : {4, 3, 2}) {
      const int height = kHeadHeight + tabs * kTabHeight + kCardBottom;
      if (y + height > this->height()) {
        break;
      }
      canvas->DrawRoundRect(gfx::RectF(0, y, width(), height), kCardRadius,
                            card);
      canvas->DrawRoundRect(
          gfx::RectF(10, y + kHeadHeight / 2 - 5, width() * 0.4f, 10), 5, bar);
      for (int i = 0; i < tabs; ++i) {
        const float top = y + kHeadHeight + i * kTabHeight + kTabHeight / 2.0f;
        canvas->DrawRoundRect(gfx::RectF(kTabIconX, top - 6, 12, 12), 3, bar);
        canvas->DrawRoundRect(
            gfx::RectF(kTabTitleX, top - 4,
                       (width() - kTabTitleX - 16) * (0.85f - 0.15f * (i % 3)),
                       8),
            4, bar);
      }
      y += height + kCardGap;
    }
  }

 private:
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(OrganizeSkeleton)
END_METADATA

// 顶部标签布局下装整理页的卡片。
class OrganizeCard : public PopoverContent {
  METADATA_HEADER(OrganizeCard, PopoverContent)

 public:
  explicit OrganizeCard(BrowserWindowInterface* browser) : browser_(browser) {
    page_ = AddChildView(std::make_unique<OrganizePage>(browser, true));
    page_->Prepare();
  }

  OrganizePage* page() { return page_; }

  // PopoverContent:
  gfx::Size ContentSize() const override {
    int height = kCardMaxHeight;
    if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
      height =
          std::clamp(view->PageBounds().height() - 40, 240, kCardMaxHeight);
    }
    return gfx::Size(kCardWidth, height);
  }
  void SetTheme(const Theme& theme) override { page_->SetTheme(theme); }
  views::View* InitialFocus() override { return page_; }
  void WillClose() override { page_->Closed(); }
  bool HandlesEscape() const override { return true; }

  // views::View:
  void Layout(PassKey) override {
    gfx::Rect bounds = GetLocalBounds();
    bounds.Inset(gfx::Insets::VH(8, 8));
    page_->SetBoundsRect(bounds);
  }

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<OrganizePage> page_ = nullptr;
};

BEGIN_METADATA(OrganizeCard)
END_METADATA

}  // namespace

class OrganizeList;

namespace {

// 改名的输入框：丢了焦点（点了别处）当作确定。
class NameField : public views::Textfield {
  METADATA_HEADER(NameField, views::Textfield)

 public:
  explicit NameField(base::RepeatingClosure blurred)
      : blurred_(std::move(blurred)) {}

  void Detach() { blurred_.Reset(); }

  void OnBlur() override {
    views::Textfield::OnBlur();
    if (blurred_) {
      blurred_.Run();
    }
  }

 private:
  base::RepeatingClosure blurred_;
};

BEGIN_METADATA(NameField)
END_METADATA

}  // namespace

// 列表本身（在滚动区域里）：说明、每组一张卡片、留在列表里的，都是自己画的；悬停的那一行右边
// 画 ✕，按位置认。新文件夹的名字点一下就地改（盖一个输入框）。
class OrganizeList : public views::View, public views::TextfieldController {
  METADATA_HEADER(OrganizeList, views::View)

 public:
  enum class Hit { kNone, kName, kDropGroup, kTab, kDropTab, kLeftover, kRest };

  explicit OrganizeList(OrganizePage* page) : page_(page) {
    SetWindowDraggable(this, false);
  }
  OrganizeList(const OrganizeList&) = delete;
  OrganizeList& operator=(const OrganizeList&) = delete;
  ~OrganizeList() override = default;

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    if (field_) {
      field_->SetColor(theme.on_surface);
    }
    SchedulePaint();
  }

  // 组变了：重新排。
  void Rebuild() {
    CancelEdit();
    rows_.clear();
    cards_.clear();
    const int width = std::max(0, this->width());
    int y = 0;
    lead_ = Lead();
    if (!lead_.empty()) {
      int text_width = std::max(0, width - 2 * kLeadInset);
      int text_height = 0;
      gfx::Canvas::SizeStringInt(lead_, UiFont(11), &text_width, &text_height,
                                 0, gfx::Canvas::MULTI_LINE);
      lead_rect_ = gfx::Rect(kLeadInset, 2, std::max(0, width - 2 * kLeadInset),
                             text_height);
      y = lead_rect_.bottom() + 8;
    }
    const auto& groups = page_->groups();
    for (size_t g = 0; g < groups.size(); ++g) {
      const int top = y;
      rows_.push_back({Row::kHead, g, 0, gfx::Rect(0, y, width, kHeadHeight)});
      y += kHeadHeight;
      for (size_t t = 0; t < groups[g].tabs.size(); ++t) {
        rows_.push_back({Row::kTab, g, t, gfx::Rect(0, y, width, kTabHeight)});
        y += kTabHeight;
      }
      y += kCardBottom;
      cards_.push_back(gfx::Rect(0, top, width, y - top));
      y += kCardGap;
    }
    const auto& leftover = page_->leftover();
    if (!leftover.empty()) {
      rows_.push_back(
          {Row::kLeftover, 0, 0, gfx::Rect(0, y, width, kLeftoverHeight)});
      y += kLeftoverHeight;
      if (show_leftover_) {
        for (size_t t = 0; t < leftover.size(); ++t) {
          rows_.push_back(
              {Row::kRest, 0, t, gfx::Rect(0, y, width, kLeftoverTabHeight)});
          y += kLeftoverTabHeight;
        }
      }
    }
    height_ = y + 8;
    hovered_.reset();
    hit_ = Hit::kNone;
    PreferredSizeChanged();
    SchedulePaint();
  }

  // 新文件夹的名字就地改。
  void BeginEdit(size_t group) {
    const auto& groups = page_->groups();
    if (group >= groups.size() || !groups[group].folder.empty()) {
      return;
    }
    CancelEdit();
    editing_ = group;
    auto field = std::make_unique<NameField>(base::BindRepeating(
        &OrganizeList::CommitEdit, weak_factory_.GetWeakPtr()));
    field->SetController(this);
    field->SetBackgroundEnabled(false);
    field->SetBorder(nullptr);
    field->SetFontList(NameFont());
    field->SetColor(theme_.on_surface);
    field->SetText(groups[group].name);
    field->SelectAll(false);
    field->GetViewAccessibility().SetName(L("organize.rename"));
    field_ = AddChildView(std::move(field));
    PlaceField();
    field_->RequestFocus();
    SchedulePaint();
  }

  bool editing() const { return field_ != nullptr; }

  void CommitEdit() {
    if (!field_ || !editing_) {
      return;
    }
    const size_t group = *editing_;
    std::u16string name;
    base::TrimWhitespace(field_->GetText(), base::TRIM_ALL, &name);
    CancelEdit();
    if (!name.empty()) {
      page_->RenameGroup(group, name);
    }
  }

  void CancelEdit() {
    if (!field_) {
      return;
    }
    NameField* field = field_;
    field_ = nullptr;
    editing_.reset();
    field->Detach();
    field->SetController(nullptr);
    // 不在它自己的事件里删：交给消息循环。
    std::unique_ptr<NameField> owned = RemoveChildViewT(field);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(owned));
    page_->RequestFocus();
    SchedulePaint();
  }

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(0, height_);
  }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    if (previous_bounds.width() != width()) {
      Rebuild();
    }
  }

  void OnPaint(gfx::Canvas* canvas) override {
    if (!lead_.empty()) {
      canvas->DrawStringRectWithFlags(
          lead_, UiFont(11), theme_.on_surface_variant, lead_rect_,
          gfx::Canvas::MULTI_LINE | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(theme_.OnSurface(theme_.is_dark ? 0.05f : 0.04f));
    for (const gfx::Rect& card : cards_) {
      canvas->DrawRoundRect(gfx::RectF(card), kCardRadius, fill);
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
      switch (rows_[i].kind) {
        case Row::kHead:
          PaintHead(canvas, i);
          break;
        case Row::kTab:
          PaintTab(canvas, i);
          break;
        case Row::kLeftover:
          PaintLeftover(canvas, i);
          break;
        case Row::kRest:
          PaintRest(canvas, i);
          break;
      }
    }
  }

  void OnMouseMoved(const ui::MouseEvent& event) override {
    UpdateHover(event.location());
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_.reset();
    hit_ = Hit::kNone;
    SchedulePaint();
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    UpdateHover(event.location());
    pressed_ = event.IsOnlyLeftMouseButton() && hit_ != Hit::kNone;
    return pressed_;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (!std::exchange(pressed_, false)) {
      return;
    }
    std::optional<size_t> row;
    const Hit hit = HitAt(event.location(), &row);
    if (row) {
      Activate(*row, hit);
    }
  }

  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    std::optional<size_t> row;
    return HitAt(event.location(), &row) == Hit::kName
               ? ui::Cursor(ui::mojom::CursorType::kIBeam)
               : ui::Cursor();
  }

  std::u16string GetRenderedTooltipText(
      const gfx::Point& point) const override {
    std::optional<size_t> row;
    switch (HitAt(point, &row)) {
      case Hit::kName:
        return L("organize.rename");
      case Hit::kDropGroup:
        return L("organize.dropGroup");
      case Hit::kDropTab:
        return L("organize.dropTab");
      default:
        return std::u16string();
    }
  }

  // views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override {
    if (key_event.type() != ui::EventType::kKeyPressed) {
      return false;
    }
    if (key_event.key_code() == ui::VKEY_RETURN) {
      CommitEdit();
      return true;
    }
    if (key_event.key_code() == ui::VKEY_ESCAPE) {
      CancelEdit();
      return true;
    }
    return false;
  }

 private:
  struct Row {
    enum Kind { kHead, kTab, kLeftover, kRest };
    Kind kind;
    size_t group;
    size_t tab;
    gfx::Rect rect;
  };

  std::u16string Lead() const {
    if (page_->state() != OrganizePage::State::kResult ||
        page_->groups().empty()) {
      return std::u16string();
    }
    std::u16string text =
        L("organize.lead.groups",
          {{"count", static_cast<int>(page_->groups().size())}});
    if (!page_->leftover().empty()) {
      text += L("organize.lead.leftover",
                {{"count", static_cast<int>(page_->leftover().size())}});
    }
    return text;
  }

  tabs::TabInterface* TabAt(const Row& row) const {
    const auto& groups = page_->groups();
    if (row.kind == Row::kTab && row.group < groups.size() &&
        row.tab < groups[row.group].tabs.size()) {
      return groups[row.group].tabs[row.tab].Get();
    }
    if (row.kind == Row::kRest && row.tab < page_->leftover().size()) {
      return page_->leftover()[row.tab].Get();
    }
    return nullptr;
  }

  gfx::Rect DropRect(const Row& row) const {
    const int size = row.kind == Row::kHead ? kDropSize : kDropTabSize;
    return gfx::Rect(row.rect.right() - kDropRight - size,
                     row.rect.y() + (row.rect.height() - size) / 2, size, size);
  }

  int BadgeWidth(const OrganizePage::Group& group) const {
    return gfx::GetStringWidth(BadgeText(group), BadgeFont()) +
           2 * kBadgePadding;
  }

  static std::u16string BadgeText(const OrganizePage::Group& group) {
    return group.folder.empty() ? L("organize.badge.new")
                                : L("organize.badge.existing");
  }

  // 名字在头一行里的位置（给徽标、✕ 留出地方）。
  gfx::Rect NameRect(const Row& row) const {
    const auto& group = page_->groups()[row.group];
    const int right = DropRect(row).x() - 4 - kBadgeGap - BadgeWidth(group);
    const int width = std::min(gfx::GetStringWidth(group.name, NameFont()) + 2,
                               std::max(0, right - kNameX));
    return gfx::Rect(kNameX - 3, row.rect.y() + 5, width + 6,
                     row.rect.height() - 10);
  }

  void PlaceField() {
    if (!field_ || !editing_) {
      return;
    }
    for (const Row& row : rows_) {
      if (row.kind == Row::kHead && row.group == *editing_) {
        field_->SetBounds(kNameX, row.rect.y() + 7,
                          std::max(0, DropRect(row).x() - 6 - kNameX),
                          row.rect.height() - 14);
      }
    }
  }

  Hit HitAt(const gfx::Point& point, std::optional<size_t>* index) const {
    index->reset();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const Row& row = rows_[i];
      if (!row.rect.Contains(point)) {
        continue;
      }
      *index = i;
      switch (row.kind) {
        case Row::kHead:
          if (DropRect(row).Contains(point)) {
            return Hit::kDropGroup;
          }
          if (page_->groups()[row.group].folder.empty() &&
              NameRect(row).Contains(point)) {
            return Hit::kName;
          }
          return Hit::kNone;
        case Row::kTab:
          return DropRect(row).Contains(point) ? Hit::kDropTab : Hit::kTab;
        case Row::kLeftover:
          return Hit::kLeftover;
        case Row::kRest:
          return Hit::kRest;
      }
    }
    return Hit::kNone;
  }

  void UpdateHover(const gfx::Point& point) {
    std::optional<size_t> row;
    const Hit hit = HitAt(point, &row);
    if (hit != hit_ || row != hovered_) {
      hit_ = hit;
      hovered_ = row;
      SchedulePaint();
    }
  }

  void Activate(size_t index, Hit hit) {
    const Row row = rows_[index];
    switch (hit) {
      case Hit::kName:
        BeginEdit(row.group);
        break;
      case Hit::kDropGroup:
        page_->DropGroup(row.group);
        break;
      case Hit::kDropTab:
        page_->DropTab(row.group, row.tab);
        break;
      case Hit::kLeftover:
        show_leftover_ = !show_leftover_;
        Rebuild();
        break;
      case Hit::kTab:
      case Hit::kRest:
        // 点一下切过去看看是哪个。
        if (tabs::TabInterface* tab = TabAt(row)) {
          TabStripModel* model = page_->browser()->GetTabStripModel();
          const int at = model->GetIndexOfTab(tab);
          if (at != TabStripModel::kNoTab) {
            model->ActivateTabAt(at);
          }
        }
        break;
      case Hit::kNone:
        break;
    }
  }

  void PaintDrop(gfx::Canvas* canvas, const Row& row, bool hot, float alpha) {
    const gfx::Rect box = DropRect(row);
    if (hot) {
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(theme_.OnSurface(0.1f));
      canvas->DrawRoundRect(gfx::RectF(box), radius::kButton, fill);
    }
    const int size = row.kind == Row::kHead ? 10 : 9;
    PaintIconAt(
        canvas, icons::kXmark, size,
        hot ? theme_.on_surface : WithAlpha(theme_.on_surface_variant, alpha),
        gfx::Point(box.x() + (box.width() - size) / 2,
                   box.y() + (box.height() - size) / 2));
  }

  void PaintHead(gfx::Canvas* canvas, size_t index) {
    const Row& row = rows_[index];
    const OrganizePage::Group& group = page_->groups()[row.group];
    const bool hovered = hovered_ == index;
    const int mid = row.rect.CenterPoint().y();
    // 文件夹图标：新的用强调色，已有的用它自己的颜色。
    SkColor tint = theme_.accent;
    if (!group.folder.empty()) {
      tint = theme_.on_surface_variant;
      for (const pin_folders::Folder& folder :
           pin_folders::Read(page_->browser()->GetProfile())) {
        if (folder.id == group.folder) {
          tint = pin_folders::ColorValue(folder.color, theme_.is_dark)
                     .value_or(theme_.on_surface_variant);
        }
      }
    }
    PaintIconAt(canvas, icons::kFolder, kFolderIcon, tint,
                gfx::Point(10, mid - kFolderIcon / 2));
    const gfx::Rect name = NameRect(row);
    const bool editing = editing_ == row.group && field_;
    if (!editing) {
      if (hovered && hit_ == Hit::kName) {
        cc::PaintFlags fill;
        fill.setAntiAlias(true);
        fill.setColor(theme_.OnSurface(0.07f));
        canvas->DrawRoundRect(gfx::RectF(name), 6, fill);
      }
      canvas->DrawStringRectWithFlags(
          gfx::ElideText(group.name, NameFont(), name.width() - 4,
                         gfx::ELIDE_TAIL),
          NameFont(), theme_.on_surface,
          gfx::Rect(kNameX, row.rect.y(), name.width() - 4, row.rect.height()),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
    } else {
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(theme_.OnSurface(0.08f));
      gfx::RectF box(field_->bounds());
      box.Outset(gfx::OutsetsF::VH(3, 4));
      canvas->DrawRoundRect(box, 6, fill);
      cc::PaintFlags ring;
      ring.setAntiAlias(true);
      ring.setStyle(cc::PaintFlags::kStroke_Style);
      ring.setStrokeWidth(1.5f);
      ring.setColor(WithAlpha(theme_.accent, 0.7f));
      box.Inset(0.75f);
      canvas->DrawRoundRect(box, 5.25f, ring);
    }
    if (!editing) {
      const int badge_width = BadgeWidth(group);
      const gfx::Rect badge(name.right() - 3 + kBadgeGap,
                            mid - kBadgeHeight / 2, badge_width, kBadgeHeight);
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(
          group.folder.empty()
              ? WithAlpha(theme_.accent, theme_.is_dark ? 0.18f : 0.13f)
              : theme_.OnSurface(0.08f));
      canvas->DrawRoundRect(gfx::RectF(badge), kBadgeHeight / 2.0f, fill);
      canvas->DrawStringRectWithFlags(
          BadgeText(group), BadgeFont(),
          group.folder.empty() ? theme_.accent : theme_.on_surface_variant,
          badge,
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }
    PaintDrop(canvas, row, hovered && hit_ == Hit::kDropGroup,
              hovered ? 0.9f : 0.55f);
  }

  void PaintTabRow(gfx::Canvas* canvas,
                   size_t index,
                   int icon_x,
                   int title_x,
                   bool dim,
                   bool droppable) {
    const Row& row = rows_[index];
    tabs::TabInterface* tab = TabAt(row);
    if (!tab) {
      return;
    }
    const bool hovered = hovered_ == index;
    const int mid = row.rect.CenterPoint().y();
    if (hovered) {
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(theme_.OnSurface(0.05f));
      gfx::RectF rect(row.rect);
      rect.Inset(gfx::InsetsF::VH(1, 4));
      canvas->DrawRoundRect(rect, 7, fill);
    }
    const tabs::TabData data = tabs::TabData::FromTabInterface(tab);
    const ui::ColorProvider* provider = GetColorProvider();
    const gfx::Rect icon(icon_x, mid - kTabIcon / 2, kTabIcon, kTabIcon);
    if (!data.favicon.IsEmpty() && provider) {
      const gfx::ImageSkia image = data.favicon.Rasterize(provider);
      if (!image.isNull()) {
        cc::PaintFlags flags;
        flags.setAlphaf(dim ? 0.7f : 1.0f);
        canvas->DrawImageInt(image, 0, 0, image.width(), image.height(),
                             icon.x(), icon.y(), icon.width(), icon.height(),
                             true, flags);
      }
    } else {
      PaintIconAt(canvas, icons::kGlobe, kTabIcon,
                  WithAlpha(theme_.on_surface_variant, 0.7f), icon.origin());
    }
    int right = row.rect.right() - 10;
    if (droppable && hovered) {
      right = DropRect(row).x() - 4;
      PaintDrop(canvas, row, hit_ == Hit::kDropTab, 0.9f);
    }
    const gfx::FontList& font = UiFont(12);
    const int width = std::max(0, right - title_x);
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(TitleOf(tab), font, width, gfx::ELIDE_TAIL), font,
        dim ? theme_.on_surface_variant : theme_.on_surface,
        gfx::Rect(title_x, row.rect.y(), width, row.rect.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void PaintTab(gfx::Canvas* canvas, size_t index) {
    PaintTabRow(canvas, index, kTabIconX, kTabTitleX, false, true);
  }

  void PaintRest(gfx::Canvas* canvas, size_t index) {
    PaintTabRow(canvas, index, 26, 48, true, false);
  }

  void PaintLeftover(gfx::Canvas* canvas, size_t index) {
    const Row& row = rows_[index];
    const bool hovered = hovered_ == index;
    const int mid = row.rect.CenterPoint().y();
    const SkColor color =
        hovered ? theme_.on_surface : theme_.on_surface_variant;
    PaintIconAt(canvas,
                show_leftover_ ? icons::kChevronDown : icons::kChevronRight, 10,
                color, gfx::Point(10, mid - 5));
    canvas->DrawStringRectWithFlags(
        L("organize.leftover",
          {{"count", static_cast<int>(page_->leftover().size())}}),
        UiFont(11, gfx::Font::Weight::MEDIUM), color,
        gfx::Rect(26, row.rect.y(), std::max(0, row.rect.width() - 30),
                  row.rect.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  const raw_ptr<OrganizePage> page_;
  Theme theme_ = Theme::DarkBase();
  std::vector<Row> rows_;
  std::vector<gfx::Rect> cards_;
  std::u16string lead_;
  gfx::Rect lead_rect_;
  int height_ = 0;
  bool show_leftover_ = false;
  std::optional<size_t> hovered_;
  Hit hit_ = Hit::kNone;
  bool pressed_ = false;
  raw_ptr<NameField> field_ = nullptr;
  std::optional<size_t> editing_;
  base::WeakPtrFactory<OrganizeList> weak_factory_{this};
};

BEGIN_METADATA(OrganizeList)
END_METADATA

void OpenOrganize(BrowserWindowInterface* browser) {
  StarryBrowserView* view = StarryBrowserView::From(browser);
  if (!view || !view->has_starry_chrome() ||
      browser->GetProfile()->IsOffTheRecord()) {
    return;
  }
  SidebarView* sidebar = view->sidebar();
  if (sidebar && !view->chrome_layout().tab_strip()) {
    if (sidebar->is_auto_hide() && !sidebar->is_peeking()) {
      sidebar->Peek();
    }
    sidebar->ShowOrganize(true);
    // 从菜单里打开的：菜单收起时把焦点还给原来的视图，之后再放到整理页上（Esc、
    // 回车管用）。
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<BrowserWindowInterface> window) {
              StarryBrowserView* view =
                  window ? StarryBrowserView::From(window.get()) : nullptr;
              SidebarView* sidebar = view ? view->sidebar() : nullptr;
              if (sidebar && sidebar->organize_open()) {
                sidebar->organize_page()->RequestFocus();
              }
            },
            browser->GetWeakPtr()));
    return;
  }
  // 顶部标签布局：内容卡片右上角的卡片。
  if (PopoverHost* host = view->popover_host()) {
    host->CancelAutoClose();
    host->Show(std::make_unique<OrganizeCard>(browser), view->PageBounds(),
               PopoverHost::Placement::kInsideTopRight);
  }
}

OrganizePage::Group::Group() = default;
OrganizePage::Group::Group(const Group&) = default;
OrganizePage::Group& OrganizePage::Group::operator=(const Group&) = default;
OrganizePage::Group::~Group() = default;

OrganizePage::OrganizePage(BrowserWindowInterface* browser, bool in_card)
    : browser_(browser), in_card_(in_card) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetWindowDraggable(this, false);
  SetFocusBehavior(FocusBehavior::ALWAYS);
  GetViewAccessibility().SetRole(ax::mojom::Role::kGroup);
  GetViewAccessibility().SetName(L("organize.title"));
  close_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&OrganizePage::Close, base::Unretained(this)),
      in_card ? icons::kXmark : icons::kChevronLeft,
      in_card ? L("common.close") : L("archive.page.back"), kButtonSize,
      in_card ? 11 : 14));
  scroll_ = AddChildView(std::make_unique<ElasticScrollView>(
      ElasticScrollView::Axis::kVertical,
      ElasticScrollView::Elasticity::kAlways));
  scroll_->SetBackgroundColor(std::nullopt);
  scroll_->SetDrawOverflowIndicator(false);
  scroll_->SetVerticalScrollBarMode(
      views::ScrollView::ScrollBarMode::kHiddenButEnabled);
  scroll_->SetHorizontalScrollBarMode(
      views::ScrollView::ScrollBarMode::kDisabled);
  list_ = scroll_->SetContents(std::make_unique<OrganizeList>(this));
  skeleton_ = AddChildView(std::make_unique<OrganizeSkeleton>());

  const auto pill = [this](void (OrganizePage::*method)(),
                           const std::u16string& title, ButtonVariant variant) {
    auto button = std::make_unique<PillButton>(
        base::BindRepeating(method, base::Unretained(this)), title, variant);
    button->SetHeight(kPillHeight);
    return AddChildView(std::move(button));
  };
  regroup_ = pill(&OrganizePage::Regroup, L("organize.regroup"),
                  ButtonVariant::kSecondary);
  regroup_->SetIcon(&icons::kArrowClockwise);
  apply_ = pill(&OrganizePage::Apply, std::u16string(), ButtonVariant::kFilled);
  cancel_ =
      pill(&OrganizePage::Close, L("common.cancel"), ButtonVariant::kSecondary);
  retry_ =
      pill(&OrganizePage::Regroup, L("common.retry"), ButtonVariant::kFilled);
  settings_ = pill(&OrganizePage::OpenSettings, L("organize.openSettings"),
                   ButtonVariant::kFilled);
  browser->GetTabStripModel()->AddObserver(this);
  Update();
}

OrganizePage::~OrganizePage() {
  browser_->GetTabStripModel()->RemoveObserver(this);
}

void OrganizePage::SetTheme(const Theme& theme) {
  theme_ = theme;
  close_->SetTheme(theme);
  list_->SetTheme(theme);
  static_cast<OrganizeSkeleton*>(skeleton_.get())->SetTheme(theme);
  for (PillButton* button : {regroup_, apply_, cancel_, retry_, settings_}) {
    button->SetTheme(theme);
  }
  SchedulePaint();
}

void OrganizePage::Prepare() {
  if (state_ == State::kThinking) {
    return;
  }
  if (state_ == State::kResult && !groups_.empty() &&
      base::TimeTicks::Now() - planned_ < kKeepPlan) {
    Prune();
    if (!groups_.empty()) {
      Update();
      return;
    }
  }
  Start();
}

void OrganizePage::Closed() {
  list_->CancelEdit();
  if (state_ == State::kThinking && grouper_) {
    grouper_->Cancel();
  }
}

void OrganizePage::Close() {
  if (in_card_) {
    if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
      if (PopoverHost* host = view->popover_host()) {
        host->Close();
      }
    }
    return;
  }
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    if (SidebarView* sidebar = view->sidebar()) {
      sidebar->ShowOrganize(false);
    }
  }
}

void OrganizePage::Regroup() {
  if (grouper_ && state_ == State::kThinking) {
    return;
  }
  Start();
}

void OrganizePage::Start() {
  list_->CancelEdit();
  state_ = State::kThinking;
  error_.clear();
  groups_.clear();
  leftover_.clear();
  asked_ = 0;
  Update();
  grouper_ = std::make_unique<tab_organizer::TabGrouper>(browser_);
  grouper_->Start(
      base::BindOnce(&OrganizePage::OnGrouped, weak_factory_.GetWeakPtr()));
}

void OrganizePage::OnGrouped(tab_organizer::TabGrouper::Result result) {
  if (!result.has_value()) {
    if (result.error() == "cancelled") {
      state_ = State::kIdle;
    } else {
      state_ = State::kError;
      error_ = result.error();
    }
    Update();
    return;
  }
  asked_ = grouper_ ? grouper_->tab_count() : 0;
  groups_.clear();
  for (const tab_organizer::GroupPlan::Group& group : result->groups) {
    Group copy;
    copy.name = group.name;
    copy.folder = group.folder;
    copy.tabs = group.tabs;
    groups_.push_back(std::move(copy));
  }
  leftover_ = result->leftover;
  state_ = State::kResult;
  planned_ = base::TimeTicks::Now();
  Update();
}

void OrganizePage::Apply() {
  if (state_ != State::kResult || groups_.empty()) {
    return;
  }
  list_->CommitEdit();
  Prune();
  std::vector<tab_organizer::FolderGroup> groups;
  for (const Group& group : groups_) {
    tab_organizer::FolderGroup folder;
    folder.name = group.name;
    folder.folder = group.folder;
    folder.tabs = group.tabs;
    groups.push_back(std::move(folder));
  }
  BrowserWindowInterface* browser = browser_;
  // 用过了：下次翻过来重新问。
  state_ = State::kIdle;
  groups_.clear();
  leftover_.clear();
  Update();
  // 先回去（卡片收起时这一页跟着没了：后面不再碰成员），标签列表看得到放进去的样子和提示。
  Close();
  const tab_organizer::Batch* batch =
      tab_organizer::TabOrganizer::ForProfile(browser->GetProfile())
          ->MoveToFolders(groups, tab_organizer::Source::kOrganize,
                          /*collapse_new=*/true);
  if (!batch) {
    return;
  }
  // 用户看过、放进去的：每个网站和文件夹记一次（整理的习惯，够了学成规则）。
  std::set<std::pair<std::string, std::string>> noted;
  for (const tab_organizer::Batch::Move& move : batch->moves) {
    tabs::TabInterface* tab = move.tab.Get();
    if (!tab) {
      continue;
    }
    const GURL url = tab->GetContents()->GetVisibleURL();
    if (noted.insert({tab_organizer::SiteOf(url), move.folder}).second) {
      tab_organizer::NotePlacement(browser->GetProfile(), url, move.folder);
    }
  }
}

void OrganizePage::RenameGroup(size_t group, const std::u16string& name) {
  if (group >= groups_.size() || !groups_[group].folder.empty()) {
    return;
  }
  std::u16string clean;
  base::TrimWhitespace(name, base::TRIM_ALL, &clean);
  if (clean.empty()) {
    return;
  }
  groups_[group].name = clean.substr(0, tab_organizer::kMaxFolderName);
  // 记住用户起的名字：下次一键整理时告诉模型。
  tab_organizer::NoteName(browser_->GetProfile(), groups_[group].name);
  Update();
}

void OrganizePage::DropGroup(size_t group) {
  if (group >= groups_.size()) {
    return;
  }
  // 这一组的标签留在列表里（按列表里的顺序排进去），记住不要这样放。
  for (const tabs::TabHandle& tab : groups_[group].tabs) {
    NoteDropped(groups_[group], tab);
    leftover_.push_back(tab);
  }
  groups_.erase(groups_.begin() + group);
  TabStripModel* model = browser_->GetTabStripModel();
  std::ranges::sort(leftover_, {}, [model](const tabs::TabHandle& handle) {
    tabs::TabInterface* tab = handle.Get();
    return tab ? model->GetIndexOfTab(tab) : model->count();
  });
  Update();
}

void OrganizePage::DropTab(size_t group, size_t tab) {
  if (group >= groups_.size() || tab >= groups_[group].tabs.size()) {
    return;
  }
  NoteDropped(groups_[group], groups_[group].tabs[tab]);
  leftover_.push_back(groups_[group].tabs[tab]);
  groups_[group].tabs.erase(groups_[group].tabs.begin() + tab);
  if (groups_[group].tabs.empty()) {
    groups_.erase(groups_.begin() + group);
  }
  TabStripModel* model = browser_->GetTabStripModel();
  std::ranges::sort(leftover_, {}, [model](const tabs::TabHandle& handle) {
    tabs::TabInterface* entry = handle.Get();
    return entry ? model->GetIndexOfTab(entry) : model->count();
  });
  Update();
}

void OrganizePage::NoteDropped(const Group& group,
                               const tabs::TabHandle& handle) {
  if (tabs::TabInterface* tab = handle.Get()) {
    tab_organizer::NoteRejection(
        browser_->GetProfile(), tab->GetContents()->GetVisibleURL(),
        group.folder.empty() ? tab_organizer::NameKey(group.name)
                             : group.folder);
  }
}

void OrganizePage::OpenSettings() {
  OpenStarrySettings(browser_, "tabs");
}

void OrganizePage::Prune() {
  const auto gone = [this](const tabs::TabHandle& handle) {
    tabs::TabInterface* tab = handle.Get();
    return !tab || tab->IsPinned() ||
           tab->GetBrowserWindowInterface() != browser_;
  };
  for (Group& group : groups_) {
    std::erase_if(group.tabs, gone);
  }
  std::erase_if(groups_, [](const Group& group) { return group.tabs.empty(); });
  std::erase_if(leftover_, gone);
}

size_t OrganizePage::TabCount() const {
  size_t count = 0;
  for (const Group& group : groups_) {
    count += group.tabs.size();
  }
  return count;
}

bool OrganizePage::NeedsSettings() const {
  return error_ == "notConfigured" || error_ == "localAgentMissing";
}

std::u16string OrganizePage::ErrorText() const {
  if (state_ == State::kResult && groups_.empty()) {
    return L("organize.empty");
  }
  if (state_ != State::kError) {
    return std::u16string();
  }
  if (error_ == "notConfigured") {
    return L("organize.error.notConfigured");
  }
  if (error_ == "localAgentMissing") {
    return L("organize.error.localAgent");
  }
  if (error_ == "tooFew") {
    return L("organize.error.tooFew");
  }
  if (error_ == "timeout") {
    return L("organize.error.timeout");
  }
  if (error_ == "badAnswer" || error_ == "toolCall" || error_ == "tooLong") {
    return L("organize.error.badAnswer");
  }
  std::string message = error_;
  if (base::StartsWith(message, "failed: ")) {
    message = message.substr(8);
  }
  return L("organize.error.failed", {{"message", message}});
}

void OrganizePage::Update() {
  const bool thinking = state_ == State::kThinking;
  const bool result = state_ == State::kResult && !groups_.empty();
  scroll_->SetVisible(result);
  skeleton_->SetVisible(thinking);
  if (thinking && !motion::ReduceMotion() && !pulse_.running()) {
    pulse_.Pulse(skeleton_->layer(), 0.45f, 1.0f, base::Milliseconds(700));
  } else if (!thinking && pulse_.running()) {
    pulse_.Stop();
  }
  regroup_->SetVisible(state_ == State::kResult);
  apply_->SetVisible(result);
  apply_->SetTitle(
      L("organize.apply", {{"count", static_cast<int>(groups_.size())}}));
  cancel_->SetVisible(thinking);
  const bool error = state_ == State::kError;
  settings_->SetVisible(error && NeedsSettings());
  retry_->SetVisible(error && !NeedsSettings() && error_ != "tooFew");
  list_->Rebuild();
  InvalidateLayout();
  SchedulePaint();
}

void OrganizePage::Layout(PassKey) {
  const int top = (kHeaderHeight - kButtonSize) / 2;
  close_->SetBounds(in_card_ ? width() - kButtonSize : 0, top, kButtonSize,
                    kButtonSize);
  const int bottom = height() - kFooterHeight;
  scroll_->SetBounds(0, kListTop, width(), std::max(0, bottom - kListTop));
  skeleton_->SetBounds(0, kListTop + 30, width(),
                       std::max(0, bottom - kListTop - 30));
  // 最下面：左边次要的，右边主要的。
  int left = 0;
  int right = width();
  for (PillButton* button : {regroup_, cancel_, apply_, retry_, settings_}) {
    if (!button->GetVisible()) {
      continue;
    }
    const gfx::Size size = button->GetPreferredSize();
    const int y = bottom + (kFooterHeight - size.height()) / 2;
    const int outset = PillButton::FocusOutset();
    if (button == regroup_) {
      button->SetBounds(left - outset, y, size.width(), size.height());
      left += size.width();
    } else {
      right -= size.width() - outset;
      button->SetBounds(right, y, size.width(), size.height());
      right -= 8;
    }
  }
}

void OrganizePage::OnPaint(gfx::Canvas* canvas) {
  // 标题：「整理标签页」13 半粗、个数 11 灰。
  const gfx::FontList& title_font = UiFont(13, gfx::Font::Weight::SEMIBOLD);
  const std::u16string title = L("organize.title");
  const int title_x = in_card_ ? 6 : kButtonSize + 4;
  const int title_width = gfx::GetStringWidth(title, title_font);
  canvas->DrawStringRectWithFlags(
      title, title_font, theme_.on_surface,
      gfx::Rect(title_x, 0, title_width, kHeaderHeight),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
  if (state_ == State::kResult && asked_ > 0) {
    canvas->DrawStringRectWithFlags(
        L("organize.count", {{"count", static_cast<int>(asked_)}}), UiFont(11),
        theme_.on_surface_variant,
        gfx::Rect(title_x + title_width + 6, 0, 80, kHeaderHeight),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
  // 最下面一行上面一条细线。
  const int bottom = height() - kFooterHeight;
  canvas->FillRect(gfx::Rect(6, bottom, std::max(0, width() - 12), 1),
                   theme_.OnSurface(0.07f));

  if (state_ == State::kThinking) {
    constexpr int kSparkle = 13;
    PaintIconAt(canvas, icons::kSparkle, kSparkle, theme_.accent,
                gfx::Point(kLeadInset + 2, kListTop + 8));
    canvas->DrawStringRectWithFlags(
        L("organize.thinking"), UiFont(12, gfx::Font::Weight::MEDIUM),
        theme_.on_surface_variant,
        gfx::Rect(kLeadInset + kSparkle + 10, kListTop + 2,
                  std::max(0, width() - kSparkle - 20), 24),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    return;
  }
  const std::u16string message = ErrorText();
  if (message.empty()) {
    return;
  }
  // 出错、没分出组：中间一个图标和一段话。
  const int mid = kListTop + 70;
  constexpr int kIcon = 26;
  PaintIconAt(canvas,
              state_ == State::kError ? icons::kSparkle : icons::kFolder, kIcon,
              WithAlpha(theme_.on_surface_variant, 0.6f),
              gfx::Point(width() / 2 - kIcon / 2, mid - kIcon));
  canvas->DrawStringRectWithFlags(
      message, UiFont(12), theme_.on_surface_variant,
      gfx::Rect(16, mid + 12, std::max(0, width() - 32), 60),
      gfx::Canvas::MULTI_LINE | gfx::Canvas::TEXT_ALIGN_CENTER |
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
}

bool OrganizePage::OnKeyPressed(const ui::KeyEvent& event) {
  if (event.key_code() == ui::VKEY_ESCAPE) {
    Close();
    return true;
  }
  if (event.key_code() == ui::VKEY_RETURN && state_ == State::kResult) {
    Apply();
    return true;
  }
  return false;
}

bool OrganizePage::SkipDefaultKeyEventProcessing(const ui::KeyEvent& event) {
  // Esc、回车先给这一页（不然 Esc 是浏览器的「停止载入」）。
  return event.key_code() == ui::VKEY_ESCAPE ||
         event.key_code() == ui::VKEY_RETURN;
}

void OrganizePage::VisibilityChanged(views::View* starting_from,
                                     bool is_visible) {
  // 呼吸只在看得见的时候跑。
  if (!is_visible && pulse_.running()) {
    pulse_.Stop();
  } else if (is_visible) {
    Update();
  }
}

void OrganizePage::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (change.type() != TabStripModelChange::kRemoved ||
      state_ != State::kResult) {
    return;
  }
  // 关掉的标签不能留在预览里（异步：模型这时还在改）。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<OrganizePage> page) {
                       if (page && page->state_ == State::kResult) {
                         page->Prune();
                         page->Update();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

bool OrganizePage::RenameForTesting(size_t group, const std::u16string& name) {
  if (group >= groups_.size() || !groups_[group].folder.empty()) {
    return false;
  }
  list_->BeginEdit(group);
  RenameGroup(group, name);
  list_->CancelEdit();
  return true;
}

std::string OrganizePage::DescribeForTesting() const {
  const char* state = "idle";
  switch (state_) {
    case State::kThinking:
      state = "thinking";
      break;
    case State::kResult:
      state = "result";
      break;
    case State::kError:
      state = "error";
      break;
    case State::kIdle:
      break;
  }
  std::string text = base::StrCat(
      {"organizepage state=", state, " visible=", IsDrawn() ? "1" : "0",
       " card=", in_card_ ? "1" : "0", " asked=", base::NumberToString(asked_),
       " groups=", base::NumberToString(groups_.size()),
       " tabs=", base::NumberToString(TabCount()),
       " leftover=", base::NumberToString(leftover_.size()), " error=", error_,
       " apply=\"", base::UTF16ToUTF8(apply_->title()), "\"\n"});
  for (size_t g = 0; g < groups_.size(); ++g) {
    base::StrAppend(&text,
                    {"  group ", base::NumberToString(g), " \"",
                     base::UTF16ToUTF8(groups_[g].name), "\" ",
                     groups_[g].folder.empty() ? "new" : "existing", ":"});
    for (const tabs::TabHandle& handle : groups_[g].tabs) {
      if (tabs::TabInterface* tab = handle.Get()) {
        base::StrAppend(&text, {" \"", base::UTF16ToUTF8(TitleOf(tab)), "\""});
      }
    }
    text += "\n";
  }
  if (!leftover_.empty()) {
    text += "  leftover:";
    for (const tabs::TabHandle& handle : leftover_) {
      if (tabs::TabInterface* tab = handle.Get()) {
        base::StrAppend(&text, {" \"", base::UTF16ToUTF8(TitleOf(tab)), "\""});
      }
    }
    text += "\n";
  }
  return text;
}

BEGIN_METADATA(OrganizePage)
END_METADATA

}  // namespace starry
