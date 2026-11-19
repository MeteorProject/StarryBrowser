// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/archive/archive_page.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "base/i18n/case_conversion.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/favicon/core/favicon_service.h"
#include "components/favicon_base/favicon_types.h"
#include "components/keyed_service/core/service_access_type.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/tab_organizer/tab_organizer.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/motion/elastic_scroll_view.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "starry/browser/ui/webui/starry_pages.h"
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

namespace starry {

namespace {

using tab_organizer::ArchivedTab;
using tab_organizer::ArchiveReason;

// 最上面一行高 30（按钮 26）；搜索框高 28；底下一行高 28。
constexpr int kHeaderHeight = 30;
constexpr int kButtonSize = 26;
constexpr int kSearchTop = kHeaderHeight + 6;
constexpr int kSearchHeight = 28;
constexpr int kListTop = kSearchTop + kSearchHeight + 6;
constexpr int kFooterHeight = 28;
// 列表：一行 40（标题、网站两行），分组的标题 26（第一个 20）。
constexpr int kRowHeight = 40;
constexpr int kSectionHeight = 26;
constexpr int kFirstSectionHeight = 20;
constexpr int kLeading = 10;
constexpr int kIconSize = 16;
constexpr int kTextX = kLeading + kIconSize + 10;
// 悬停时右边的两个按钮：22 见方，离右边 5，之间 2。
constexpr int kActionSize = 22;
constexpr int kActionRight = 5;
constexpr int kActionGap = 2;
// 侧栏里最多列多少个，多的去整页看。
constexpr size_t kMaxRows = 200;

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

std::u16string DisplayHost(const GURL& url) {
  std::string host(url.host());
  if (base::StartsWith(host, "www.")) {
    host = host.substr(4);
  }
  return base::UTF8ToUTF16(host);
}

// 「1 周」「3 天」「12 小时」（同设置 › 标签管理、整页的写法）。
std::u16string DurationLabel(int hours) {
  if (hours >= 24 * 7 && hours % (24 * 7) == 0 && hours < 24 * 28) {
    return L("settings.tabs.duration.weeks", {{"count", hours / (24 * 7)}});
  }
  if (hours >= 24 && hours % 24 == 0) {
    return L("settings.tabs.duration.days", {{"count", hours / 24}});
  }
  return L("settings.tabs.duration.hours", {{"count", hours}});
}

// 为什么归档的：「闲置 9 天」「重复的标签页」「手动归档」「AI 助手归档」。
std::u16string ReasonLabel(const ArchivedTab& tab) {
  switch (tab.reason) {
    case ArchiveReason::kDuplicate:
      return L("archive.reason.duplicate");
    case ArchiveReason::kManual:
      return L("archive.reason.manual");
    case ArchiveReason::kAssistant:
      return L("archive.reason.assistant");
    case ArchiveReason::kIdle:
      break;
  }
  const base::TimeDelta idle = tab.last_active.is_null()
                                   ? base::TimeDelta()
                                   : tab.archived - tab.last_active;
  if (idle >= base::Days(1)) {
    return L("archive.reason.idleDays", {{"count", idle.InDays()}});
  }
  if (idle >= base::Hours(1)) {
    return L("archive.reason.idleHours", {{"count", idle.InHours()}});
  }
  return L("archive.reason.idle");
}

// 归档的那天在哪一组：今天、昨天、7 天内、更早。
int SectionOf(base::Time archived, base::Time now) {
  const base::Time today = now.LocalMidnight();
  if (archived >= today) {
    return 0;
  }
  if (archived >= today - base::Days(1)) {
    return 1;
  }
  if (archived >= today - base::Days(6)) {
    return 2;
  }
  return 3;
}

std::u16string SectionTitle(int section) {
  switch (section) {
    case 0:
      return L("library.day.today");
    case 1:
      return L("library.day.yesterday");
    case 2:
      return L("archive.section.week");
    default:
      return L("archive.section.earlier");
  }
}

}  // namespace

// 列表本身（在滚动区域里）：分组的标题和每个标签一行，都是自己画的；悬停那一行右边画两个
// 小按钮，按位置认。
class ArchiveList : public views::View {
  METADATA_HEADER(ArchiveList, views::View)

