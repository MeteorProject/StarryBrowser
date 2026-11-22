// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/command_bar/command_bar_home.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/case_conversion.h"
#include "base/i18n/number_formatting.h"
#include "base/strings/escape.h"
#include "base/strings/utf_string_conversions.h"
#include "base/timer/timer.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/favicon/large_icon_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/favicon/core/large_icon_service.h"
#include "components/favicon_base/fallback_icon_style.h"
#include "components/favicon_base/favicon_types.h"
#include "components/url_formatter/url_formatter.h"
#include "starry/browser/content_blocking/blocked_stats.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/new_tab/new_tab.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view_targeter.h"
#include "url/gurl.h"

namespace starry {

namespace {

constexpr int kTileWidth = 96;
constexpr int kTileBox = 64;
constexpr int kTileIcon = 32;
constexpr int kTileHeight = 92;
constexpr int kTileGapX = 14;
constexpr int kTileGapY = 16;
constexpr int kColumns = 6;
constexpr size_t kMaxTiles = 12;
constexpr int kTopGap = 40;
constexpr size_t kMaxCards = 3;
constexpr int kCardHeight = 58;
constexpr int kCardGap = 14;
constexpr int kSectionGap = 34;
constexpr int kHomeSectionTitleHeight = 26;
constexpr int kFooterHeight = 24;
constexpr int kFooterBottom = 22;
constexpr float kRevealDistance = 14;
constexpr int kMenuRemove = 1;

int ContentWidth() {
  return kColumns * kTileWidth + (kColumns - 1) * kTileGapX;
}

double RevealDelay(int index) {
  return 0.06 + std::min(index, motion::kStaggerLimit) * motion::kStaggerStep;
}

class Revealer {
 public:
  explicit Revealer(views::View* view) : view_(view) {}

  void Run(double delay) {
    ui::Layer* layer = view_->layer();
    const float distance = motion::ReduceMotion() ? 0 : kRevealDistance;
    motion_.SetOpacity(layer, 0);
    LayerPose pose;
    pose.offset = gfx::Vector2dF(0, distance);
    pose.scale = motion::ReduceMotion() ? 1 : 0.98f;
    motion_.SetTransform(layer, pose, gfx::PointF(view_->width() / 2.0f, 0));
    timer_.Start(
        FROM_HERE, base::Seconds(delay),
        base::BindOnce(&Revealer::Start, base::Unretained(this), pose));
  }

  void Stop() {
    timer_.Stop();
    motion_.SetOpacity(view_->layer(), 1);
    motion_.SetTransform(view_->layer(), LayerPose());
  }

 private:
  void Start(LayerPose from) {
    const MotionCurve curve = MotionCurve::Of(motion::kReveal);
    motion_.AnimateOpacityFrom(view_->layer(), 0, 1, curve);
    motion_.AnimateTransformFrom(view_->layer(), from, LayerPose(), curve,
                                 gfx::PointF(view_->width() / 2.0f, 0));
  }

  const raw_ptr<views::View> view_;
  LayerMotion motion_;
  base::OneShotTimer timer_;
};

SkColor WithAlpha(SkColor color, float alpha) {
  return SkColorSetA(color, static_cast<U8CPU>(255 * alpha));
}

std::u16string HostLabel(const GURL& url) {
  return url_formatter::FormatUrl(
      url.GetWithEmptyPath(),
      url_formatter::kFormatUrlOmitDefaults |
          url_formatter::kFormatUrlOmitTrivialSubdomains |
          url_formatter::kFormatUrlOmitHTTPS |
          url_formatter::kFormatUrlOmitTrailingSlashOnBareHostname,
      base::UnescapeRule::SPACES, nullptr, nullptr, nullptr);
}

}  // namespace

class CommandBarHome::Tile : public views::View {
  METADATA_HEADER(Tile, views::View)

 public:
  Tile(CommandBarHome* home, const new_tab::Shortcuts::Tile& tile)
      : home_(home), url_(tile.url), title_(tile.title) {
    if (title_.empty()) {
      title_ = HostLabel(url_);
    }
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    GetViewAccessibility().SetRole(ax::mojom::Role::kLink);
    GetViewAccessibility().SetName(title_);
    SetTooltipText(base::UTF8ToUTF16(url_.spec()));
  }

  const GURL& url() const { return url_; }
  Revealer& revealer() { return revealer_; }

