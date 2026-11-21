// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/command_bar/command_bar_view.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/case_conversion.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_provider_client.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_scheme_classifier.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/webui_url_constants.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/omnibox/browser/autocomplete_controller_config.h"
#include "components/omnibox/browser/autocomplete_enums.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/autocomplete_provider.h"
#include "components/omnibox/browser/autocomplete_result.h"
#include "components/tabs/public/tab_interface.h"
#include "components/url_formatter/url_formatter.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/new_tab/new_tab.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "starry/browser/tab_organizer/tab_grouper.h"
#include "starry/browser/tab_organizer/tab_organizer.h"
#include "starry/browser/ui/archive/organize_page.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/tabs/favorite_folders.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "third_party/metrics_proto/omnibox_event.pb.h"
#include "third_party/metrics_proto/omnibox_focus_type.pb.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/page_transition_types.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/range/range.h"
#include "ui/gfx/render_text.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace starry {

namespace {

// 面板：输入那一行高 56，左右 16；建议列表四周 6，行高 40、间距 2。
constexpr int kHeaderHeight = 56;
constexpr int kHeaderPadding = 16;
constexpr int kSearchIconSize = 16;
constexpr int kListPadding = 6;
constexpr int kRowSpacing = 2;
constexpr int kRowPadding = 12;
constexpr int kRowIconSize = 16;
constexpr int kFieldFontSize = 18;
// 窗口很窄时面板离窗口两边至少这么远。
constexpr int kSideMargin = 16;
// 打开时从上面 10 落下来、从 0.96 放大、从模糊 8 变清楚。
constexpr float kOpenOffset = -10;
constexpr float kOpenScale = 0.96f;
constexpr float kOpenBlur = 8;
constexpr int kFolderPathMaxWidth = 150;
constexpr int kFolderGlyphSize = 11;
constexpr size_t kMaxFolderMatches = 3;
// 归档的标签最多补几条。
constexpr size_t kMaxArchiveMatches = 3;
// 归档的结果多于 kMaxArchiveMatches 时最后一行「在已归档中查看全部」（不是归档里的 ID）。
constexpr char kArchiveAllId[] = "@all";
// 「整理标签页」（一键整理）那一行：打了「整理」这类词时排在最前面。
constexpr char kOrganizeId[] = "@organize";

// 归档的标签那一行右边的「3 天前归档」。
std::u16string ArchivedAgo(base::TimeDelta delta) {
  if (delta < base::Minutes(1)) {
    return L("commandBar.archived.now");
  }
  if (delta < base::Hours(1)) {
    return L("commandBar.archived.minutes",
             {{"count", static_cast<int>(delta.InMinutes())}});
  }
  if (delta < base::Days(1)) {
    return L("commandBar.archived.hours",
             {{"count", static_cast<int>(delta.InHours())}});
  }
  return L("commandBar.archived.days",
           {{"count", static_cast<int>(delta.InDays())}});
}
constexpr size_t kMaxRows = 10;
// shown_ 里大于等于它的是 extras_ 的下标。
constexpr size_t kExtraRow = 1u << 20;

SkColor WithAlpha(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(255 * opacity));
}

bool IsTabMatch(const AutocompleteMatch& match) {
  return match.has_tab_match.value_or(false) ||
         match.type == AutocompleteMatch::Type::kOpenTab;
}

bool IsBookmarkMatch(const AutocompleteMatch& match) {
  return match.type == AutocompleteMatch::Type::kBookmarkTitle ||
         (match.provider &&
          match.provider->type() == AutocompleteProvider::Type::kBookmark);
}

// 标识建议行的稳定键：搜索按查询词，其余按目标网址。结果变化时用于复用同一行并
// 平滑滑动到新位置。
std::string MatchKey(const AutocompleteMatch& match) {
  if (AutocompleteMatch::IsSearchType(match.type)) {
    return "search:" + base::UTF16ToUTF8(match.contents);
  }
  return "url:" + match.destination_url.spec();
}

// ⌘L 时输入框里的网址：空白页、新标签页不显示。
std::u16string EditText(content::WebContents* contents) {
  if (!contents) {
    return std::u16string();
  }
  const GURL& url = contents->GetVisibleURL();
  if (IsBlankTabUrl(url)) {
    return std::u16string();
  }
  return url_formatter::FormatUrl(url, url_formatter::kFormatUrlOmitDefaults,
                                  base::UnescapeRule::SPACES, nullptr,
                                  nullptr, nullptr);
}

// 带 Chrome 分类（匹配的字加粗、提示文字变淡）的一行文字。
std::unique_ptr<gfx::RenderText> MakeText(const std::u16string& text,
                                          const ACMatchClassifications& classes,
                                          const gfx::FontList& font,
                                          SkColor color,
                                          SkColor dim) {
  std::unique_ptr<gfx::RenderText> render = gfx::RenderText::CreateRenderText();
  render->SetFontList(font);
  render->SetText(text);
  render->SetColor(color);
  render->SetElideBehavior(gfx::ELIDE_TAIL);
  render->SetCursorEnabled(false);
  render->SetVerticalAlignment(gfx::ALIGN_MIDDLE);
  render->set_subpixel_rendering_suppressed(true);
  for (size_t i = 0; i < classes.size(); ++i) {
    const size_t start = classes[i].offset;
    const size_t end =
        std::min(i + 1 < classes.size() ? classes[i + 1].offset : text.size(),
                 text.size());
    if (start >= end) {
      continue;
    }
    const gfx::Range range(start, end);
    if (classes[i].style & ACMatchClassification::MATCH) {
      render->ApplyWeight(gfx::Font::Weight::SEMIBOLD, range);
    }
    if (classes[i].style & ACMatchClassification::DIM) {
      render->ApplyColor(dim, range);
    }
  }
  return render;
}

void PaintIcon(gfx::Canvas* canvas,
               const gfx::VectorIcon& icon,
               int size,
               SkColor color,
               const gfx::Point& origin) {
  canvas->Save();
  canvas->Translate(origin.OffsetFromOrigin());
  gfx::PaintVectorIcon(canvas, icon, size, color);
  canvas->Restore();
}

}  // namespace

// 面板上的东西：玻璃上那层主题色、细描边、放大镜和「新标签页」的小标。
// 输入框、分隔线和建议列表是它的子视图；入场模糊在外层玻璃上一起做。
class CommandBarPanel : public views::View {
  METADATA_HEADER(CommandBarPanel, views::View)

 public:
  CommandBarPanel() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  void Set(const Theme& theme, bool new_tab) {
    if (theme_ == theme && new_tab_ == new_tab) {
      return;
    }
    theme_ = theme;
    new_tab_ = new_tab;
    SchedulePaint();
  }

  // 「新标签页」小标占的宽度（没有时 0）。
  int BadgeWidth() const {
    return new_tab_ ? gfx::GetStringWidth(L("commandBar.newTabBadge"),
                                          UiFont(11, gfx::Font::Weight::SEMIBOLD)) +
                          16
                    : 0;
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    // 在玻璃上叠加一层半透明面板色，保证文字覆盖在网页上方时仍清晰可读。
    fill.setColor(WithAlpha(theme_.surface_panel, theme_.is_dark ? 0.55f : 0.65f));
    canvas->DrawRoundRect(bounds, radius::kLarge, fill);

    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius::kLarge - 0.5f, border);

    PaintIcon(canvas, icons::kMagnifyingGlass, kSearchIconSize,
              theme_.on_surface_variant,
              gfx::Point(kHeaderPadding, (kHeaderHeight - kSearchIconSize) / 2));