 public:
  enum class Hit { kNone, kRow, kRestore, kRemove, kMore };

  struct Item {
    bool section = false;
    std::u16string title;
    std::u16string detail;
    int count = 0;
    std::string id;
    GURL url;
    int y = 0;
    int height = 0;
  };

  explicit ArchiveList(ArchivePage* page) : page_(page) {
    SetWindowDraggable(this, false);
  }
  ArchiveList(const ArchiveList&) = delete;
  ArchiveList& operator=(const ArchiveList&) = delete;
  ~ArchiveList() override = default;

  // 按搜索词重新列一遍；selected 尽量留在原来那个标签上。
  void Rebuild(const std::u16string& query) {
    query_ = query;
    const std::string selected_id = selected_ && *selected_ < items_.size()
                                        ? items_[*selected_].id
                                        : std::string();
    items_.clear();
    more_ = 0;
    total_ = 0;
    Profile* profile = page_->browser()->GetProfile();
    if (profile->IsOffTheRecord()) {
      FinishRebuild(selected_id);
      return;
    }
    tab_organizer::TabArchive* archive =
        tab_organizer::TabArchive::ForProfile(profile);
    total_ = archive->tabs().size();
    const std::vector<const ArchivedTab*> found =
        archive->Search(query, archive->tabs().size());
    const base::Time now = base::Time::Now();
    int section = -1;
    size_t rows = 0;
    for (const ArchivedTab* tab : found) {
      if (rows >= kMaxRows) {
        more_ = found.size() - rows;
        break;
      }
      const int next = SectionOf(tab->archived, now);
      if (next != section) {
        section = next;
        Item header;
        header.section = true;
        header.title = SectionTitle(section);
        items_.push_back(std::move(header));
      }
      ++items_[SectionIndex()].count;
      Item row;
      row.id = tab->id;
      row.url = tab->url;
      row.title =
          tab->title.empty() ? base::UTF8ToUTF16(tab->url.spec()) : tab->title;
      // 网站 · 主题（归档时打上的；没有时是为什么归档的）。
      const std::u16string host = DisplayHost(tab->url);
      const std::u16string note =
          tab->topic.empty() ? ReasonLabel(*tab) : tab->topic;
      row.detail = host.empty() ? note : base::StrCat({host, u" · ", note});
      items_.push_back(std::move(row));
      ++rows;
    }
    FinishRebuild(selected_id);
  }

  size_t total() const { return total_; }
  size_t row_count() const {
    return static_cast<size_t>(std::ranges::count_if(
        items_, [](const Item& item) { return !item.section; }));
  }
  const std::vector<Item>& items() const { return items_; }
  std::optional<size_t> selected() const { return selected_; }

  // 键盘：选上一个、下一个标签。
  void MoveSelection(int delta) {
    std::vector<size_t> rows;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (!items_[i].section) {
        rows.push_back(i);
      }
    }
    if (rows.empty()) {
      return;
    }
    size_t at = 0;
    if (selected_) {
      auto it = std::ranges::find(rows, *selected_);
      const int index = static_cast<int>(it - rows.begin()) + delta;
      at = static_cast<size_t>(
          std::clamp(index, 0, static_cast<int>(rows.size()) - 1));
    } else if (delta < 0) {
      at = rows.size() - 1;
    }
    selected_ = rows[at];
    const Item& item = items_[*selected_];
    ScrollRectToVisible(gfx::Rect(0, item.y, width(), item.height));
    SchedulePaint();
  }

  // 第 index 个标签的行（不算分组的标题）在 items_ 里是第几个。
  std::optional<size_t> RowItem(size_t index) const {
    for (size_t i = 0; i < items_.size(); ++i) {
      if (!items_[i].section && index-- == 0) {
        return i;
      }
    }
    return std::nullopt;
  }