  void SetIcon(const gfx::ImageSkia& icon) {
    icon_ = icon;
    SchedulePaint();
  }
  void SetFallback(SkColor background, SkColor text) {
    fallback_background_ = background;
    fallback_text_ = text;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const Theme& theme = home_->theme_;
    const gfx::RectF box((width() - kTileBox) / 2.0f, 0, kTileBox, kTileBox);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme.OnSurface(hovered_ ? 0.15f : 0.09f));
    canvas->DrawRoundRect(box, 18, flags);
    cc::PaintFlags line;
    line.setAntiAlias(true);
    line.setStyle(cc::PaintFlags::kStroke_Style);
    line.setStrokeWidth(1);
    line.setColor(theme.OnSurface(0.08f));
    gfx::RectF stroke = box;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, 17.5f, line);

    const gfx::Rect icon(
        static_cast<int>(box.CenterPoint().x()) - kTileIcon / 2,
        static_cast<int>(box.CenterPoint().y()) - kTileIcon / 2, kTileIcon,
        kTileIcon);
    if (!icon_.isNull()) {
      canvas->DrawImageInt(icon_, 0, 0, icon_.width(), icon_.height(), icon.x(),
                           icon.y(), icon.width(), icon.height(), true);
    } else {
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(fallback_background_.value_or(theme.OnSurface(0.18f)));
      canvas->DrawRoundRect(gfx::RectF(icon), 9, fill);
      const std::u16string label = HostLabel(url_);
      std::u16string letter =
          label.empty() ? u"?" : base::i18n::ToUpper(label.substr(0, 1));
      canvas->DrawStringRectWithFlags(
          letter, UiFont(16, gfx::Font::Weight::BOLD),
          fallback_text_.value_or(theme.on_surface), icon,
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }
    canvas->DrawStringRectWithFlags(
        title_, UiFont(12), WithAlpha(theme.on_surface, 0.86f),
        gfx::Rect(0, kTileBox + 8, width(), 16),
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (event.IsOnlyRightMouseButton()) {
      gfx::Point point = event.location();
      views::View::ConvertPointToScreen(this, &point);
      home_->TileMenu(this, point);
      return true;
    }
    return event.IsOnlyLeftMouseButton();
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (event.IsOnlyLeftMouseButton() && HitTestPoint(event.location())) {
      home_->TileClicked(this);
    }
  }
  void OnMouseEntered(const ui::MouseEvent& event) override {
    hovered_ = true;
    SchedulePaint();
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_ = false;
    SchedulePaint();
  }

 private:
  const raw_ptr<CommandBarHome> home_;
  const GURL url_;
  std::u16string title_;
  gfx::ImageSkia icon_;
  std::optional<SkColor> fallback_background_;
  std::optional<SkColor> fallback_text_;
  bool hovered_ = false;
  Revealer revealer_{this};
};

BEGIN_METADATA(CommandBarHome, Tile)
END_METADATA

class CommandBarHome::Card : public views::View {
  METADATA_HEADER(Card, views::View)

 public:
  Card(CommandBarHome* home, const new_tab::ClosedEntry& entry)
      : home_(home), entry_(entry) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    GetViewAccessibility().SetRole(ax::mojom::Role::kButton);
    GetViewAccessibility().SetName(Title());
  }

  const new_tab::ClosedEntry& entry() const { return entry_; }
  Revealer& revealer() { return revealer_; }