    if (new_tab_) {
      const int width = BadgeWidth();
      const gfx::Rect badge(this->width() - kHeaderPadding - width,
                            (kHeaderHeight - 20) / 2, width, 20);
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(0.07f));
      canvas->DrawRoundRect(gfx::RectF(badge), badge.height() / 2.0f, flags);
      canvas->DrawStringRectWithFlags(
          L("commandBar.newTabBadge"), UiFont(11, gfx::Font::Weight::SEMIBOLD),
          theme_.on_surface_variant, badge,
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }
  }

 private:
  Theme theme_ = Theme::DarkBase();
  bool new_tab_ = false;
};

BEGIN_METADATA(CommandBarPanel)
END_METADATA

// 单条建议行：图标、标题（13）和地址（11），匹配文字加粗；
// 右侧依次可显示「切换到标签页」、书签星标、悬停删除 ×、选中时的 ↩。
class CommandBarRow : public views::View {
  METADATA_HEADER(CommandBarRow, views::View)

 public:
  explicit CommandBarRow(CommandBarView* owner) : owner_(owner) {
    // 自己一个图层：出现、去掉时淡入淡出，挪位置时滑过去。
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  LayerMotion& motion() { return motion_; }
  const std::string& key() const { return key_; }
  void set_key(std::string key) { key_ = std::move(key); }
  base::WeakPtr<CommandBarRow> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // archived：归档的标签（选中就恢复），右边写它什么时候归档的。restore 是 false 时（「在
  // 已归档中查看全部」）胶囊里写的就是 archived，前面不再写一句。
  void Set(const AutocompleteMatch& match,
           const std::u16string& folder_path,
           const Theme& theme,
           bool selected,
           bool hovered,
           const std::u16string& archived = std::u16string(),
           bool restore = true,
           bool action = false) {
    theme_ = theme;
    folder_path_ = folder_path;
    archived_ = archived;
    restore_ = restore;
    action_ = action;
    selected_ = selected;
    hovered_ = hovered;
    search_ = AutocompleteMatch::IsSearchType(match.type);
    history_search_ = match.type == AutocompleteMatch::Type::kSearchHistory;
    tab_ = IsTabMatch(match);
    bookmark_ = IsBookmarkMatch(match);
    removable_ = match.SupportsDeletion();
    if (search_ || match.description.empty()) {
      title_ = match.contents;
      title_class_ = match.contents_class;
      subtitle_ = search_ ? std::u16string() : std::u16string();
      subtitle_class_.clear();
    } else {
      title_ = match.description;
      title_class_ = match.description_class;
      subtitle_ = match.contents;
      subtitle_class_ = match.contents_class;
    }
    GetViewAccessibility().SetName(title_);
    if (folder_path_.empty()) {
      GetViewAccessibility().RemoveDescription();
    } else {
      GetViewAccessibility().SetDescription(folder_path_);
    }
    SchedulePaint();
  }

  const std::u16string& title() const { return title_; }
  const std::u16string& folder_path() const { return folder_path_; }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(0, metrics::kCommandBarRowHeight);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const bool dark = theme_.is_dark;
    SkColor background = SK_ColorTRANSPARENT;
    if (selected_) {
      background = WithAlpha(theme_.accent, dark ? 0.16f : 0.12f);
    } else if (hovered_) {
      background = theme_.OnSurface(0.05f);
    }
    if (SkColorGetA(background) > 0) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(background);
      canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kMenu, flags);
    }

    const SkColor icon_color =
        selected_ ? theme_.accent : theme_.on_surface_variant;
    const gfx::VectorIcon* icon = &icons::kGlobe;
    if (search_) {
      icon = history_search_ ? &icons::kClock : &icons::kMagnifyingGlass;
    } else if (tab_) {
      icon = &icons::kSidebarLeft;
    } else if (action_) {
      icon = &icons::kSparkle;
    } else if (!archived_.empty()) {
      icon = &icons::kArchivebox;
    }
    PaintIcon(canvas, *icon, kRowIconSize, icon_color,
              gfx::Point(kRowPadding + 1, (height() - kRowIconSize) / 2));

    // 右边的小东西，从右往左排。
    int right = width() - kRowPadding;
    if (selected_) {
      const int size = 12;
      right -= size;
      PaintReturnGlyph(canvas, gfx::Rect(right, (height() - size) / 2, size,
                                          size));
      right -= 8;
    }
    remove_bounds_ = gfx::Rect();
    if (removable_ && hovered_ && !tab_) {
      const int size = 20;
      right -= size;
      remove_bounds_ = gfx::Rect(right, (height() - size) / 2, size, size);
      PaintIcon(canvas, icons::kXmarkBold, 10, theme_.on_surface_variant,
                remove_bounds_.CenterPoint() - gfx::Vector2d(5, 5));
      right -= 8;
    }
    if (tab_) {
      const gfx::FontList& font = UiFont(11, gfx::Font::Weight::SEMIBOLD);
      const std::u16string& label = L("commandBar.switchToTab");
      const int width = gfx::GetStringWidth(label, font) + 16;
      right -= width;
      const gfx::Rect pill(right, (height() - 20) / 2, width, 20);
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(selected_ ? WithAlpha(theme_.accent, 0.14f)
                               : theme_.OnSurface(0.07f));
      canvas->DrawRoundRect(gfx::RectF(pill), 10, flags);
      canvas->DrawStringRectWithFlags(
          label, font, selected_ ? theme_.accent : theme_.on_surface_variant,
          pill,
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
      right -= 8;
      if (!folder_path_.empty()) {
        const gfx::FontList& path_font = UiFont(11);
        const int text = std::min(gfx::GetStringWidth(folder_path_, path_font),
                                  kFolderPathMaxWidth);
        right -= kFolderGlyphSize + 4 + text;
        PaintIcon(canvas, icons::kFolder, kFolderGlyphSize,
                  theme_.on_surface_variant,
                  gfx::Point(right, (height() - kFolderGlyphSize) / 2));
        canvas->DrawStringRectWithFlags(
            folder_path_, path_font, theme_.on_surface_variant,
            gfx::Rect(right + kFolderGlyphSize + 4, 0, text, height()),
            gfx::Canvas::NO_SUBPIXEL_RENDERING);
        right -= 8;
      }
    } else if (!archived_.empty()) {
      // 「恢复」的胶囊（同「切换到标签页」），前面一句什么时候归档的。
      const gfx::FontList& font = UiFont(11, gfx::Font::Weight::SEMIBOLD);
      const std::u16string label =
          restore_ ? L("commandBar.restoreArchived") : archived_;
      const int width = gfx::GetStringWidth(label, font) + 16;
      right -= width;
      const gfx::Rect pill(right, (height() - 20) / 2, width, 20);
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(selected_ ? WithAlpha(theme_.accent, 0.14f)
                               : theme_.OnSurface(0.07f));
      canvas->DrawRoundRect(gfx::RectF(pill), 10, flags);
      canvas->DrawStringRectWithFlags(
          label, font, selected_ ? theme_.accent : theme_.on_surface_variant,
          pill,
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
      right -= 8;
      if (restore_) {
        const gfx::FontList& note_font = UiFont(11);
        const int text = std::min(gfx::GetStringWidth(archived_, note_font),
                                  kFolderPathMaxWidth);
        right -= text;
        canvas->DrawStringRectWithFlags(archived_, note_font,
                                        theme_.on_surface_variant,
                                        gfx::Rect(right, 0, text, height()),
                                        gfx::Canvas::NO_SUBPIXEL_RENDERING);
        right -= 8;
      }
    } else if (bookmark_) {
      right -= 10;
      PaintIcon(canvas, icons::kStarFill, 10, theme_.on_surface_variant,
                gfx::Point(right, (height() - 10) / 2));
      right -= 8;
    }

    const int x = kRowPadding + 18 + 12;
    const int text_width = std::max(0, right - x);
    const SkColor dim = theme_.on_surface_variant;
    if (subtitle_.empty()) {
      auto title = MakeText(title_, title_class_, UiFont(13),
                            theme_.on_surface, dim);
      title->SetDisplayRect(gfx::Rect(x, 0, text_width, height()));
      title->Draw(canvas);
      return;
    }
    auto title =
        MakeText(title_, title_class_, UiFont(13), theme_.on_surface, dim);
    title->SetDisplayRect(gfx::Rect(x, 3, text_width, 19));
    title->Draw(canvas);
    auto subtitle = MakeText(subtitle_, subtitle_class_, UiFont(11),
                             theme_.on_surface_variant, dim);
    subtitle->SetDisplayRect(gfx::Rect(x, 21, text_width, 15));
    subtitle->Draw(canvas);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    return event.IsOnlyLeftMouseButton();
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (!HitTestPoint(event.location())) {
      return;
    }
    if (!remove_bounds_.IsEmpty() && remove_bounds_.Contains(event.location())) {
      owner_->RowRemoveClicked(this);
      return;
    }
    owner_->RowClicked(this);
  }

  void OnMouseEntered(const ui::MouseEvent& event) override {
    owner_->RowHovered(this, true);
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    owner_->RowHovered(this, false);
  }

 private:
  // ↩：竖线下来再往左拐，带箭头。
  void PaintReturnGlyph(gfx::Canvas* canvas, const gfx::Rect& rect) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1.5f);
    flags.setStrokeCap(cc::PaintFlags::kRound_Cap);
    flags.setColor(theme_.on_surface_variant);
    const float l = rect.x() + 1.5f;
    const float r = rect.right() - 1.5f;
    const float t = rect.y() + 2;
    const float m = rect.y() + rect.height() * 0.62f;
    canvas->DrawLine(gfx::PointF(r, t), gfx::PointF(r, m), flags);
    canvas->DrawLine(gfx::PointF(r, m), gfx::PointF(l, m), flags);
    canvas->DrawLine(gfx::PointF(l, m), gfx::PointF(l + 3, m - 3), flags);
    canvas->DrawLine(gfx::PointF(l, m), gfx::PointF(l + 3, m + 3), flags);
  }

  const raw_ptr<CommandBarView> owner_;
  LayerMotion motion_;
  std::string key_;
  Theme theme_ = Theme::DarkBase();
  bool selected_ = false;
  bool hovered_ = false;
  bool search_ = false;
  bool history_search_ = false;
  bool tab_ = false;
  bool bookmark_ = false;
  bool removable_ = false;
  std::u16string title_;
  ACMatchClassifications title_class_;
  std::u16string subtitle_;
  ACMatchClassifications subtitle_class_;
  std::u16string folder_path_;
  std::u16string archived_;
  bool restore_ = true;
  // 动作的一行（「整理标签页」）：星光图标，右边的胶囊写做什么。
  bool action_ = false;
  gfx::Rect remove_bounds_;
  base::WeakPtrFactory<CommandBarRow> weak_factory_{this};
};

