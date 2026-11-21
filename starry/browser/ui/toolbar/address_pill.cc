// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/toolbar/address_pill.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_user_gesture_details.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/url_formatter/url_formatter.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/privacy/fingerprint_protection.h"
#include "starry/browser/privacy/location_simulation.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/popover/content_blocking_popover.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "starry/browser/ui/toolbar/content_blocking_button.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "starry/common/internal_urls.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

constexpr int kPillPadding = 10;
constexpr int kCompactPadding = 8;
constexpr int kCompactHeight = 28;
constexpr int kIconFrame = 14;
constexpr int kIconSize = 11;
constexpr int kStarButton = 16;
constexpr int kStarGlyph = 11;
// 地址与右端小按钮之间的间距。
constexpr int kPillSpacing = 8;
constexpr int kCompactSpacing = 7;
// 盾牌和星号之间。
constexpr int kShieldSpacing = 6;
// 紧凑地址栏里网站名、斜杠和标题之间的间距。
constexpr int kTitleGap = 6;

}  // namespace

AddressSummary SummarizeAddress(content::WebContents* contents) {
  AddressSummary summary;
  summary.name = L("toolbar.address.placeholder");
  if (!contents) {
    return summary;
  }
  const GURL& url = contents->GetVisibleURL();
  if (IsBlankTabUrl(url)) {
    return summary;
  }
  summary.placeholder = false;
  // 内部页面显示对应页面名。
  const InternalPage page = InternalPageForUrl(url);
  summary.page_icon = InternalPageIcon(page);
  switch (page) {
    case InternalPage::kHistory:
      summary.name = L("internalPage.history");
      return summary;
    case InternalPage::kBookmarks:
      summary.name = L("internalPage.bookmarks");
      return summary;
    case InternalPage::kDownloads:
      summary.name = L("internalPage.downloads");
      return summary;
    case InternalPage::kSettings:
      // Starry 设置和补充的浏览器设置使用不同的页面名。
      summary.name = ToChromeURL(url).host() == kStarrySettingsHost
                         ? L("internalPage.settings")
                         : L("internalPage.browserSettings");
      return summary;
    case InternalPage::kExtensions:
      summary.name = L("internalPage.extensions");
      return summary;
    case InternalPage::kArchive:
      summary.name = L("internalPage.archive");
      return summary;
    case InternalPage::kWelcome:
      summary.name = L("internalPage.welcome");
      return summary;
    case InternalPage::kNone:
      break;
  }
  summary.url = url;
  summary.secure = url.SchemeIsCryptographic();
  if (url.has_host() && url.SchemeIsHTTPOrHTTPS()) {
    summary.name = url_formatter::FormatUrl(
        url.GetWithEmptyPath(),
        url_formatter::kFormatUrlOmitDefaults |
            url_formatter::kFormatUrlOmitHTTPS |
            url_formatter::kFormatUrlOmitTrivialSubdomains |
            url_formatter::kFormatUrlOmitTrailingSlashOnBareHostname,
        base::UnescapeRule::SPACES, nullptr, nullptr, nullptr);
  } else {
    summary.name = url_formatter::FormatUrl(
        url, url_formatter::kFormatUrlOmitDefaults, base::UnescapeRule::SPACES,
        nullptr, nullptr, nullptr);
  }
  std::u16string title;
  base::TrimWhitespace(contents->GetTitle(), base::TRIM_ALL, &title);
  if (!title.empty() && title != summary.name &&
      title != base::UTF8ToUTF16(url.spec())) {
    summary.title = std::move(title);
  }
  return summary;
}