  void SetIcon(const gfx::ImageSkia& icon) {
    icon_ = icon;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const Theme& theme = home_->theme_;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme.OnSurface(hovered_ ? 0.13f : 0.08f));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kCard, flags);
    const int icon_size = 20;
    const gfx::Rect icon(14, (height() - icon_size) / 2, icon_size, icon_size);
    if (!icon_.isNull()) {
      canvas->DrawImageInt(icon_, 0, 0, icon_.width(), icon_.height(), icon.x(),
                           icon.y(), icon.width(), icon.height(), true);
    } else {
      gfx::ScopedCanvas scoped(canvas);
      canvas->Translate(icon.OffsetFromOrigin() + gfx::Vector2d(2, 2));
      gfx::PaintVectorIcon(canvas, icons::kGlobe, icon_size - 4,
                           theme.on_surface_variant);
    }
    const int x = icon.right() + 10;
    const int text_width = std::max(0, width() - x - 12);
    canvas->DrawStringRectWithFlags(
        Title(), UiFont(12, gfx::Font::Weight::MEDIUM), theme.on_surface,
        gfx::Rect(x, 11, text_width, 18), gfx::Canvas::NO_SUBPIXEL_RENDERING);
    std::u16string detail =
        entry_.window ? L("newTab.closed.window",
                          {{"count", entry_.tab_count},
                           {"time", new_tab::TimeAgo(entry_.closed)}})
                      : L("newTab.closed.tab",
                          {{"time", new_tab::TimeAgo(entry_.closed)}});
    canvas->DrawStringRectWithFlags(
        detail, UiFont(11), theme.on_surface_variant,
        gfx::Rect(x, 30, text_width, 16), gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    return event.IsOnlyLeftMouseButton();
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (event.IsOnlyLeftMouseButton() && HitTestPoint(event.location())) {
      home_->CardClicked(this);
    }
  }
  void OnMouseEntered(const ui::MouseEvent& event) override {
    hovered_ = true;
    SchedulePaint();
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_ = false;
    SchedulePaint();
  }

 private:
  std::u16string Title() const {
    return entry_.title.empty() ? HostLabel(entry_.url) : entry_.title;
  }

  const raw_ptr<CommandBarHome> home_;
  const new_tab::ClosedEntry entry_;
  gfx::ImageSkia icon_;
  bool hovered_ = false;
  Revealer revealer_{this};
};

BEGIN_METADATA(CommandBarHome, Card)
END_METADATA

class CommandBarHome::Footer : public views::View {
  METADATA_HEADER(Footer, views::View)

 public:
  enum Item { kShield = 0, kMedia = 1, kCustomize = 2 };

  explicit Footer(CommandBarHome* home) : home_(home) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  Revealer& revealer() { return revealer_; }

  void Set(std::optional<int> blocked, std::u16string media) {
    items_.clear();
    if (blocked) {
      items_.push_back({kShield, &icons::kShieldLefthalfFilled,
                        L("newTab.overlay.blocked",
                          {{"count", base::FormatNumber(*blocked)}})});
    }
    if (!media.empty()) {
      items_.push_back({kMedia, &icons::kSpeakerWave, std::move(media)});
    }
    items_.push_back(
        {kCustomize, &icons::kSliderHorizontal3, L("newTab.customize")});
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const Theme& theme = home_->theme_;
    const gfx::FontList& font = UiFont(12);
    constexpr int kIcon = 13;
    constexpr int kGap = 22;
    int total = 0;
    for (const Entry& item : items_) {
      total += kIcon + 6 + std::min(gfx::GetStringWidth(item.text, font), 240) +
               kGap;
    }
    total -= kGap;
    int x = (width() - total) / 2;
    for (Entry& item : items_) {
      const int text = std::min(gfx::GetStringWidth(item.text, font), 240);
      item.bounds = gfx::Rect(x, 0, kIcon + 6 + text, height());
      const SkColor color =
          item.hovered ? theme.on_surface : theme.on_surface_variant;
      {
        gfx::ScopedCanvas scoped(canvas);
        canvas->Translate(gfx::Vector2d(x, (height() - kIcon) / 2));
        gfx::PaintVectorIcon(canvas, *item.icon, kIcon, color);
      }
      canvas->DrawStringRectWithFlags(
          item.text, font, color, gfx::Rect(x + kIcon + 6, 0, text, height()),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
      x += item.bounds.width() + kGap;
    }
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    return event.IsOnlyLeftMouseButton() && ItemAt(event.location());
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (Entry* item = ItemAt(event.location())) {
      home_->FooterClicked(item->id);
    }
  }
  void OnMouseMoved(const ui::MouseEvent& event) override {
    UpdateHover(event.location());
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    UpdateHover(gfx::Point(-1, -1));
  }

  bool HasItemAt(const gfx::Point& point) const {
    return std::ranges::any_of(items_, [&](const Entry& item) {
      return item.bounds.Contains(point);
    });
  }

 private:
  struct Entry {
    int id;
    raw_ptr<const gfx::VectorIcon> icon;
    std::u16string text;
    gfx::Rect bounds;
    bool hovered = false;
  };

  Entry* ItemAt(const gfx::Point& point) {
    for (Entry& item : items_) {
      if (item.bounds.Contains(point)) {
        return &item;
      }
    }
    return nullptr;
  }

  void UpdateHover(const gfx::Point& point) {
    bool changed = false;
    for (Entry& item : items_) {
      const bool hovered = item.bounds.Contains(point);
      changed |= hovered != item.hovered;
      item.hovered = hovered;
    }
    if (changed) {
      SchedulePaint();
    }
  }

  const raw_ptr<CommandBarHome> home_;
  std::vector<Entry> items_;
  Revealer revealer_{this};
};

BEGIN_METADATA(CommandBarHome, Footer)
END_METADATA

class CommandBarHome::SectionTitle : public views::View {
  METADATA_HEADER(SectionTitle, views::View)