BEGIN_METADATA(CommandBarRow)
END_METADATA

CommandBarView::CommandBarView(BrowserWindowInterface* browser)
    : browser_(browser),
      top_offset_(metrics::kCommandBarTopOffset),
      height_(kHeaderHeight,
              base::BindRepeating(&CommandBarView::LayoutPanel,
                                  base::Unretained(this))) {
  // 面板外由本视图接收点击。自己的图层让 Aura 在网页上方命中这块遮罩。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetVisible(false);

  backdrop_ = AddChildView(std::make_unique<views::View>());
  backdrop_->SetPaintToLayer();
  backdrop_->layer()->SetFillsBoundsOpaquely(false);
  backdrop_->layer()->SetOpacity(0);
  backdrop_->SetCanProcessEventsWithinSubtree(false);

  home_ = AddChildView(std::make_unique<CommandBarHome>(browser, this));
  home_->SetVisible(false);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));

  glass_ = AddChildView(std::make_unique<GlassView>());
  glass_->SetCornerRadius(radius::kLarge, true);
  panel_ = glass_->AddChildView(std::make_unique<CommandBarPanel>());

  field_ = panel_->AddChildView(std::make_unique<views::Textfield>());
  field_->SetController(this);
  field_->SetBackgroundEnabled(false);
  field_->SetBorder(nullptr);
  field_->SetFontList(UiFont(kFieldFontSize));
  field_->SetPlaceholderText(L("commandBar.placeholder"));
  field_->GetViewAccessibility().SetName(L("sidebar.addressBar"));

  list_ = panel_->AddChildView(std::make_unique<views::View>());
  list_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  list_->layer()->SetMasksToBounds(true);
  list_->layer()->SetRoundedCornerRadius(
      gfx::RoundedCornersF(0, 0, radius::kLarge, radius::kLarge));

  divider_ = panel_->AddChildView(std::make_unique<views::View>());
  divider_->SetPaintToLayer();
  divider_->layer()->SetFillsBoundsOpaquely(false);
  divider_->layer()->SetOpacity(0);
  divider_->SetCanProcessEventsWithinSubtree(false);

  Profile* profile = browser_->GetProfile();
  AutocompleteControllerConfig config;
  config.provider_types = AutocompleteClassifier::DefaultOmniboxProviders() |
                          static_cast<int>(AutocompleteProvider::Type::kOpenTab);
  config.unscoped_open_tab_suggestions = true;
  controller_ = std::make_unique<AutocompleteController>(
      std::make_unique<ChromeAutocompleteProviderClient>(profile), config);
  controller_->AddObserver(this);
  ApplyTheme();
}

CommandBarView::~CommandBarView() {
  StopFocusListening();
  controller_->RemoveObserver(this);
}

void CommandBarView::Open(Mode mode) {
  OpenImpl(mode, DockSlot(), std::u16string(), /*drop=*/true,
           base::OnceClosure());
}

void CommandBarView::OpenDocked(DockSlot slot,
                                const std::u16string& text,
                                bool drop,
                                base::OnceClosure closed) {
  OpenImpl(Mode::kEdit, std::move(slot), text, drop, std::move(closed));
}

void CommandBarView::DockMoved() {
  if (!dock_slot_.is_null() && GetVisible()) {
    LayoutPanel();
  }
}

void CommandBarView::FocusField(const std::u16string& text) {
  if (!open_) {
    return;
  }
  if (!text.empty()) {
    field_->SetText(text);
    field_->SetSelectedRange(gfx::Range(text.size()));
    ContentsChanged(field_, text);
  }
  field_->RequestFocus();
}