AddressPill::AddressPill(BrowserWindowInterface* browser, Style style)
    : views::Button(
          base::BindRepeating(&AddressPill::Pressed, base::Unretained(this))),
      browser_(browser),
      style_(style),
      swap_(1,
            base::BindRepeating(&views::View::SchedulePaint,
                                base::Unretained(this))) {
  SetAnimationDuration(base::Milliseconds(motion::kHoverDuration * 1000));
  // 鼠标移到星号上还算在地址栏上（星号还要显示着）。
  SetNotifyEnterExitOnChild(true);
  // 点在地址栏上不拖动窗口；自己的图层（换布局时飞到另一边、淡入淡出）。
  SetWindowDraggable(this, false);
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  GetViewAccessibility().SetName(L("sidebar.addressBar"));

  star_ = AddChildView(std::make_unique<GlyphButton>(
      base::BindRepeating(&AddressPill::ToggleBookmark, base::Unretained(this)),
      icons::kStar, kStarGlyph, kStarButton));
  star_->SetPaintToLayer();
  star_->layer()->SetFillsBoundsOpaquely(false);
  star_->SetVisible(false);

  shield_ = AddChildView(std::make_unique<ContentBlockingButton>(
      base::BindRepeating(&AddressPill::ShowContentBlockingPopover,
                          base::Unretained(this))));
  shield_->SetPaintToLayer();
  shield_->layer()->SetFillsBoundsOpaquely(false);
  shield_->SetVisible(false);
  content_blocking::ContentBlockingService::Get()->AddObserver(this);
  content_blocking::ContentFilterTabHelper::AddObserver(this);
  fingerprint_prefs_.Init(prefs::PrefsFor(browser_->GetProfile()));
  const auto fingerprint_changed =
      base::BindRepeating(&AddressPill::UpdateShield, base::Unretained(this));
  fingerprint_prefs_.Add(prefs::kFingerprintProtection, fingerprint_changed);
  fingerprint_prefs_.Add(prefs::kFingerprintProtectionExceptions,
                         fingerprint_changed);
  location_subscription_ = AddLocationSimulationObserver(
      base::BindRepeating(&AddressPill::UpdateShield, base::Unretained(this)));

  bookmarks_ =
      BookmarkModelFactory::GetForBrowserContext(browser_->GetProfile());
  if (bookmarks_) {
    bookmarks_observation_.Observe(bookmarks_.get());
  }
  browser_->GetTabStripModel()->AddObserver(this);
  Refresh();
}

AddressPill::~AddressPill() {
  content_blocking::ContentFilterTabHelper::RemoveObserver(this);
  content_blocking::ContentBlockingService::Get()->RemoveObserver(this);
  browser_->GetTabStripModel()->RemoveObserver(this);
}

void AddressPill::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  UpdateStar(/*animate=*/false);
  shield_->SetTheme(theme);
  SchedulePaint();
}

void AddressPill::SetPaneTab(std::optional<tabs::TabHandle> tab) {
  if (pane_tab_ == tab) {
    return;
  }
  pane_tab_ = tab;
  Refresh();
}

void AddressPill::SetDragWindowOnDrag(bool enabled) {
  drag_window_on_drag_ = enabled;
  if (!enabled) {
    pending_window_drag_ = false;
  }
}

tabs::TabInterface* AddressPill::ShownTab() const {
  if (pane_tab_) {
    return pane_tab_->Get();
  }
  return browser_->GetActiveTabInterface();
}

void AddressPill::Refresh() {
  tabs::TabInterface* tab = ShownTab();
  std::optional<tabs::TabHandle> handle;
  if (tab) {
    handle = tab->GetHandle();
  }
  AddressSummary summary = SummarizeAddress(tab ? tab->GetContents() : nullptr);
  const bool tab_changed = handle != shown_tab_;
  shown_tab_ = handle;
  if (summary != summary_) {
    if (tab_changed && GetWidget() && GetVisible()) {
      // 换了标签：网站交叉淡化。
      previous_ = summary_;
      swap_.SetValue(0);
      swap_.AnimateTo(1, motion::AddressSwap());
    }
    summary_ = std::move(summary);
    PreferredSizeChanged();
    SchedulePaint();
  }
  UpdateStar(/*animate=*/false);
  UpdateShield();
}