  void Activate(size_t item, Hit hit) {
    if (item >= items_.size() || items_[item].section) {
      return;
    }
    const std::string id = items_[item].id;
    BrowserWindowInterface* browser = page_->browser();
    auto* organizer =
        tab_organizer::TabOrganizer::ForProfile(browser->GetProfile());
    switch (hit) {
      case Hit::kRow:
        // 恢复并切过去：这一页收起来，回到标签列表看得到它。
        page_->Close();
        organizer->Restore(browser, id, /*select=*/true);
        break;
      case Hit::kRestore:
        organizer->Restore(browser, id, /*select=*/false);
        break;
      case Hit::kRemove:
        tab_organizer::TabArchive::ForProfile(browser->GetProfile())
            ->Remove({id});
        break;
      case Hit::kNone:
      case Hit::kMore:
        break;
    }
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(0, height_);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    if (items_.empty()) {
      PaintEmpty(canvas);
      return;
    }
    // 只画要重画的那一块里的行。不能按 GetVisibleBounds() 算：侧栏滑出来、页刚显示时它
    // 还很小，那时没画的行之后不会再画（内容在自己的图层里，只是可见的部分变大了）。
    SkRect clip = SkRect::MakeWH(width(), height_);
    canvas->sk_canvas()->getLocalClipBounds(&clip);
    for (size_t i = 0; i < items_.size(); ++i) {
      const Item& item = items_[i];
      if (item.y + item.height < clip.top() || item.y > clip.bottom()) {
        continue;
      }
      if (item.section) {
        PaintSection(canvas, item);
      } else {
        PaintRow(canvas, i, item);
      }
    }
    if (more_ > 0) {
      const gfx::Rect rect(kLeading, height_ - kRowHeight + 6,
                           width() - 2 * kLeading, kRowHeight - 12);
      canvas->DrawStringRectWithFlags(
          L("archive.page.more", {{"count", static_cast<int>(more_)}}),
          UiFont(11, gfx::Font::Weight::MEDIUM),
          hit_ == Hit::kMore ? theme_.accent : theme_.on_surface_variant, rect,
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
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
    if (!pressed_) {
      return;
    }
    pressed_ = false;
    std::optional<size_t> item;
    const Hit hit = HitAt(event.location(), &item);
    if (hit == Hit::kMore) {
      page_->Close();
      OpenStarryArchive(page_->browser(), query_);
      return;
    }
    if (item) {
      Activate(*item, hit);
    }
  }

  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    return hit_ == Hit::kMore ? ui::Cursor(ui::mojom::CursorType::kHand)
                              : ui::Cursor();
  }

  std::u16string GetRenderedTooltipText(
      const gfx::Point& point) const override {
    std::optional<size_t> item;
    switch (HitAt(point, &item)) {
      case Hit::kRestore:
        return L("archive.menu.restoreBackground");
      case Hit::kRemove:
        return L("archive.menu.remove");
      case Hit::kRow:
        return item ? base::UTF8ToUTF16(items_[*item].url.spec())
                    : std::u16string();
      case Hit::kNone:
      case Hit::kMore:
        return std::u16string();
    }
    return std::u16string();
  }

 private:
  size_t SectionIndex() const {
    for (size_t i = items_.size(); i-- > 0;) {
      if (items_[i].section) {
        return i;
      }
    }
    return 0;
  }

  void FinishRebuild(const std::string& selected_id) {
    int y = 0;
    bool first = true;
    for (Item& item : items_) {
      item.y = y;
      item.height = item.section
                        ? (first ? kFirstSectionHeight : kSectionHeight)
                        : kRowHeight;
      first = false;
      y += item.height;
    }
    if (more_ > 0) {
      y += kRowHeight;
    }
    // 没有东西时留一块地方写「没有归档的标签页」。
    height_ = items_.empty() ? 160 : y + 8;
    selected_.reset();
    if (!selected_id.empty()) {
      for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == selected_id) {
          selected_ = i;
        }
      }
    }
    hovered_.reset();
    hit_ = Hit::kNone;
    LoadFavicons();
    PreferredSizeChanged();
    SchedulePaint();
  }

  void LoadFavicons() {
    Profile* profile = page_->browser()->GetProfile();
    favicon::FaviconService* service = FaviconServiceFactory::GetForProfile(
        profile, ServiceAccessType::EXPLICIT_ACCESS);
    if (!service) {
      return;
    }
    for (const Item& item : items_) {
      if (item.section || favicons_.contains(item.url) ||
          loading_.contains(item.url)) {
        continue;
      }
      loading_.insert(item.url);
      service->GetFaviconImageForPageURL(
          item.url,
          base::BindOnce(
              [](base::WeakPtr<ArchiveList> list, GURL url,
                 const favicon_base::FaviconImageResult& result) {
                if (!list) {
                  return;
                }
                list->loading_.erase(url);
                list->favicons_[url] = result.image.AsImageSkia();
                list->SchedulePaint();
              },
              weak_factory_.GetWeakPtr(), item.url),
          &tracker_);
    }
  }

  gfx::Rect RemoveRect(const Item& item) const {
    return gfx::Rect(width() - kActionRight - kActionSize,
                     item.y + (item.height - kActionSize) / 2, kActionSize,
                     kActionSize);
  }

  gfx::Rect RestoreRect(const Item& item) const {
    gfx::Rect rect = RemoveRect(item);
    rect.Offset(-kActionSize - kActionGap, 0);
    return rect;
  }

  Hit HitAt(const gfx::Point& point, std::optional<size_t>* item) const {
    item->reset();
    if (more_ > 0 && point.y() >= height_ - kRowHeight - 8 &&
        point.y() < height_) {
      return Hit::kMore;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
      const Item& row = items_[i];
      if (row.section || point.y() < row.y || point.y() >= row.y + row.height) {
        continue;
      }
      *item = i;
      if (RemoveRect(row).Contains(point)) {
        return Hit::kRemove;
      }
      if (RestoreRect(row).Contains(point)) {
        return Hit::kRestore;
      }
      return Hit::kRow;
    }
    return Hit::kNone;
  }

  void UpdateHover(const gfx::Point& point) {
    std::optional<size_t> item;
    const Hit hit = HitAt(point, &item);
    if (hit != hit_ || item != hovered_) {
      hit_ = hit;
      hovered_ = item;
      SchedulePaint();
    }
  }

  void PaintSection(gfx::Canvas* canvas, const Item& item) {
    const gfx::FontList& font = UiFont(11, gfx::Font::Weight::SEMIBOLD);
    const int text_height = font.GetHeight();
    const int y = item.y + item.height - text_height - 3;
    const int title_width = gfx::GetStringWidth(item.title, font);
    canvas->DrawStringRectWithFlags(
        item.title, font, WithAlpha(theme_.on_surface_variant, 0.9f),
        gfx::Rect(kLeading, y, title_width, text_height),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    const std::u16string count = base::NumberToString16(item.count);
    const gfx::FontList& count_font = UiFont(11);
    const int count_width = gfx::GetStringWidth(count, count_font);
    canvas->DrawStringRectWithFlags(count, count_font,
                                    WithAlpha(theme_.on_surface_variant, 0.6f),
                                    gfx::Rect(width() - kLeading - count_width,
                                              y, count_width, text_height),
                                    gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void PaintRow(gfx::Canvas* canvas, size_t index, const Item& item) {
    const bool hovered = hovered_ == index && hit_ != Hit::kNone;
    const bool selected = selected_ == index;
    const gfx::RectF rect(0, item.y, width(), item.height);
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    if (selected) {
      fill.setColor(WithAlpha(theme_.accent, theme_.is_dark ? 0.18f : 0.13f));
      canvas->DrawRoundRect(rect, radius::kRow, fill);
    } else if (hovered) {
      fill.setColor(theme_.OnSurface(0.06f));
      canvas->DrawRoundRect(rect, radius::kRow, fill);
    }
    // 网站图标；还没取到（或者没有）时是地球。
    const gfx::Rect icon(kLeading, item.y + (item.height - kIconSize) / 2,
                         kIconSize, kIconSize);
    auto favicon = favicons_.find(item.url);
    if (favicon != favicons_.end() && !favicon->second.isNull()) {
      canvas->DrawImageInt(favicon->second, 0, 0, favicon->second.width(),
                           favicon->second.height(), icon.x(), icon.y(),
                           icon.width(), icon.height(), true);
    } else {
      PaintIconAt(canvas, icons::kGlobe, kIconSize,
                  WithAlpha(theme_.on_surface_variant, 0.7f), icon.origin());
    }
    int right = width() - kLeading;
    if (hovered) {
      right = RestoreRect(item).x() - 4;
      for (const auto& [hit, glyph, size, box] :
           {std::tuple(Hit::kRestore, &icons::kArrowUturnBackward, 13,
                       RestoreRect(item)),
            std::tuple(Hit::kRemove, &icons::kXmark, 10, RemoveRect(item))}) {
        if (hit_ == hit) {
          fill.setColor(theme_.OnSurface(0.1f));
          canvas->DrawRoundRect(gfx::RectF(box), radius::kButton, fill);
        }
        PaintIconAt(canvas, *glyph, size,
                    hit_ == hit ? theme_.on_surface : theme_.on_surface_variant,
                    gfx::Point(box.x() + (box.width() - size) / 2,
                               box.y() + (box.height() - size) / 2));
      }
    }
    const gfx::FontList& title_font = UiFont(12, gfx::Font::Weight::MEDIUM);
    const gfx::FontList& detail_font = UiFont(11);
    const int text_width = std::max(0, right - kTextX);
    const int block = title_font.GetHeight() + 1 + detail_font.GetHeight();
    const int top = item.y + (item.height - block) / 2;
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(item.title, title_font, text_width, gfx::ELIDE_TAIL),
        title_font, theme_.on_surface,
        gfx::Rect(kTextX, top, text_width, title_font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(item.detail, detail_font, text_width, gfx::ELIDE_TAIL),
        detail_font, theme_.on_surface_variant,
        gfx::Rect(kTextX, top + title_font.GetHeight() + 1, text_width,
                  detail_font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void PaintEmpty(gfx::Canvas* canvas) {
    const int mid = 56;
    PaintIconAt(canvas, icons::kArchivebox, 28,
                WithAlpha(theme_.on_surface_variant, 0.6f),
                gfx::Point(width() / 2 - 14, mid - 28));
    const std::u16string text =
        query_.empty() ? L("archive.empty.title")
                       : L("library.search.noResults", {{"query", query_}});
    canvas->DrawStringRectWithFlags(
        text, UiFont(12, gfx::Font::Weight::MEDIUM), theme_.on_surface_variant,
        gfx::Rect(kLeading, mid + 10, std::max(0, width() - 2 * kLeading), 18),
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  const raw_ptr<ArchivePage> page_;
  Theme theme_ = Theme::DarkBase();
  std::u16string query_;
  std::vector<Item> items_;
  size_t more_ = 0;
  size_t total_ = 0;
  int height_ = 0;
  std::optional<size_t> hovered_;
  std::optional<size_t> selected_;
  Hit hit_ = Hit::kNone;
  bool pressed_ = false;
  std::map<GURL, gfx::ImageSkia> favicons_;
  std::set<GURL> loading_;
  base::CancelableTaskTracker tracker_;
  base::WeakPtrFactory<ArchiveList> weak_factory_{this};
};

BEGIN_METADATA(ArchiveList)
END_METADATA

ArchivePage::ArchivePage(BrowserWindowInterface* browser) : browser_(browser) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetWindowDraggable(this, false);
  back_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&ArchivePage::Close, base::Unretained(this)),
      icons::kChevronLeft, L("archive.page.back"), kButtonSize, 14));
  full_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&ArchivePage::OpenFullPage, base::Unretained(this)),
      icons::kArrowUpLeftAndArrowDownRight, L("archive.page.openFull"),
      kButtonSize, 13));
  auto search = std::make_unique<views::Textfield>();
  search->SetController(this);
  search->SetBackgroundEnabled(false);
  search->SetBorder(nullptr);
  search->SetFontList(UiFont(12));
  search->SetPlaceholderText(L("archive.page.searchPlaceholder"));
  search->GetViewAccessibility().SetName(L("archive.page.searchPlaceholder"));
  search_ = AddChildView(std::move(search));
  scroll_ = AddChildView(std::make_unique<ElasticScrollView>(
      ElasticScrollView::Axis::kVertical,
      ElasticScrollView::Elasticity::kAlways));
  scroll_->SetBackgroundColor(std::nullopt);
  scroll_->SetDrawOverflowIndicator(false);
  scroll_->SetVerticalScrollBarMode(
      views::ScrollView::ScrollBarMode::kHiddenButEnabled);
  scroll_->SetHorizontalScrollBarMode(
      views::ScrollView::ScrollBarMode::kDisabled);
  list_ = scroll_->SetContents(std::make_unique<ArchiveList>(this));
  if (!browser->GetProfile()->IsOffTheRecord()) {
    observation_.Observe(
        tab_organizer::TabArchive::ForProfile(browser->GetProfile()));
  }
  Reload();
}