void CommandBarView::OpenImpl(Mode mode,
                              DockSlot slot,
                              const std::u16string& initial_text,
                              bool drop,
                              base::OnceClosure closed) {
  const bool was_open = open_;
  const bool docked = !slot.is_null();
  const bool was_docked = !dock_slot_.is_null();
  if (was_open && docked && was_docked) {
    if (dock_closed_) {
      std::move(dock_closed_).Run();
    }
    dock_slot_ = std::move(slot);
    dock_closed_ = std::move(closed);
    LayoutPanel();
    if (!initial_text.empty()) {
      FocusField(initial_text);
    } else {
      field_->SelectAll(false);
      field_->RequestFocus();
    }
    return;
  }
  if (was_open && !docked && !was_docked && mode_ == mode) {
    field_->SelectAll(false);
    field_->RequestFocus();
    return;
  }
  if (was_open && was_docked && dock_closed_) {
    std::move(dock_closed_).Run();
  }
  if (!docked) {
    StopFocusListening();
  }
  // 关掉后还在淡出时又打开。
  const bool closing = !was_open && GetVisible();
  mode_ = mode;
  open_ = true;
  ++close_generation_;
  dock_slot_ = std::move(slot);
  dock_closed_ = std::move(closed);
  home_enabled_ =
      !docked && mode == Mode::kNewTab &&
      new_tab::GetStyle(browser_->GetProfile()) == new_tab::Style::kOverlay;

  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  if (docked) {
    text_ = initial_text;
  } else {
    text_ = mode == Mode::kEdit && tab ? EditText(tab->GetContents())
                                       : std::u16string();
  }
  edited_ = docked && !text_.empty();
  prevent_inline_ = false;
  selected_ = 0;
  hovered_.reset();
  controller_->Stop(AutocompleteStopReason::kClobbered);
  // 还看得见的旧建议收起来；从关着打开时直接是只有输入那一行的面板。
  RebuildRows(/*allow_animation=*/was_open || closing);

  if (!was_open) {
    views::FocusManager* focus_manager = GetFocusManager();
    previous_focus_.SetView(focus_manager ? focus_manager->GetFocusedView()
                                          : nullptr);
  }
  SetVisible(true);
  ApplyTheme();
  InvalidateLayout();
  DeprecatedLayoutImmediately();

  syncing_ = true;
  field_->SetText(text_);
  if (edited_) {
    // 在页面上打的字：光标放在后面，接着打。
    field_->SetSelectedRange(gfx::Range(text_.size()));
  } else {
    field_->SelectAll(false);
  }
  synced_text_ = text_;
  synced_selection_ = field_->GetSelectedRange();
  syncing_ = false;
  field_->RequestFocus();
  if (docked) {
    // 点到网页上（焦点去了网页）就关。
    if (views::FocusManager* focus_manager = GetFocusManager();
        focus_manager && !focus_listening_) {
      focus_manager->AddFocusChangeListener(this);
      focus_listening_ = focus_manager;
    }
  }
  if (edited_) {
    StartQuery();
  }

  home_->SetVisible(home_enabled_);
  if (home_enabled_) {
    home_->SetBoundsRect(GetLocalBounds());
    home_->Refresh(/*reveal=*/!was_open && !closing);
  }

  // 遮罩淡入；面板从上方偏移落下、缩放、去模糊并淡入。停靠时没有遮罩，点了页面上的
  // 搜索框时面板原地淡入。
  const MotionCurve curve = MotionCurve::Of(motion::kCommandBarOpen);
  if (docked) {
    backdrop_motion_.SetOpacity(backdrop_->layer(), 0);
  } else {
    backdrop_motion_.AnimateOpacity(backdrop_->layer(), 1, curve);
  }
  if (docked && !drop && !closing) {
    glass_motion_.SetTransform(glass_->layer(), LayerPose());
    glass_motion_.AnimateOpacityFrom(glass_->layer(), 0, 1,
                                     MotionCurve::EaseOut(0.12));
  } else if (closing) {
    // 还在淡出：从现在的样子淡回来，不再落一次。
    glass_motion_.AnimateOpacity(glass_->layer(), 1, curve);
  } else if (!was_open) {
    LayerPose from;
    if (!motion::ReduceMotion()) {
      from.offset = gfx::Vector2dF(0, kOpenOffset);
      from.scale = kOpenScale;
    }
    const gfx::Rect rect = glass_->glass_rect();
    // 缩放锚点位于窗口顶边居中（包含顶部留白），而非玻璃自身顶边。
    glass_motion_.AnimateTransformFrom(
        glass_->layer(), from, LayerPose(), curve,
        gfx::PointF(rect.CenterPoint().x(), -glass_->y()), base::OnceClosure(),
        kOpenBlur);
    glass_motion_.AnimateOpacityFrom(glass_->layer(), 0, 1, curve);
    // 整块玻璃、描边与内容随变换动画一起去模糊，共用合成器起始时间。
  }
}

void CommandBarView::Close() {
  if (!open_) {
    return;
  }
  open_ = false;
  StopFocusListening();
  if (dock_closed_) {
    std::move(dock_closed_).Run();
  }
  if (home_enabled_) {
    home_->SetCanProcessEventsWithinSubtree(false);
  }
  controller_->Stop(AutocompleteStopReason::kInteraction);
  const int generation = ++close_generation_;

  // 焦点还给原来的视图，没有就给网页。
  if (previous_focus_.view()) {
    previous_focus_.view()->RequestFocus();
  } else if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
    tab->GetContents()->Focus();
  }
  previous_focus_.SetView(nullptr);

  // 关闭时整个命令栏原地淡出，面板不做位移动画。
  const MotionCurve curve = MotionCurve::Of(motion::kCommandBarClose);
  backdrop_motion_.AnimateOpacity(backdrop_->layer(), 0, curve);
  glass_motion_.AnimateOpacity(
      glass_->layer(), 0, curve,
      base::BindOnce(
          [](base::WeakPtr<CommandBarView> view, int generation) {
            if (view && view->close_generation_ == generation) {
              view->FinishClose();
            }
          },
          weak_factory_.GetWeakPtr(), generation));
}

void CommandBarView::FinishClose() {
  if (open_) {
    return;
  }
  dock_slot_.Reset();
  home_enabled_ = false;
  home_->SetVisible(false);
  home_->SetCanProcessEventsWithinSubtree(true);
  SetVisible(false);
  controller_->Stop(AutocompleteStopReason::kClobbered);
  RebuildRows(/*allow_animation=*/false);
  glass_->layer()->SetLayerBlur(0);
}

void CommandBarView::SetTopOffset(int top_offset) {
  top_offset_ = top_offset;
  InvalidateLayout();
}

void CommandBarView::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  ApplyTheme();
}

void CommandBarView::ApplyTheme() {
  if (home_enabled_) {
    // 全屏浮层：整个窗口模糊，再压暗（深色 52% 的黑、浅色 50% 的白），首页的字直接写在上面。
    backdrop_->SetBackground(views::CreateSolidBackground(
        theme_.is_dark ? SkColorSetA(SK_ColorBLACK, 0x85)
                       : SkColorSetA(SK_ColorWHITE, 0x80)));
    backdrop_->layer()->SetBackgroundBlur(28);
  } else {
    // 遮罩：深色 28%、浅色 12% 的黑。
    backdrop_->SetBackground(views::CreateSolidBackground(
        SkColorSetA(SK_ColorBLACK, theme_.is_dark ? 0x47 : 0x1F)));
    backdrop_->layer()->SetBackgroundBlur(0);
  }
  home_->SetTheme(theme_);
  GlassStyle style;
  style.dark = theme_.is_dark;
  style.luma = theme_.is_dark ? 0.1f : 0.9f;
  glass_->SetStyle(style);
  field_->SetColor(theme_.on_surface);
  divider_->SetBackground(
      views::CreateSolidBackground(theme_.OnSurface(0.08f)));
  RefreshRows();
}

void CommandBarView::TypeForTesting(const std::u16string& text) {
  field_->SetText(text);
  ContentsChanged(field_, text);
}

void CommandBarView::PressKeyForTesting(ui::KeyboardCode key, int flags) {
  ui::KeyEvent event(ui::EventType::kKeyPressed, key, flags);
  HandleKeyEvent(field_, event);
}

std::u16string CommandBarView::FieldTextForTesting() const {
  return std::u16string(field_->GetText());
}

std::vector<std::u16string> CommandBarView::RowTitlesForTesting() const {
  std::vector<std::u16string> titles;
  for (CommandBarRow* row : rows_) {
    titles.push_back(row->folder_path().empty()
                         ? row->title()
                         : row->title() + u" [" + row->folder_path() + u"]");
  }
  return titles;
}

void CommandBarView::RowHovered(CommandBarRow* row, bool hovered) {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i] == row) {
      if (hovered) {
        hovered_ = i;
      } else if (hovered_ == i) {
        hovered_.reset();
      }
    }
  }
  RefreshRows();
}