void AddressPill::UpdateShield() {
  content_blocking::ContentBlockingService* service =
      content_blocking::ContentBlockingService::Get();
  const bool visible =
      summary_.url.is_valid() && summary_.url.SchemeIsHTTPOrHTTPS() &&
      (service->enabled() ||
       FingerprintProtectionEnabled(browser_->GetProfile()) ||
       ResolveLocation(browser_->GetProfile(), summary_.url).simulated);
  if (visible != shield_->GetVisible()) {
    shield_->SetVisible(visible);
    PreferredSizeChanged();
    SchedulePaint();
  }
  if (!visible) {
    return;
  }
  tabs::TabInterface* tab = ShownTab();
  shield_->SetState(content_blocking::ContentFilterTabHelper::BlockedCount(
                        tab ? tab->GetContents() : nullptr),
                    !service->enabled() || service->IsAllowlisted(summary_.url),
                    FingerprintProtectionEnabledForSite(browser_->GetProfile(),
                                                        summary_.url));
  shield_->SetTooltipText(shield_->GetRenderedTooltipText(gfx::Point()));
  shield_->SetAlpha(shield_->sticky() || shield_popover_open_
                        ? 1
                        : static_cast<float>(
                              hover_animation().GetCurrentValue()));
}

void AddressPill::ShowContentBlockingPopover() {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  tabs::TabInterface* tab = ShownTab();
  if (!view || !view->popover_host() || !tab) {
    return;
  }
  PopoverHost* host = view->popover_host();
  // 横向跟着盾牌，纵向仍放在整条地址栏下面。
  const gfx::Rect anchor = views::View::ConvertRectToTarget(
      this, host, gfx::Rect(shield_->x(), 0, shield_->width(), height()));
  host->CancelAutoClose();
  host->Show(std::make_unique<ContentBlockingPopover>(browser_, host,
                                                      tab->GetContents()),
             anchor, PopoverHost::Placement::kBelowRight);
  shield_popover_open_ = true;
  host->SetClosedCallback(base::BindOnce(
      [](base::WeakPtr<AddressPill> self) {
        if (self) {
          self->shield_popover_open_ = false;
          self->UpdateShield();
        }
      },
      weak_factory_.GetWeakPtr()));
  UpdateShield();
}

std::u16string AddressPill::DescribeShieldForTesting() const {
  if (!shield_->GetVisible()) {
    return u"hidden";
  }
  return u"count=" + base::NumberToString16(shield_->count()) +
         u" allowlisted=" + (shield_->allowlisted() ? u"1" : u"0") +
         u" alpha=" + base::NumberToString16(static_cast<int>(
                          100 * (shield_->sticky() || shield_popover_open_
                                     ? 1.0
                                     : hover_animation().GetCurrentValue())));
}

void AddressPill::ChildPreferredSizeChanged(views::View* child) {
  // 盾牌的数字多了一位：重新排右端，并通知按内容定宽的父视图（小窗口工具栏）。
  PreferredSizeChanged();
  SchedulePaint();
}

void AddressPill::OnContentBlockingChanged() {
  UpdateShield();
}

void AddressPill::OnBlockedCountChanged(content::WebContents* contents) {
  tabs::TabInterface* tab = ShownTab();
  if (tab && tab->GetContents() == contents) {
    UpdateShield();
  }
}