ArchivePage::~ArchivePage() = default;

void ArchivePage::SetTheme(const Theme& theme) {
  theme_ = theme;
  back_->SetTheme(theme);
  full_->SetTheme(theme);
  search_->SetColor(theme.on_surface);
  list_->SetTheme(theme);
  SchedulePaint();
}

void ArchivePage::Prepare() {
  if (!search_->GetText().empty()) {
    search_->SetText(std::u16string());
  }
  Reload();
  scroll_->ScrollToPosition(scroll_->vertical_scroll_bar(), 0);
}

void ArchivePage::FocusSearch() {
  search_->RequestFocus();
}

void ArchivePage::Close() {
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    if (SidebarView* sidebar = view->sidebar()) {
      sidebar->ShowArchive(false);
    }
  }
}

std::string ArchivePage::DescribeForTesting() const {
  std::string text = base::StrCat(
      {"archivepage query=\"", base::UTF16ToUTF8(search_->GetText()),
       "\" rows=", base::NumberToString(list_->row_count()),
       " total=", base::NumberToString(list_->total()),
       " visible=", GetVisible() ? "1" : "0", "\n"});
  for (const ArchiveList::Item& item : list_->items()) {
    base::StrAppend(
        &text, {item.section ? "  # " : "    ", base::UTF16ToUTF8(item.title)});
    if (item.section) {
      base::StrAppend(&text, {" ", base::NumberToString(item.count)});
    } else {
      base::StrAppend(&text, {" | ", base::UTF16ToUTF8(item.detail)});
    }
    text += "\n";
  }
  return text;
}