void CommandBarView::RowClicked(CommandBarRow* row) {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i] != row) {
      continue;
    }
    OpenRow(i);
    return;
  }
}

void CommandBarView::RowRemoveClicked(CommandBarRow* row) {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i] == row) {
      selected_ = i;
      RemoveSelected();
      return;
    }
  }
}

gfx::Rect CommandBarView::PanelBounds() const {
  const int height = static_cast<int>(std::round(height_.value()));
  if (!dock_slot_.is_null()) {
    if (std::optional<gfx::Rect> slot = dock_slot_.Run()) {
      dock_rect_ = *slot;
    }
    return gfx::Rect(dock_rect_.x(), dock_rect_.y(), dock_rect_.width(),
                     std::max(kHeaderHeight, height));
  }
  const int width = std::min(metrics::kCommandBarWidth,
                             std::max(0, this->width() - 2 * kSideMargin));
  return gfx::Rect((this->width() - width) / 2, top_offset_, width,
                   std::max(kHeaderHeight, height));
}

int CommandBarView::TargetPanelHeight() const {
  if (rows_.empty()) {
    return kHeaderHeight;
  }
  const int count = static_cast<int>(rows_.size());
  return kHeaderHeight + 1 + 2 * kListPadding +
         count * metrics::kCommandBarRowHeight + (count - 1) * kRowSpacing;
}

void CommandBarView::Layout(PassKey) {
  backdrop_->SetBoundsRect(GetLocalBounds());
  home_->SetBoundsRect(GetLocalBounds());
  home_->SetPanelBottom(top_offset_ + kHeaderHeight);
  LayoutPanel();
  PlaceRows(/*animate=*/false);
}

void CommandBarView::LayoutPanel() {
  const gfx::Rect panel = PanelBounds();
  glass_->SetGlassBounds(panel);
  panel_->SetBoundsRect(glass_->glass_rect());

  const int field_x = kHeaderPadding + kSearchIconSize + 12;
  const int badge = panel_->BadgeWidth();
  const int field_right =
      panel.width() - kHeaderPadding - (badge ? badge + 12 : 0);
  field_->SetBounds(field_x, (kHeaderHeight - 30) / 2,
                    std::max(0, field_right - field_x), 30);

  divider_->SetBounds(0, kHeaderHeight, panel.width(), 1);
  // 列表是玻璃现在的高度，行摆在最后的位置上：面板变高时下面的行一点点露出来。
  list_->SetBounds(0, kHeaderHeight + 1, panel.width(),
                   std::max(0, panel.height() - kHeaderHeight - 1));
}

void CommandBarView::PlaceRows(bool animate) {
  const int width = std::max(0, list_->width() - 2 * kListPadding);
  int y = kListPadding;
  for (CommandBarRow* row : rows_) {
    const gfx::Rect old = row->bounds();
    const gfx::Rect target(kListPadding, y, width,
                           metrics::kCommandBarRowHeight);
    row->SetBoundsRect(target);
    // 同一条建议换了位置：从原来的地方滑过去（新建的行没有原来的位置，在原地淡入）。
    if (animate && !old.IsEmpty() && old.origin() != target.origin()) {
      row->motion().ShiftAndAnimate(
          row->layer(), gfx::Vector2dF(old.origin() - target.origin()),
          LayerPose(), MotionCurve::Of(motion::kSearchResize));
    }
    y += metrics::kCommandBarRowHeight + kRowSpacing;
  }
}

bool CommandBarView::DoesIntersectRect(const views::View* target,
                                       const gfx::Rect& rect) const {
  if (target != this) {
    return views::ViewTargeterDelegate::DoesIntersectRect(target, rect);
  }
  if (dock_slot_.is_null()) {
    // 遮罩盖住整个窗口（点外面关掉）。
    return true;
  }
  // 停着时只有面板接鼠标，别处的点击给网页和侧栏。
  gfx::Rect panel = glass_->glass_rect();
  panel.Offset(glass_->bounds().OffsetFromOrigin());
  return panel.Intersects(rect);
}

void CommandBarView::OnDidChangeFocus(views::View* focused_before,
                                      views::View* focused_now) {
  if (!docked_open() || !focused_now || Contains(focused_now)) {
    return;
  }
  // 点了网页（或者别的界面）：焦点不还回去。
  previous_focus_.SetView(nullptr);
  Close();
}

void CommandBarView::StopFocusListening() {
  if (focus_listening_) {
    focus_listening_->RemoveFocusChangeListener(this);
    focus_listening_ = nullptr;
  }
}

void CommandBarView::HomeOpenUrl(const GURL& url) {
  previous_focus_.SetView(nullptr);
  Close();
  NavigateParams params(browser_, url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  params.disposition = WindowOpenDisposition::kNewForegroundTab;
  Navigate(&params);
  if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
    tab->GetContents()->Focus();
  }
}

void CommandBarView::HomeDismiss() {
  previous_focus_.SetView(nullptr);
  Close();
}

void CommandBarView::UpdateHome() {
  if (home_enabled_ && open_) {
    home_->SetShown(text_.empty());
  }
}

bool CommandBarView::OnMousePressed(const ui::MouseEvent& event) {
  pressed_backdrop_ = !glass_->bounds().Contains(event.location()) ||
                      !glass_->glass_rect().Contains(
                          event.location() - glass_->bounds().OffsetFromOrigin());
  return true;
}

void CommandBarView::OnMouseReleased(const ui::MouseEvent& event) {
  if (pressed_backdrop_) {
    pressed_backdrop_ = false;
    Close();
  }
}

void CommandBarView::ContentsChanged(views::Textfield* sender,
                                     const std::u16string& new_contents) {
  if (syncing_) {
    return;
  }
  const bool caret_at_end =
      sender->GetSelectedRange() == gfx::Range(new_contents.size());
  prevent_inline_ =
      prevent_inline_ || !caret_at_end || sender->IsIMEComposing();
  text_ = new_contents;
  synced_text_ = new_contents;
  synced_selection_ = sender->GetSelectedRange();
  edited_ = true;
  selected_ = 0;
  StartQuery();
  // 删字的标记只管这一次。
  prevent_inline_ = false;
}

bool CommandBarView::HandleKeyEvent(views::Textfield* sender,
                                    const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  switch (key_event.key_code()) {
    case ui::VKEY_UP:
      Move(-1);
      return true;
    case ui::VKEY_DOWN:
      Move(1);
      return true;
    case ui::VKEY_TAB:
      Move(key_event.IsShiftDown() ? -1 : 1);
      return true;
    case ui::VKEY_RETURN:
      Submit();
      return true;
    case ui::VKEY_ESCAPE:
      // 选着别的建议时先回到默认建议，再按一次才关。
      if (selected_ != 0) {
        selected_ = 0;
        RefreshRows();
        SyncField();
      } else {
        Close();
      }
      return true;
    case ui::VKEY_DELETE:
      if (key_event.IsShiftDown() && RemoveSelected()) {
        return true;
      }
      prevent_inline_ = true;
      return false;
    case ui::VKEY_BACK:
      // ⌫ 去掉内联补全时不能马上又补回来。
      prevent_inline_ = true;
      return false;
    case ui::VKEY_RIGHT:
    case ui::VKEY_END:
      // 显示着补全或别的建议时移动光标：显示的文字整个算作输入的。
      if (std::u16string(sender->GetText()) != text_) {
        text_ = std::u16string(sender->GetText());
        edited_ = true;
        selected_ = 0;
        prevent_inline_ = true;
        StartQuery();
        prevent_inline_ = false;
      }
      return false;
    default:
      return false;
  }
}