void AddressPill::UpdateStar(bool animate) {
  const bool has_url = summary_.url.is_valid();
  const bool bookmarked =
      has_url && bookmarks_ && bookmarks_->loaded() &&
      bookmarks::IsBookmarkedByUser(bookmarks_, summary_.url);
  const bool changed = bookmarked != bookmarked_;
  const bool star_visibility_changed = has_url != star_->GetVisible();
  bookmarked_ = bookmarked;
  star_->SetVisible(has_url);
  if (star_visibility_changed) {
    PreferredSizeChanged();
  }
  star_->SetIcon(bookmarked ? icons::kStarFill : icons::kStar);
  star_->SetColors(bookmarked ? theme_.accent : theme_.on_surface_variant,
                   SK_ColorTRANSPARENT);
  star_->SetTooltipText(bookmarked ? L("toolbar.bookmark.remove.help")
                                   : L("toolbar.bookmark.add.help"));
  star_->SetAlpha(
      bookmarked ? 1 : static_cast<float>(hover_animation().GetCurrentValue()));
  if (changed && (animate || star_toggled_) && !motion::ReduceMotion()) {
    // 图标切换时的缩放淡入替换动效。
    LayerPose from;
    from.scale = 0.5f;
    const gfx::PointF center(star_->width() / 2.0f, star_->height() / 2.0f);
    star_motion_.AnimateTransformFrom(star_->layer(), from, LayerPose(),
                                      MotionCurve::Of(motion::kLift), center);
    star_motion_.AnimateOpacityFrom(star_->layer(), 0.2f, 1, motion::Hover());
  }
}

void AddressPill::ToggleBookmark() {
  if (!bookmarks_ || !bookmarks_->loaded() || !summary_.url.is_valid()) {
    return;
  }
  // 点击星号直接添加或移除书签，不弹出书签卡片。
  star_toggled_ = true;
  if (bookmarks::IsBookmarkedByUser(bookmarks_, summary_.url)) {
    bookmarks::RemoveAllBookmarks(bookmarks_, summary_.url, FROM_HERE);
  } else {
    tabs::TabInterface* tab = ShownTab();
    const std::u16string title =
        tab ? tab->GetContents()->GetTitle() : summary_.name;
    bookmarks::AddIfNotBookmarked(bookmarks_, summary_.url,
                                  title.empty() ? summary_.name : title);
  }
  UpdateStar(/*animate=*/true);
  star_toggled_ = false;
}

void AddressPill::Pressed() {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  // 分屏里不是当前的一格：先让它成为当前标签。
  if (tabs::TabInterface* tab = ShownTab();
      tab && pane_tab_ && tab != browser_->GetActiveTabInterface()) {
    TabStripModel* model = browser_->GetTabStripModel();
    const int index = model->GetIndexOfTab(tab);
    if (index != TabStripModel::kNoTab) {
      model->ActivateTabAt(
          index, TabStripUserGestureDetails(
                     TabStripUserGestureDetails::GestureType::kMouse));
    }
  }
  view->OpenCommandBar(CommandBarView::Mode::kEdit);
}

int AddressPill::AccessoryWidth() const {
  if (!star_->GetVisible()) {
    return 0;
  }
  int width = kStarButton +
              (style_ == Style::kCompact ? kCompactSpacing : kPillSpacing);
  if (shield_->GetVisible()) {
    width += shield_->GetPreferredSize().width() + kShieldSpacing -
             (kStarButton - kStarGlyph) / 2;
  }
  return width;
}

int AddressPill::PreferredContentWidth() const {
  const bool compact = style_ == Style::kCompact;
  const int padding = compact ? kCompactPadding : kPillPadding;
  const int spacing = compact ? kCompactSpacing : kPillSpacing;
  const gfx::FontList& name_font =
      compact ? UiFont(13, summary_.placeholder ? gfx::Font::Weight::NORMAL
                                                : gfx::Font::Weight::MEDIUM)
              : UiFont(13);
  int text_width = gfx::GetStringWidth(summary_.name, name_font);
  if (compact && !summary_.title.empty()) {
    const gfx::FontList& title_font = UiFont(13);
    text_width += kTitleGap + gfx::GetStringWidth(u"/", title_font) +
                  kTitleGap + gfx::GetStringWidth(summary_.title, title_font);
  }
  // DrawStringRectWithFlags 的 RenderText 宽度偶尔比 GetStringWidth 多 1-2px，
  // 留一点余量避免按自然宽度排版时末尾被截成省略号。
  if (text_width > 0) {
    text_width += 2;
  }
  return padding + kIconFrame + spacing + text_width + AccessoryWidth() +
         padding;
}