void ArchivePage::SetQueryForTesting(const std::u16string& query) {
  search_->SetText(query);
  ContentsChanged(search_, query);
}

bool ArchivePage::ClickRowForTesting(size_t index, const std::string& button) {
  const std::optional<size_t> item = list_->RowItem(index);
  if (!item) {
    return false;
  }
  list_->Activate(*item, button == "restore"  ? ArchiveList::Hit::kRestore
                         : button == "remove" ? ArchiveList::Hit::kRemove
                                              : ArchiveList::Hit::kRow);
  return true;
}

gfx::Rect ArchivePage::SearchRect() const {
  return gfx::Rect(0, kSearchTop, width(), kSearchHeight);
}

gfx::Rect ArchivePage::FooterRect() const {
  return gfx::Rect(0, height() - kFooterHeight, width(), kFooterHeight);
}

std::u16string ArchivePage::SettingsSummary() const {
  const tab_organizer::Settings settings =
      tab_organizer::ReadSettings(browser_->GetProfile());
  if (!settings.auto_archive) {
    return L("archive.subtitle.autoOff");
  }
  return L(
      "archive.subtitle.auto",
      {{"duration", DurationLabel(static_cast<int>(settings.idle.InHours()))}});
}

gfx::Rect ArchivePage::FullLinkRect() const {
  const gfx::Rect footer = FooterRect();
  const int width =
      gfx::GetStringWidth(L("archive.page.fullLink"), UiFont(11)) + 12;
  return gfx::Rect(footer.right() - width - 4, footer.y() + 4, width,
                   footer.height() - 8);
}