void CommandBarView::OnResultChanged(AutocompleteController* controller,
                                     bool default_match_changed) {
  if (!open_) {
    return;
  }
  RebuildRows(/*allow_animation=*/true);
  SyncField();
}

void CommandBarView::StartQuery() {
  UpdateHome();
  if (text_.empty()) {
    controller_->Stop(AutocompleteStopReason::kClobbered);
    RebuildRows(/*allow_animation=*/true);
    SyncField();
    return;
  }
  Profile* profile = browser_->GetProfile();
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  AutocompleteInput input(
      text_, std::min(field_->GetCursorPosition(), text_.size()),
      mode_ == Mode::kNewTab ? ::metrics::OmniboxEventProto::
                                   INSTANT_NTP_WITH_OMNIBOX_AS_STARTING_FOCUS
                             : ::metrics::OmniboxEventProto::OTHER,
      ChromeAutocompleteSchemeClassifier(profile));
  if (mode_ == Mode::kEdit && tab) {
    input.set_current_url(tab->GetContents()->GetVisibleURL());
    input.set_current_title(tab->GetContents()->GetTitle());
  }
  input.set_prevent_inline_autocomplete(prevent_inline_);
  // 同 Chrome 地址栏不在关键词模式时（命令栏没有关键词模式）：不然打了搜索引擎域名
  // 的开头（g、b……）会多出「<输入搜索字词>」这样的站内搜索占位。
  input.set_allow_exact_keyword_match(false);
  input.set_focus_type(::metrics::OmniboxFocusType::INTERACTION_DEFAULT);
  controller_->Start(input);
}

void CommandBarView::RebuildRows(bool allow_animation) {
  const AutocompleteResult& result = controller_->result();
  const bool current = open_ && controller_->input().text() == text_ &&
                       !text_.empty();
  shown_.clear();
  extras_.clear();
  extra_archive_ids_.clear();
  std::vector<std::string> keys;
  for (size_t i = 0; current && i < result.size(); ++i) {
    if (result.match_at(i).destination_url.is_valid()) {
      shown_.push_back(i);
      keys.push_back(MatchKey(result.match_at(i)));
    }
  }
  // 文件夹的名字也搜得到里面的固定标签：排在默认的那条后面，后面的搜索建议
  // 让出位置。
  if (current) {
    extras_ = FolderMatches(keys);
    extra_archive_ids_.assign(extras_.size(), std::string());
    // 归档的标签：接在文件夹的后面。
    std::vector<std::string> archive_ids;
    for (AutocompleteMatch& match : ArchiveMatches(keys, &archive_ids)) {
      extras_.push_back(std::move(match));
    }
    extra_archive_ids_.insert(extra_archive_ids_.end(), archive_ids.begin(),
                              archive_ids.end());
    // 「整理标签页」：打了「整理」这类词时排在最前面（默认的那条后面）。
    if (std::optional<AutocompleteMatch> organize = OrganizeMatch()) {
      extras_.insert(extras_.begin(), std::move(*organize));
      extra_archive_ids_.insert(extra_archive_ids_.begin(), kOrganizeId);
    }
    // 归档里有的网页不再单独列一行历史记录（恢复它还带着前进后退）。默认的那一条
    // （第 0 行，回车打开的）留着。
    std::set<GURL> archived_urls;
    for (size_t i = 0; i < extras_.size(); ++i) {
      if (!extra_archive_ids_[i].empty()) {
        archived_urls.insert(extras_[i].destination_url);
      }
    }
    for (size_t i = shown_.size(); i-- > 1;) {
      if (archived_urls.contains(
              result.match_at(shown_[i]).destination_url)) {
        shown_.erase(shown_.begin() + i);
        keys.erase(keys.begin() + i);
      }
    }
    const size_t at = std::min<size_t>(1, shown_.size());
    for (size_t i = 0; i < extras_.size(); ++i) {
      shown_.insert(shown_.begin() + at + i, kExtraRow + i);
      keys.insert(keys.begin() + at + i,
                  extra_archive_ids_[i].empty()
                      ? MatchKey(extras_[i])
                      : "archive:" + extra_archive_ids_[i]);
    }
    if (shown_.size() > kMaxRows) {
      shown_.resize(kMaxRows);
      keys.resize(kMaxRows);
    }
  }
  const size_t count = shown_.size();
  // 仅当建议行集合（按 MatchKey 标识）或顺序变化时播放过渡动画；仅高亮加粗变化时
  // 直接更新内容。
  bool changed = count != rows_.size();
  for (size_t i = 0; !changed && i < count; ++i) {
    changed = rows_[i]->key() != keys[i];
  }
  const bool animate = allow_animation && open_ && GetVisible() && changed &&
                       !motion::ReduceMotion();
  if (!open_ || !allow_animation) {
    DropLeavingRows();
  }
  const MotionCurve curve = MotionCurve::Of(motion::kSearchResize);
  auto take = [](std::vector<raw_ptr<CommandBarRow, VectorExperimental>>& rows,
                 const std::string& key) -> CommandBarRow* {
    auto it = std::ranges::find_if(
        rows, [&](CommandBarRow* row) { return row->key() == key; });
    if (it == rows.end()) {
      return nullptr;
    }
    CommandBarRow* row = *it;
    rows.erase(it);
    return row;
  };
  std::vector<raw_ptr<CommandBarRow, VectorExperimental>> old =
      std::move(rows_);
  rows_.clear();
  for (size_t i = 0; i < count; ++i) {
    std::string& key = keys[i];
    CommandBarRow* row = take(old, key);
    if (!row && (row = take(leaving_, key))) {
      // 正在淡出的又回来了（删了字又打回去）：从现在的样子淡回来。
      row->SetCanProcessEventsWithinSubtree(true);
      row->motion().AnimateOpacity(row->layer(), 1, curve);
    }
    if (!row) {
      row = list_->AddChildView(std::make_unique<CommandBarRow>(this));
      if (animate) {
        row->motion().AnimateOpacityFrom(row->layer(), 0, 1, curve);
      }
    }
    row->set_key(std::move(key));
    rows_.push_back(row);
  }
  for (CommandBarRow* row : old) {
    if (animate) {
      LeaveRow(row, curve);
    } else {
      list_->RemoveChildViewT(row);
    }
  }
  if (hovered_ && *hovered_ >= count) {
    hovered_.reset();
  }
  selected_ = std::min(selected_, count > 0 ? count - 1 : 0);
  RefreshRows();

  // 分隔线跟着列表出现、消失；面板的高度动到新的行数。
  const float divider = rows_.empty() ? 0 : 1;
  const int height = TargetPanelHeight();
  if (animate) {
    divider_motion_.AnimateOpacity(divider_->layer(), divider, curve);
    height_.AnimateTo(height, curve);
  } else {
    if (divider_->layer()->GetTargetOpacity() != divider) {
      divider_motion_.SetOpacity(divider_->layer(), divider);
    }
    if (height_.target() != height || !height_.is_animating()) {
      height_.SetValue(height);
    }
  }
  LayoutPanel();
  PlaceRows(animate);
}

void CommandBarView::RefreshRows() {
  tab_organizer::TabArchive* archive =
      browser_->GetProfile()->IsOffTheRecord()
          ? nullptr
          : tab_organizer::TabArchive::ForProfile(browser_->GetProfile());
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (const AutocompleteMatch* match = RowMatch(i)) {
      std::u16string archived;
      const bool all = RowArchiveId(i) == kArchiveAllId;
      const bool organize = RowArchiveId(i) == kOrganizeId;
      if (organize) {
        archived = L("organize.pill");
      } else if (const std::string id = RowArchiveId(i); all) {
        archived = L("commandBar.archiveAll.note");
      } else if (!id.empty() && archive) {
        if (const tab_organizer::ArchivedTab* tab = archive->Find(id)) {
          archived = ArchivedAgo(base::Time::Now() - tab->archived);
        }
      }
      rows_[i]->Set(*match,
                    archived.empty() ? FolderPathFor(*match) : std::u16string(),
                    theme_, i == selected_, hovered_ == i, archived,
                    !all && !organize, organize);
    }
  }
  panel_->Set(theme_, mode_ == Mode::kNewTab);
}