gfx::Size AddressPill::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(
      PreferredContentWidth(),
      style_ == Style::kPill ? metrics::kAddressPillHeight : kCompactHeight);
}

void AddressPill::Layout(PassKey) {
  const int padding =
      style_ == Style::kCompact ? kCompactPadding : kPillPadding;
  // 星号的符号在按钮正中，按钮比符号大一点：往外放出去，符号和边距对齐。
  const int inset = (kStarButton - kStarGlyph) / 2;
  star_->SetBounds(width() - padding - kStarButton + inset,
                   (height() - kStarButton) / 2, kStarButton, kStarButton);
  // 盾牌位于星号左侧。
  const gfx::Size shield = shield_->GetPreferredSize();
  shield_->SetBounds(star_->x() + inset - kShieldSpacing - shield.width(),
                     (height() - shield.height()) / 2, shield.width(),
                     shield.height());
}

void AddressPill::StateChanged(ButtonState old_state) {
  views::Button::StateChanged(old_state);
  UpdateStar(/*animate=*/false);
}

void AddressPill::PaintButtonContents(gfx::Canvas* canvas) {
  const float hover = static_cast<float>(hover_animation().GetCurrentValue());
  // 悬停淡入淡出的每一帧：星号、盾牌跟着。
  if (!bookmarked_) {
    star_->SetAlpha(hover);
  }
  if (!shield_->sticky() && !shield_popover_open_) {
    shield_->SetAlpha(hover);
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  if (style_ == Style::kPill) {
    flags.setColor(theme_.OnSurface(0.06f + 0.03f * hover));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kRow, flags);
  } else if (hover > 0) {
    flags.setColor(theme_.OnSurface(0.08f * hover));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kButton, flags);
  }
  const double swap = std::clamp(swap_.value(), 0.0, 1.0);
  if (swap < 1) {
    canvas->SaveLayerAlpha(static_cast<uint8_t>(255 * (1 - swap)));
    PaintSummary(canvas, previous_);
    canvas->Restore();
    canvas->SaveLayerAlpha(static_cast<uint8_t>(255 * swap));
    PaintSummary(canvas, summary_);
    canvas->Restore();
  } else {
    PaintSummary(canvas, summary_);
  }
}