gfx::Rect ArchivePage::SettingsLinkRect() const {
  const gfx::Rect footer = FooterRect();
  const int right = FullLinkRect().x() - 6;
  const int width = std::min(
      right - 4, gfx::GetStringWidth(SettingsSummary(), UiFont(11)) + 12);
  return gfx::Rect(footer.x() + 4, footer.y() + 4, std::max(0, width),
                   footer.height() - 8);
}

ArchivePage::Link ArchivePage::LinkAt(const gfx::Point& point) const {
  if (FullLinkRect().Contains(point)) {
    return Link::kFullPage;
  }
  if (SettingsLinkRect().Contains(point)) {
    return Link::kSettings;
  }
  return Link::kNone;
}

void ArchivePage::Layout(PassKey) {
  back_->SetBounds(0, (kHeaderHeight - kButtonSize) / 2, kButtonSize,
                   kButtonSize);
  full_->SetBounds(width() - kButtonSize, (kHeaderHeight - kButtonSize) / 2,
                   kButtonSize, kButtonSize);
  const gfx::Rect search = SearchRect();
  search_->SetBounds(search.x() + 28, search.y() + 4,
                     std::max(0, search.width() - 28 - 8), search.height() - 8);
  const int list_bottom = height() - kFooterHeight - 4;
  scroll_->SetBounds(0, kListTop, width(), std::max(0, list_bottom - kListTop));
}