const AutocompleteMatch* CommandBarView::RowMatch(size_t row) const {
  const AutocompleteResult& result = controller_->result();
  if (row >= shown_.size()) {
    return nullptr;
  }
  if (shown_[row] >= kExtraRow) {
    const size_t extra = shown_[row] - kExtraRow;
    return extra < extras_.size() ? &extras_[extra] : nullptr;
  }
  if (shown_[row] >= result.size()) {
    return nullptr;
  }
  return &result.match_at(shown_[row]);
}

namespace {

// 这个窗口在前，再是同一个用户资料的别的窗口。
std::vector<BrowserWindowInterface*> WindowsOf(
    BrowserWindowInterface* browser) {
  std::vector<BrowserWindowInterface*> windows{browser};
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    if (window != browser && window->GetProfile() == browser->GetProfile()) {
      windows.push_back(window);
    }
  }
  return windows;
}

}  // namespace

std::vector<AutocompleteMatch> CommandBarView::FolderMatches(
    const std::vector<std::string>& taken) const {
  std::vector<AutocompleteMatch> matches;
  std::u16string query;
  base::TrimWhitespace(text_, base::TRIM_ALL, &query);
  if (query.empty()) {
    return matches;
  }
  const std::u16string needle = base::i18n::ToLower(query);
  std::map<std::string, std::u16string> paths;
  for (const pin_folders::Folder& folder :
       pin_folders::Read(browser_->GetProfile())) {
    paths[folder.id] = folder.path;
  }
  std::set<std::string> keys(taken.begin(), taken.end());
  for (BrowserWindowInterface* window : WindowsOf(browser_)) {
    TabStripModel* model = window->GetTabStripModel();
    for (int i = 0; model && i < model->IndexOfFirstNonPinnedTab() &&
                    matches.size() < kMaxFolderMatches;
         ++i) {
      tabs::TabInterface* tab = model->GetTabAtIndex(i);
      auto path = paths.find(pinned_tabs::Folder(tab));
      if (pinned_tabs::IsFavorite(tab) || path == paths.end() ||
          base::i18n::ToLower(path->second).find(needle) ==
              std::u16string::npos) {
        continue;
      }
      content::WebContents* contents = tab->GetContents();
      const GURL url = contents->GetVisibleURL();
      if (!url.is_valid() || IsBlankTabUrl(url)) {
        continue;
      }
      AutocompleteMatch match;
      match.type = AutocompleteMatch::Type::kOpenTab;
      match.destination_url = url;
      match.has_tab_match = true;
      match.fill_into_edit = base::UTF8ToUTF16(url.spec());
      match.contents = url_formatter::FormatUrl(url);
      match.contents_class = {
          ACMatchClassification(0, ACMatchClassification::URL)};
      match.description = pinned_tabs::DisplayName(tab);
      if (match.description.empty()) {
        match.description = contents->GetTitle();
      }
      match.description_class = {
          ACMatchClassification(0, ACMatchClassification::NONE)};
      if (!keys.insert(MatchKey(match)).second) {
        continue;
      }
      matches.push_back(std::move(match));
    }
  }
  return matches;
}

std::vector<AutocompleteMatch> CommandBarView::ArchiveMatches(
    const std::vector<std::string>& taken,
    std::vector<std::string>* ids) const {
  std::vector<AutocompleteMatch> matches;
  std::u16string query;
  base::TrimWhitespace(text_, base::TRIM_ALL, &query);
  Profile* profile = browser_->GetProfile();
  if (query.empty() || profile->IsOffTheRecord()) {
    return matches;
  }
  // 已经有同一个网址的打开的标签（切换到标签页）时不列。
  std::set<std::string> open;
  for (BrowserWindowInterface* window : WindowsOf(browser_)) {
    TabStripModel* model = window->GetTabStripModel();
    for (int i = 0; model && i < model->count(); ++i) {
      open.insert(model->GetWebContentsAt(i)->GetVisibleURL().spec());
    }
  }
  // 多出来的不列，最后一行「在已归档中查看全部 N 个」去整页。
  size_t found = 0;
  for (const tab_organizer::ArchivedTab* tab :
       tab_organizer::TabArchive::ForProfile(profile)->Search(query, 200)) {
    if (open.contains(tab->url.spec())) {
      continue;
    }
    if (++found > kMaxArchiveMatches) {
      continue;
    }
    AutocompleteMatch match;
    match.type = AutocompleteMatch::Type::kHistoryUrl;
    match.destination_url = tab->url;
    match.fill_into_edit = base::UTF8ToUTF16(tab->url.spec());
    match.contents = url_formatter::FormatUrl(tab->url);
    match.contents_class = {
        ACMatchClassification(0, ACMatchClassification::URL)};
    match.description = tab->title.empty() ? match.contents : tab->title;
    match.description_class = {
        ACMatchClassification(0, ACMatchClassification::NONE)};
    matches.push_back(std::move(match));
    ids->push_back(tab->id);
  }
  if (found > kMaxArchiveMatches) {
    AutocompleteMatch match;
    match.type = AutocompleteMatch::Type::kNavsuggest;
    match.destination_url = StarryArchiveUrl(query);
    match.contents = L("internalPage.archive");
    match.contents_class = {
        ACMatchClassification(0, ACMatchClassification::NONE)};
    match.description =
        L("commandBar.archiveAll", {{"count", static_cast<int>(found)}});
    match.description_class = {
        ACMatchClassification(0, ACMatchClassification::NONE)};
    matches.push_back(std::move(match));
    ids->push_back(kArchiveAllId);
  }
  return matches;
}

std::optional<AutocompleteMatch> CommandBarView::OrganizeMatch() const {
  std::u16string query;
  base::TrimWhitespace(text_, base::TRIM_ALL, &query);
  if (query.size() < 2 || browser_->GetProfile()->IsOffTheRecord()) {
    return std::nullopt;
  }
  const std::u16string lower = base::i18n::ToLower(query);
  const bool wanted = std::ranges::any_of(
      base::SplitString(L("commandBar.organize.keywords"), u",",
                        base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY),
      [&](const std::u16string& keyword) {
        const std::u16string word = base::i18n::ToLower(keyword);
        return base::StartsWith(word, lower) || base::StartsWith(lower, word);
      });
  if (!wanted) {
    return std::nullopt;
  }
  const size_t count = tab_organizer::OrganizableTabs(browser_).size();
  if (count < 2) {
    return std::nullopt;
  }
  AutocompleteMatch match;
  match.type = AutocompleteMatch::Type::kNavsuggest;
  match.destination_url = GURL(chrome::kChromeUINewTabURL);
  match.description = L("organize.title");
  match.description_class = {
      ACMatchClassification(0, ACMatchClassification::NONE)};
  match.contents =
      L("commandBar.organize.detail", {{"count", static_cast<int>(count)}});
  match.contents_class = {
      ACMatchClassification(0, ACMatchClassification::NONE)};
  return match;
}

std::string CommandBarView::RowArchiveId(size_t row) const {
  if (row >= shown_.size() || shown_[row] < kExtraRow) {
    return std::string();
  }
  const size_t extra = shown_[row] - kExtraRow;
  return extra < extra_archive_ids_.size() ? extra_archive_ids_[extra]
                                           : std::string();
}