void AddressPill::PaintSummary(gfx::Canvas* canvas,
                               const AddressSummary& summary) {
  const bool compact = style_ == Style::kCompact;
  const int padding = compact ? kCompactPadding : kPillPadding;
  const gfx::VectorIcon& glyph = summary.page_icon     ? *summary.page_icon
                                 : summary.placeholder ? icons::kMagnifyingGlass
                                 : summary.secure      ? icons::kLockFill
                                                       : icons::kInfoCircle;
  canvas->Save();
  canvas->Translate(gfx::Vector2d(padding + (kIconFrame - kIconSize) / 2,
                                  (height() - kIconSize) / 2));
  gfx::PaintVectorIcon(canvas, glyph, kIconSize, theme_.on_surface_variant);
  canvas->Restore();

  const int x =
      padding + kIconFrame + (compact ? kCompactSpacing : kPillSpacing);
  const int right = width() - padding - AccessoryWidth();
  const int available = std::max(0, right - x);
  const SkColor name_color =
      summary.placeholder ? theme_.on_surface_variant : theme_.on_surface;
  if (!compact) {
    canvas->DrawStringRectWithFlags(summary.name, UiFont(13), name_color,
                                    gfx::Rect(x, 0, available, height()),
                                    gfx::Canvas::NO_SUBPIXEL_RENDERING);
    return;
  }
  // 紧凑地址栏：网站（中等字重）后面跟着淡一些的「/
  // 标题」，地方不够时先截断标题。
  const gfx::FontList& name_font =
      UiFont(13, summary.placeholder ? gfx::Font::Weight::NORMAL
                                     : gfx::Font::Weight::MEDIUM);
  const gfx::FontList& title_font = UiFont(13);
  const int name_width =
      std::min(available, gfx::GetStringWidth(summary.name, name_font));
  canvas->DrawStringRectWithFlags(summary.name, name_font, name_color,
                                  gfx::Rect(x, 0, name_width, height()),
                                  gfx::Canvas::NO_SUBPIXEL_RENDERING);
  if (summary.title.empty()) {
    return;
  }
  const std::u16string slash = u"/";
  const int slash_x = x + name_width + kTitleGap;
  const int slash_width = gfx::GetStringWidth(slash, title_font);
  const int title_x = slash_x + slash_width + kTitleGap;
  if (title_x >= right) {
    return;
  }
  canvas->DrawStringRectWithFlags(
      slash, title_font,
      SkColorSetA(
          theme_.on_surface_variant,
          static_cast<U8CPU>(SkColorGetA(theme_.on_surface_variant) * 0.55f)),
      gfx::Rect(slash_x, 0, slash_width, height()),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
  canvas->DrawStringRectWithFlags(
      summary.title, title_font, theme_.on_surface_variant,
      gfx::Rect(title_x, 0, right - title_x, height()),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
}

bool AddressPill::OnMousePressed(const ui::MouseEvent& event) {
  const bool handled = views::Button::OnMousePressed(event);
  if (drag_window_on_drag_ && handled && event.IsOnlyLeftMouseButton() &&
      !event.IsShiftDown() && !event.IsControlDown() && !event.IsAltDown() &&
      !event.IsCommandDown()) {
    pending_window_drag_ = true;
    drag_press_location_ = event.location();
  } else {
    pending_window_drag_ = false;
  }
  return handled;
}

bool AddressPill::OnMouseDragged(const ui::MouseEvent& event) {
  if (pending_window_drag_ && !running_move_loop_ &&
      event.IsOnlyLeftMouseButton() &&
      ExceededDragThreshold(event.location() - drag_press_location_)) {
    pending_window_drag_ = false;
    views::Widget* widget = GetWidget();
    if (widget && widget->IsMoveLoopSupported() &&
        widget->IsMouseButtonDown()) {
      const gfx::Point press_screen =
          views::View::ConvertPointToScreen(this, drag_press_location_);
      const gfx::Vector2d drag_offset =
          press_screen - widget->GetWindowBoundsInScreen().origin();
      SetState(STATE_NORMAL);
      widget->ReleaseCapture();
      base::WeakPtr<AddressPill> self = weak_factory_.GetWeakPtr();
      running_move_loop_ = true;
      widget->RunMoveLoop(drag_offset, views::Widget::MoveLoopSource::kMouse,
                          views::Widget::MoveLoopEscapeBehavior::kDontHide);
      if (self) {
        self->running_move_loop_ = false;
      }
      return true;
    }
  }
  return views::Button::OnMouseDragged(event);
}

void AddressPill::OnMouseReleased(const ui::MouseEvent& event) {
  pending_window_drag_ = false;
  views::Button::OnMouseReleased(event);
}

void AddressPill::OnMouseCaptureLost() {
  pending_window_drag_ = false;
  views::Button::OnMouseCaptureLost();
}

std::u16string AddressPill::GetRenderedTooltipText(const gfx::Point& p) const {
  // 紧凑地址栏悬停提示显示完整网址。
  if (style_ == Style::kCompact && summary_.url.is_valid()) {
    return url_formatter::FormatUrl(summary_.url);
  }
  return L("toolbar.address.help");
}

void AddressPill::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  Refresh();
}

void AddressPill::OnTabChangedAt(tabs::TabInterface* tab,
                                 TabChangeType change_type) {
  if (tab == ShownTab()) {
    Refresh();
  }
}

void AddressPill::BookmarkModelChanged() {
  UpdateStar(/*animate=*/false);
}

BEGIN_METADATA(AddressPill)
END_METADATA

}  // namespace starry