 public:
  SectionTitle() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }
  Revealer& revealer() { return revealer_; }
  void SetColor(SkColor color) {
    color_ = color;
    SchedulePaint();
  }
  void OnPaint(gfx::Canvas* canvas) override {
    canvas->DrawStringRectWithFlags(
        L("newTab.continue"), UiFont(12, gfx::Font::Weight::SEMIBOLD), color_,
        GetLocalBounds(), gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  SkColor color_ = SK_ColorWHITE;
  Revealer revealer_{this};
};

BEGIN_METADATA(CommandBarHome, SectionTitle)
END_METADATA

CommandBarHome::CommandBarHome(BrowserWindowInterface* browser,
                               Delegate* delegate)
    : browser_(browser), delegate_(delegate) {
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  cards_title_ = AddChildView(std::make_unique<SectionTitle>());
  footer_ = AddChildView(std::make_unique<Footer>(this));
}

CommandBarHome::~CommandBarHome() = default;

void CommandBarHome::SetTheme(const Theme& theme) {
  theme_ = theme;
  cards_title_->SetColor(theme.on_surface_variant);
  for (views::View* child : children()) {
    child->SchedulePaint();
  }
}

void CommandBarHome::Refresh(bool reveal) {
  Profile* profile = browser_->GetProfile();
  if (!shortcuts_ && new_tab::WidgetEnabled(profile, "shortcuts")) {
    // 延迟到首次打开再创建，避免启动时查询历史。
    shortcuts_ = std::make_unique<new_tab::Shortcuts>(
        profile, kMaxTiles,
        base::BindRepeating(&CommandBarHome::ShortcutsChanged,
                            weak_factory_.GetWeakPtr()));
  }
  shown_ = true;
  SetCanProcessEventsWithinSubtree(true);
  if (reveal) {
    motion_.SetOpacity(layer(), 1);
  } else {
    // 退场中重新打开时，从当前透明度接回去。
    motion_.AnimateOpacity(layer(), 1,
                           MotionCurve::Of(motion::kCommandBarOpen));
  }
  motion_.SetTransform(layer(), LayerPose());
  RebuildTiles(reveal);
  RebuildCards(reveal);

  std::optional<int> blocked;
  if (new_tab::WidgetEnabled(profile, "shield")) {
    blocked = content_blocking::BlockedThisWeek(profile).total;
  }
  std::u16string media;
  if (new_tab::WidgetEnabled(profile, "playing")) {
    if (const MediaEntry* entry = MediaHub::Get(profile)->Primary()) {
      media = entry->title;
    }
  }
  footer_->Set(blocked, std::move(media));
  if (reveal) {
    cards_title_->revealer().Run(RevealDelay(static_cast<int>(tiles_.size())));
    footer_->revealer().Run(0.06 + motion::kStaggerStep *
                                       (tiles_.size() + cards_.size() + 2));
  } else {
    cards_title_->revealer().Stop();
    footer_->revealer().Stop();
  }
}

void CommandBarHome::ShortcutsChanged() {
  if (GetVisible()) {
    RebuildTiles(/*reveal=*/false);
  }
}

void CommandBarHome::RebuildTiles(bool reveal) {
  for (Tile* tile : tiles_) {
    RemoveChildViewT(tile);
  }
  tiles_.clear();
  if (!shortcuts_ ||
      !new_tab::WidgetEnabled(browser_->GetProfile(), "shortcuts")) {
    return;
  }
  for (const new_tab::Shortcuts::Tile& item : shortcuts_->tiles()) {
    if (tiles_.size() >= kMaxTiles) {
      break;
    }
    Tile* tile = AddChildView(std::make_unique<Tile>(this, item));
    tiles_.push_back(tile);
    LoadIcon(tile);
  }
  InvalidateLayout();
  DeprecatedLayoutImmediately();
  for (size_t i = 0; i < tiles_.size(); ++i) {
    if (reveal) {
      tiles_[i]->revealer().Run(RevealDelay(static_cast<int>(i)));
    }
  }
}

void CommandBarHome::RebuildCards(bool reveal) {
  for (Card* card : cards_) {
    RemoveChildViewT(card);
  }
  cards_.clear();
  Profile* profile = browser_->GetProfile();
  if (new_tab::WidgetEnabled(profile, "continue")) {
    for (const new_tab::ClosedEntry& entry :
         new_tab::RecentlyClosed(profile, kMaxCards)) {
      Card* card = AddChildView(std::make_unique<Card>(this, entry));
      cards_.push_back(card);
      favicon::LargeIconService* service =
          LargeIconServiceFactory::GetForBrowserContext(profile);
      if (service) {
        service->GetLargeIconImageOrFallbackStyleForPageUrl(
            entry.url, 16, 40,
            base::BindOnce(
                [](base::WeakPtr<CommandBarHome> home, SessionID id,
                   const favicon_base::LargeIconImageResult& result) {
                  if (!home || result.image.IsEmpty()) {
                    return;
                  }
                  for (Card* card : home->cards_) {
                    if (card->entry().id == id) {
                      card->SetIcon(result.image.AsImageSkia());
                    }
                  }
                },
                weak_factory_.GetWeakPtr(), entry.id),
            &tracker_);
      }
    }
  }
  cards_title_->SetVisible(!cards_.empty());
  InvalidateLayout();
  DeprecatedLayoutImmediately();
  for (size_t i = 0; i < cards_.size(); ++i) {
    if (reveal) {
      cards_[i]->revealer().Run(
          RevealDelay(static_cast<int>(tiles_.size() + 1 + i)));
    }
  }
}

void CommandBarHome::LoadIcon(Tile* tile) {
  favicon::LargeIconService* service =
      LargeIconServiceFactory::GetForBrowserContext(browser_->GetProfile());
  if (!service) {
    return;
  }
  service->GetLargeIconImageOrFallbackStyleForPageUrl(
      tile->url(), 16, 64,
      base::BindOnce(
          [](base::WeakPtr<CommandBarHome> home, GURL url,
             const favicon_base::LargeIconImageResult& result) {
            if (!home) {
              return;
            }
            for (Tile* tile : home->tiles_) {
              if (tile->url() != url) {
                continue;
              }
              if (!result.image.IsEmpty()) {
                tile->SetIcon(result.image.AsImageSkia());
              } else if (result.fallback_icon_style) {
                tile->SetFallback(result.fallback_icon_style->background_color,
                                  result.fallback_icon_style->text_color);
              }
            }
          },
          weak_factory_.GetWeakPtr(), tile->url()),
      &tracker_);
}

void CommandBarHome::SetPanelBottom(int bottom) {
  if (panel_bottom_ == bottom) {
    return;
  }
  panel_bottom_ = bottom;
  InvalidateLayout();
}

void CommandBarHome::SetShown(bool shown) {
  if (shown_ == shown) {
    return;
  }
  shown_ = shown;
  SetCanProcessEventsWithinSubtree(shown);
  if (motion::ReduceMotion()) {
    motion_.AnimateOpacity(layer(), shown ? 1 : 0, MotionCurve::EaseOut(0.14));
    return;
  }
  LayerPose pose;
  if (!shown) {
    pose.offset = gfx::Vector2dF(0, 16);
    pose.scale = 0.98f;
  }
  const MotionCurve curve =
      shown ? MotionCurve::Of(motion::kReveal) : MotionCurve::EaseOut(0.14);
  motion_.AnimateOpacity(layer(), shown ? 1 : 0, curve);
  motion_.AnimateTransform(layer(), pose, curve,
                           gfx::PointF(width() / 2.0f, panel_bottom_));
}

void CommandBarHome::FadeOut(const MotionCurve& curve) {
  SetCanProcessEventsWithinSubtree(false);
  motion_.AnimateOpacity(layer(), 0, curve);
}

void CommandBarHome::Layout(PassKey) {
  const int content = std::min(ContentWidth(), std::max(0, width() - 32));
  const int columns = std::max(
      1, std::min(kColumns, (content + kTileGapX) / (kTileWidth + kTileGapX)));
  const int footer_top = height() - kFooterBottom - kFooterHeight;
  int y = panel_bottom_ + kTopGap;
  const int rows_fit = std::max(
      0, (footer_top - 20 - y + kTileGapY) / (kTileHeight + kTileGapY));
  const int count = static_cast<int>(tiles_.size());
  int bottom = y;
  for (int i = 0; i < count; ++i) {
    const int row = i / columns;
    const int column = i % columns;
    const bool visible = row < std::min(2, rows_fit);
    tiles_[i]->SetVisible(visible);
    if (!visible) {
      continue;
    }
    const int in_row = std::min(columns, count - row * columns);
    const int row_width = in_row * kTileWidth + (in_row - 1) * kTileGapX;
    tiles_[i]->SetBounds(
        (width() - row_width) / 2 + column * (kTileWidth + kTileGapX),
        y + row * (kTileHeight + kTileGapY), kTileWidth, kTileHeight);
    bottom = tiles_[i]->bounds().bottom();
  }
  if (count > 0) {
    y = bottom + kSectionGap;
  }
  const bool cards_fit =
      !cards_.empty() &&
      y + kHomeSectionTitleHeight + kCardHeight <= footer_top - 20;
  const int card_width =
      (content - (static_cast<int>(kMaxCards) - 1) * kCardGap) /
      static_cast<int>(kMaxCards);
  const int cards = static_cast<int>(cards_.size());
  const int cards_left =
      (width() - (cards * card_width + std::max(0, cards - 1) * kCardGap)) / 2;
  cards_title_->SetVisible(cards_fit);
  cards_title_->SetBounds(cards_left, y, content, kHomeSectionTitleHeight - 8);
  for (int i = 0; i < cards; ++i) {
    cards_[i]->SetVisible(cards_fit);
    cards_[i]->SetBounds(cards_left + i * (card_width + kCardGap),
                         y + kHomeSectionTitleHeight, card_width, kCardHeight);
  }
  if (cards_fit) {
    bottom = y + kHomeSectionTitleHeight + kCardHeight;
  }
  footer_->SetVisible(footer_top >= bottom + 16);
  footer_->SetBounds(0, footer_top, width(), kFooterHeight);
}

bool CommandBarHome::DoesIntersectRect(const views::View* target,
                                       const gfx::Rect& rect) const {
  if (target != this) {
    return views::ViewTargeterDelegate::DoesIntersectRect(target, rect);
  }
  if (!shown_) {
    return false;
  }
  for (const views::View* child : children()) {
    if (child->GetVisible() && child->GetCanProcessEventsWithinSubtree() &&
        child->bounds().Intersects(rect)) {
      if (child == footer_) {
        gfx::Point point = rect.CenterPoint();
        point.Offset(-footer_->x(), -footer_->y());
        return footer_->HasItemAt(point);
      }
      return true;
    }
  }
  return false;
}

void CommandBarHome::TileClicked(Tile* tile) {
  delegate_->HomeOpenUrl(tile->url());
}

void CommandBarHome::TileMenu(Tile* tile, const gfx::Point& screen_point) {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view || !shortcuts_) {
    return;
  }
  menu_url_ = tile->url();
  menu_ = std::make_unique<ui::SimpleMenuModel>(this);
  menu_->AddItemWithIcon(kMenuRemove, L("newTab.shortcut.remove"),
                         ui::ImageModel::FromVectorIcon(icons::kTrash));
  gfx::Point point = screen_point;
  views::View::ConvertPointFromScreen(view, &point);
  MenuOptions options;
  options.destructive = base::BindRepeating(
      [](int command_id) { return command_id == kMenuRemove; });
  view->ShowMenu(menu_.get(), point, base::DoNothing(), std::move(options));
}

void CommandBarHome::ExecuteCommand(int command_id, int event_flags) {
  if (command_id == kMenuRemove && shortcuts_) {
    shortcuts_->Remove(menu_url_);
  }
}

void CommandBarHome::CardClicked(Card* card) {
  const SessionID id = card->entry().id;
  delegate_->HomeDismiss();
  new_tab::RestoreClosed(browser_, id,
                         WindowOpenDisposition::kNewForegroundTab);
}

void CommandBarHome::FooterClicked(int item) {
  BrowserWindowInterface* browser = browser_;
  Profile* profile = browser->GetProfile();
  delegate_->HomeDismiss();
  switch (item) {
    case Footer::kShield:
      OpenStarrySettings(browser, "privacy");
      break;
    case Footer::kMedia:
      if (const MediaEntry* entry = MediaHub::Get(profile)->Primary()) {
        MediaHub::Get(profile)->ShowTab(entry->id);
      }
      break;
    case Footer::kCustomize:
      OpenStarrySettings(browser, "newTab");
      break;
  }
}

BEGIN_METADATA(CommandBarHome)
END_METADATA

}  // namespace starry