std::u16string CommandBarView::FolderPathFor(
    const AutocompleteMatch& match) const {
  if (!IsTabMatch(match)) {
    return std::u16string();
  }
  std::map<std::string, std::u16string> paths;
  for (const pin_folders::Folder& folder :
       pin_folders::Read(browser_->GetProfile())) {
    paths[folder.id] = folder.path;
  }
  // 常用图块的文件夹（只有一层）。
  for (const pin_folders::Folder& folder :
       favorite_folders::Read(browser_->GetProfile())) {
    paths[folder.id] = folder.path;
  }
  if (paths.empty()) {
    return std::u16string();
  }
  for (BrowserWindowInterface* window : WindowsOf(browser_)) {
    TabStripModel* model = window->GetTabStripModel();
    for (int i = 0; model && i < model->IndexOfFirstNonPinnedTab(); ++i) {
      tabs::TabInterface* tab = model->GetTabAtIndex(i);
      content::WebContents* contents = tab->GetContents();
      if (contents->GetVisibleURL() != match.destination_url &&
          contents->GetLastCommittedURL() != match.destination_url) {
        continue;
      }
      const std::string folder = pinned_tabs::IsFavorite(tab)
                                     ? pinned_tabs::FavoriteFolder(tab)
                                     : pinned_tabs::Folder(tab);
      if (auto path = paths.find(folder); path != paths.end()) {
        return path->second;
      }
    }
  }
  return std::u16string();
}

void CommandBarView::LeaveRow(CommandBarRow* row, const MotionCurve& curve) {
  row->SetCanProcessEventsWithinSubtree(false);
  leaving_.push_back(row);
  row->motion().AnimateOpacity(
      row->layer(), 0, curve,
      base::BindOnce(&CommandBarView::RowLeft, weak_factory_.GetWeakPtr(),
                     row->GetWeakPtr()));
}

void CommandBarView::RowLeft(base::WeakPtr<CommandBarRow> row) {
  if (!row) {
    return;
  }
  auto it = std::ranges::find_if(
      leaving_, [&](CommandBarRow* item) { return item == row.get(); });
  if (it == leaving_.end()) {
    return;
  }
  leaving_.erase(it);
  list_->RemoveChildViewT(row.get());
}

void CommandBarView::DropLeavingRows() {
  for (CommandBarRow* row : leaving_) {
    list_->RemoveChildViewT(row);
  }
  leaving_.clear();
}

void CommandBarView::SyncField() {
  if (field_->IsIMEComposing()) {
    return;
  }
  const bool current = controller_->input().text() == text_;
  const AutocompleteMatch* selected = current ? RowMatch(selected_) : nullptr;
  std::u16string display;
  gfx::Range selection;
  if (selected && selected_ > 0) {
    display = selected->fill_into_edit;
    selection = gfx::Range(display.size());
  } else {
    std::u16string completion;
    // 内联补全只用默认的那条（结果里的第一条），它也得是显示出来的第一行。
    if (current && edited_ && !shown_.empty() && shown_[0] == 0) {
      const AutocompleteMatch& top = *RowMatch(0);
      if (top.allowed_to_be_default_match &&
          !controller_->input().prevent_inline_autocomplete()) {
        completion = top.inline_autocompletion;
      }
    }
    display = text_ + completion;
    selection = gfx::Range(text_.size(), display.size());
  }
  // 文字未变时保留光标和选区；异步建议不覆盖用户主动移动后的光标位置，
  // 仅在输入框仍与上次同步一致或光标位于输入末尾时才执行补全。
  if (field_->GetText() == display) {
    return;
  }
  const gfx::Range& range = field_->GetSelectedRange();
  const bool untouched =
      (field_->GetText() == synced_text_ && range == synced_selection_) ||
      (field_->GetText() == text_ && range == gfx::Range(text_.size()));
  if (selected_ == 0 && !untouched) {
    return;
  }
  syncing_ = true;
  field_->SetText(display);
  field_->SetSelectedRange(selection);
  synced_text_ = std::move(display);
  synced_selection_ = selection;
  syncing_ = false;
}

void CommandBarView::Move(int delta) {
  const size_t count = rows_.size();
  if (count == 0) {
    return;
  }
  selected_ = (selected_ + count + (delta % static_cast<int>(count)) + count) %
              count;
  RefreshRows();
  SyncField();
}

void CommandBarView::Submit() {
  // 没改过 ⌘L 带进来的网址：按它重新打开。
  if (!edited_) {
    if (!text_.empty()) {
      OpenText(text_);
    }
    return;
  }
  if (controller_->input().text() == text_ && RowMatch(selected_)) {
    OpenRow(selected_);
    return;
  }
  // 结果还没出来：按地址栏的规则直接判断。
  if (!text_.empty()) {
    OpenText(text_);
  }
}

void CommandBarView::OpenRow(size_t row) {
  // 「整理标签页」：侧栏翻到整理页（顶部标签布局下是卡片）。
  if (RowArchiveId(row) == kOrganizeId) {
    previous_focus_.SetView(nullptr);
    Close();
    OpenOrganize(browser_);
    return;
  }
  // 「在已归档中查看全部」：整页的归档，带着搜索词。
  if (RowArchiveId(row) == kArchiveAllId) {
    std::u16string query;
    base::TrimWhitespace(text_, base::TRIM_ALL, &query);
    previous_focus_.SetView(nullptr);
    Close();
    OpenStarryArchive(browser_, query);
    return;
  }
  // 归档的标签：恢复它（带着前进后退），切过去。
  if (const std::string id = RowArchiveId(row); !id.empty()) {
    previous_focus_.SetView(nullptr);
    Close();
    if (content::WebContents* contents =
            tab_organizer::TabOrganizer::ForProfile(browser_->GetProfile())
                ->Restore(browser_, id, /*select=*/true)) {
      contents->Focus();
    }
    return;
  }
  if (const AutocompleteMatch* match = RowMatch(row)) {
    // 先拷一份：打开时会关掉命令栏、清掉结果。
    const AutocompleteMatch copy = *match;
    OpenMatch(copy);
  }
}

bool CommandBarView::RemoveSelected() {
  const AutocompleteMatch* match = RowMatch(selected_);
  if (!match || !match->SupportsDeletion()) {
    return false;
  }
  // 先拷一份：删的时候结果会变。
  const AutocompleteMatch copy = *match;
  controller_->DeleteMatch(copy);
  return true;
}

void CommandBarView::OpenText(const std::u16string& text) {
  AutocompleteMatch match;
  AutocompleteClassifierFactory::GetForProfile(browser_->GetProfile())
      ->Classify(text, /*in_keyword_mode=*/false,
                 /*allow_exact_keyword_match=*/false,
                 ::metrics::OmniboxEventProto::OTHER, &match,
                 /*alternate_nav_url=*/nullptr);
  OpenMatch(match);
}

void CommandBarView::OpenMatch(const AutocompleteMatch& match) {
  if (!match.destination_url.is_valid()) {
    return;
  }

  WindowOpenDisposition disposition =
      mode_ == Mode::kNewTab ? WindowOpenDisposition::kNewForegroundTab
                             : WindowOpenDisposition::kCurrentTab;
  if (IsTabMatch(match)) {
    disposition = WindowOpenDisposition::kSwitchToTab;
  }
  const GURL url = match.destination_url;
  const ui::PageTransition transition = ui::PageTransitionFromInt(
      match.transition | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  // 关掉时焦点先回到原处，再打开（新标签会再拿走焦点）。
  previous_focus_.SetView(nullptr);
  Close();
  NavigateParams params(browser_, url, transition);
  params.disposition = disposition;
  Navigate(&params);
  if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
    tab->GetContents()->Focus();
  }
}

BEGIN_METADATA(CommandBarView)
END_METADATA

}  // namespace starry