void ArchivePage::OnPaint(gfx::Canvas* canvas) {
  // 标题：「已归档」13 半粗、数量 11 灰。
  const gfx::FontList& title_font = UiFont(13, gfx::Font::Weight::SEMIBOLD);
  const std::u16string title = L("archive.page.title");
  const int title_x = kButtonSize + 4;
  const int title_width = gfx::GetStringWidth(title, title_font);
  canvas->DrawStringRectWithFlags(
      title, title_font, theme_.on_surface,
      gfx::Rect(title_x, 0, title_width, kHeaderHeight),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
  canvas->DrawStringRectWithFlags(
      base::NumberToString16(list_->total()), UiFont(11),
      theme_.on_surface_variant,
      gfx::Rect(title_x + title_width + 6, 0, 60, kHeaderHeight),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);

  // 搜索框：浅底的圆角条，左边放大镜。
  const gfx::Rect search = SearchRect();
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(theme_.OnSurface(search_->HasFocus() ? 0.08f : 0.06f));
  canvas->DrawRoundRect(gfx::RectF(search), radius::kRow, fill);
  PaintIconAt(canvas, icons::kMagnifyingGlass, 12,
              WithAlpha(theme_.on_surface_variant, 0.85f),
              gfx::Point(search.x() + 10, search.CenterPoint().y() - 6));

  // 最下面：自动归档的设置、整页查看。
  const gfx::Rect footer = FooterRect();
  canvas->FillRect(
      gfx::Rect(footer.x() + 6, footer.y(), footer.width() - 12, 1),
      theme_.OnSurface(0.07f));
  for (const auto& [link, rect, text, align] :
       {std::tuple(Link::kSettings, SettingsLinkRect(), SettingsSummary(),
                   gfx::Canvas::TEXT_ALIGN_LEFT),
        std::tuple(Link::kFullPage, FullLinkRect(), L("archive.page.fullLink"),
                   gfx::Canvas::TEXT_ALIGN_RIGHT)}) {
    if (hovered_link_ == link) {
      fill.setColor(theme_.OnSurface(0.06f));
      canvas->DrawRoundRect(gfx::RectF(rect), radius::kButton, fill);
    }
    gfx::Rect text_rect = rect;
    text_rect.Inset(gfx::Insets::VH(0, 6));
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(text, UiFont(11), text_rect.width(), gfx::ELIDE_TAIL),
        UiFont(11),
        link == Link::kFullPage ? theme_.accent : theme_.on_surface_variant,
        text_rect, align | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
}

bool ArchivePage::OnMousePressed(const ui::MouseEvent& event) {
  pressed_link_ =
      event.IsOnlyLeftMouseButton() ? LinkAt(event.location()) : Link::kNone;
  return pressed_link_ != Link::kNone;
}

void ArchivePage::OnMouseReleased(const ui::MouseEvent& event) {
  const Link link = LinkAt(event.location());
  const bool clicked = link != Link::kNone && link == pressed_link_;
  pressed_link_ = Link::kNone;
  if (!clicked) {
    return;
  }
  if (link == Link::kFullPage) {
    OpenFullPage();
  } else {
    OpenSettings();
  }
}

void ArchivePage::OnMouseMoved(const ui::MouseEvent& event) {
  const Link link = LinkAt(event.location());
  if (link != hovered_link_) {
    hovered_link_ = link;
    SchedulePaint();
  }
}

void ArchivePage::OnMouseExited(const ui::MouseEvent& event) {
  if (hovered_link_ != Link::kNone) {
    hovered_link_ = Link::kNone;
    SchedulePaint();
  }
}

ui::Cursor ArchivePage::GetCursor(const ui::MouseEvent& event) {
  return LinkAt(event.location()) != Link::kNone
             ? ui::Cursor(ui::mojom::CursorType::kHand)
             : ui::Cursor();
}

bool ArchivePage::OnKeyPressed(const ui::KeyEvent& event) {
  if (event.key_code() == ui::VKEY_ESCAPE) {
    Close();
    return true;
  }
  return false;
}

void ArchivePage::ContentsChanged(views::Textfield* sender,
                                  const std::u16string& new_contents) {
  Reload();
  scroll_->ScrollToPosition(scroll_->vertical_scroll_bar(), 0);
}

bool ArchivePage::HandleKeyEvent(views::Textfield* sender,
                                 const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  switch (key_event.key_code()) {
    case ui::VKEY_ESCAPE:
      if (!search_->GetText().empty()) {
        search_->SetText(std::u16string());
        ContentsChanged(search_, std::u16string());
      } else {
        Close();
      }
      return true;
    case ui::VKEY_DOWN:
      list_->MoveSelection(1);
      return true;
    case ui::VKEY_UP:
      list_->MoveSelection(-1);
      return true;
    case ui::VKEY_RETURN:
      if (const std::optional<size_t> selected = list_->selected()) {
        list_->Activate(*selected, ArchiveList::Hit::kRow);
      } else if (const std::optional<size_t> first = list_->RowItem(0)) {
        list_->Activate(*first, ArchiveList::Hit::kRow);
      }
      return true;
    default:
      return false;
  }
}

void ArchivePage::OnArchiveChanged() {
  Reload();
}

void ArchivePage::Reload() {
  list_->Rebuild(std::u16string(search_->GetText()));
  InvalidateLayout();
  SchedulePaint();
}

void ArchivePage::OpenFullPage() {
  const std::u16string query(search_->GetText());
  Close();
  OpenStarryArchive(browser_, query);
}

void ArchivePage::OpenSettings() {
  OpenStarrySettings(browser_, "tabs");
}

BEGIN_METADATA(ArchivePage)
END_METADATA

}  // namespace starry
