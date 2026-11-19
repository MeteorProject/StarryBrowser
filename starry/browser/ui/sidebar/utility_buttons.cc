// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/sidebar/utility_buttons.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/vector_icons/vector_icons.h"
#include "starry/browser/agent/private_windows.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view_targeter.h"

namespace starry {

namespace {

constexpr int kButtonSize = 28;
constexpr int kIconSize = 13;
constexpr int kSpacing = 4;
// 出现、消失时的缩放起点。
constexpr float kAppearScale = 0.8f;

// 菜单的命令 ID。
constexpr int kToggleButtonBase = 100;  // + UtilityButton
constexpr int kCustomize = 200;
constexpr int kLayoutDocked = 300;
constexpr int kLayoutFloating = 301;
constexpr int kLayoutSettings = 303;
constexpr int kLayoutTop = 304;
constexpr int kAddressOnTop = 305;
constexpr int kExtensionsOnTop = 306;
constexpr int kCompactAddress = 307;

struct ButtonInfo {
  UtilityButton kind;
  const char* key;
  bool trailing;
};

// 顺序与 UtilityButton 枚举一一对应。
constexpr auto kButtons = std::to_array<ButtonInfo>({
    {UtilityButton::kDownloads, "downloads", false},
    {UtilityButton::kHistory, "history", false},
    {UtilityButton::kBookmarks, "bookmarks", false},
    {UtilityButton::kExtensions, "extensions", false},
    {UtilityButton::kAssistant, "assistant", false},
    {UtilityButton::kLayout, "layout", true},
    {UtilityButton::kSettings, "settings", true},
});

const ButtonInfo& Info(UtilityButton button) {
  return kButtons[static_cast<size_t>(button)];
}

// 「显示的按钮」菜单里的名字。
const std::u16string& TitleFor(UtilityButton button) {
  switch (button) {
    case UtilityButton::kDownloads:
      return L("common.downloads");
    case UtilityButton::kHistory:
      return L("common.history");
    case UtilityButton::kBookmarks:
      return L("common.bookmarks");
    case UtilityButton::kExtensions:
      return L("common.extensions");
    case UtilityButton::kAssistant:
      return L("utilityButton.assistant");
    case UtilityButton::kLayout:
      return L("utilityButton.layout");
    case UtilityButton::kSettings:
      return L("common.settings");
  }
}

// 按钮悬停提示。快捷键仅在 macOS 上展示，其他平台快捷键不同不附带。
const std::u16string& TooltipFor(UtilityButton button) {
  switch (button) {
    case UtilityButton::kDownloads:
      return L("toolbar.downloads.help");
#if BUILDFLAG(IS_MAC)
    case UtilityButton::kHistory:
      return L("toolbar.history.help");
    case UtilityButton::kBookmarks:
      return L("toolbar.bookmarks.help");
    case UtilityButton::kSettings:
      return L("toolbar.settings.help");
#else
    case UtilityButton::kHistory:
      return L("common.history");
    case UtilityButton::kBookmarks:
      return L("common.bookmarks");
    case UtilityButton::kSettings:
      return L("common.settings");
#endif
    case UtilityButton::kExtensions:
      return L("toolbar.extensions.help");
#if BUILDFLAG(IS_MAC)
    case UtilityButton::kAssistant:
      return L("toolbar.assistant.help");
#else
    case UtilityButton::kAssistant:
      return L("utilityButton.assistant");
#endif
    case UtilityButton::kLayout:
      return L("toolbar.layoutMenu.help");
  }
}

const gfx::VectorIcon& IconFor(UtilityButton button) {
  switch (button) {
    case UtilityButton::kDownloads:
      return icons::kArrowDown;
    case UtilityButton::kHistory:
      return icons::kClock;
    case UtilityButton::kBookmarks:
      return icons::kStar;
    case UtilityButton::kExtensions:
      return icons::kPuzzlepiece;
    case UtilityButton::kAssistant:
      return icons::kSparkle;
    case UtilityButton::kLayout:
      return icons::kSidebarLeft;
    case UtilityButton::kSettings:
      return vector_icons::kSettingsIcon;
  }
}

int CommandFor(UtilityButton button) {
  switch (button) {
    case UtilityButton::kDownloads:
      return IDC_SHOW_DOWNLOADS;
    case UtilityButton::kHistory:
      return IDC_SHOW_HISTORY;
    case UtilityButton::kBookmarks:
      return IDC_SHOW_BOOKMARK_MANAGER;
    case UtilityButton::kExtensions:
      return IDC_MANAGE_EXTENSIONS;
    case UtilityButton::kSettings:
      return IDC_OPTIONS;
    case UtilityButton::kAssistant:
    case UtilityButton::kLayout:
      return 0;
  }
}

ui::ImageModel MenuIcon(const gfx::VectorIcon& icon) {
  return ui::ImageModel::FromVectorIcon(icon, ui::kColorMenuIcon, 14);
}

}  // namespace

const char* UtilityButtonKey(UtilityButton button) {
  return Info(button).key;
}

// 下载按钮：有进行中的下载（下载中、排队、暂停）时外圈绘制总进度，箭头缩小到 9；
// 下载完成时箭头向下回弹。同时统计 Chrome 与 Starry 自身的下载。
class DownloadsButton : public IconButton, public DownloadService::Observer {
  METADATA_HEADER(DownloadsButton, IconButton)

 public:
  DownloadsButton(PressedCallback callback,
                  Profile* profile,
                  base::RepeatingClosure active_changed)
      : IconButton(std::move(callback),
                   icons::kArrowDown,
                   L("toolbar.downloads.help"),
                   kButtonSize,
                   kIconSize),
        active_changed_(std::move(active_changed)) {
    if (DownloadService* service = DownloadService::Get(profile)) {
      service_ = service;
      observation_.Observe(service);
    }
    Recompute(/*animated=*/false);
  }
  DownloadsButton(const DownloadsButton&) = delete;
  DownloadsButton& operator=(const DownloadsButton&) = delete;
  ~DownloadsButton() override = default;

  bool HasActiveDownloads() const { return progress_.has_value(); }

  // 箭头向下回弹动画：完成下载或启动下载图标落入时触发。
  void Bounce() {
    bounce_.SetValue(0);
    bounce_.AnimateTo(1, MotionCurve::Keyframes(0.5, {0, 3, -1, 0},
                                                {0, 0.3f, 0.65f, 1}));
  }
  double shown_progress() const { return shown_progress_.value(); }

  // DownloadService::Observer:
  void OnDownloadAdded(Download* download) override {
    Recompute(/*animated=*/true);
  }
  void OnDownloadUpdated(Download* download) override {
    Recompute(/*animated=*/true);
  }
  void OnDownloadRemoved(const std::string& guid) override {
    Recompute(/*animated=*/true);
  }
  void OnDownloadServiceShutdown() override {
    observation_.Reset();
    service_ = nullptr;
  }

 protected:
  void PaintIcon(gfx::Canvas* canvas, SkColor color) override {
    const float presence = static_cast<float>(presence_.value());
    const float alpha = std::clamp(presence, 0.0f, 1.0f);
    const gfx::PointF center(width() / 2.0f, height() / 2.0f);
    if (presence > 0.001f) {
      // 20 × 20 的圈，线宽 2；出现时从 0.6 放大。
      const float radius = 9 * (0.6f + 0.4f * presence);
      const SkRect oval =
          SkRect::MakeLTRB(center.x() - radius, center.y() - radius,
                           center.x() + radius, center.y() + radius);
      cc::PaintFlags track;
      track.setAntiAlias(true);
      track.setStyle(cc::PaintFlags::kStroke_Style);
      track.setStrokeWidth(2);
      track.setColor(theme().OnSurface(0.12f * alpha));
      canvas->DrawCircle(center, radius, track);
      cc::PaintFlags arc = track;
      arc.setStrokeCap(cc::PaintFlags::kRound_Cap);
      arc.setColor(SkColorSetA(theme().accent,
                               static_cast<U8CPU>(255 * alpha)));
      const float sweep =
          360 * std::max(0.02f, static_cast<float>(shown_progress_.value()));
      SkPathBuilder path;
      path.addArc(oval, -90, sweep);
      canvas->DrawPath(path.detach(), arc);
    }
    const float size = kIconSize + (9 - kIconSize) * alpha;
    const int icon = static_cast<int>(std::round(size));
    canvas->Save();
    canvas->Translate(gfx::Vector2d(
        static_cast<int>(std::round(center.x() - icon / 2.0f)),
        static_cast<int>(std::round(center.y() - icon / 2.0f +
                                    bounce_.value()))));
    gfx::PaintVectorIcon(canvas, icons::kArrowDown, icon, color);
    canvas->Restore();
  }

 private:
  void Recompute(bool animated) {
    std::optional<double> progress;
    std::set<std::string> running;
    bool finished = false;
    if (service_) {
      int64_t received = 0;
      int64_t total = 0;
      bool all_known = true;
      double fraction_sum = 0;
      int count = 0;
      for (Download* item : service_->Downloads()) {
        const DownloadState state = item->State();
        if (state == DownloadState::kComplete &&
            running_.contains(item->Guid())) {
          finished = true;
        }
        if (!IsActive(state)) {
          continue;
        }
        running.insert(item->Guid());
        ++count;
        received += item->ReceivedBytes();
        if (item->TotalBytes() > 0) {
          total += item->TotalBytes();
          fraction_sum +=
              std::min(1.0, static_cast<double>(item->ReceivedBytes()) /
                                static_cast<double>(item->TotalBytes()));
        } else {
          all_known = false;
        }
      }
      // 总进度：全部任务大小已知时按总字节加权，否则取各任务平均值。
      if (count > 0) {
        progress = all_known && total > 0
                       ? std::min(1.0, static_cast<double>(received) /
                                           static_cast<double>(total))
                       : fraction_sum / count;
      }
    }
    running_ = std::move(running);
    const bool was_active = progress_.has_value();
    progress_ = progress;

    const MotionCurve lift = MotionCurve::Of(motion::kLift);
    if (!animated) {
      presence_.SetValue(progress_ ? 1 : 0);
      shown_progress_.SetValue(progress_.value_or(0));
    } else {
      presence_.AnimateTo(progress_ ? 1 : 0, lift);
      if (progress_) {
        if (!was_active) {
          shown_progress_.SetValue(0);
        }
        shown_progress_.AnimateTo(*progress_, motion::Progress());
      }
    }
    if (finished && animated) {
      Bounce();
    }
    SetTooltipText(
        progress_ ? L("toolbar.downloads.progress.help",
                      {{"percent", static_cast<int>(*progress_ * 100)}})
                  : L("toolbar.downloads.help"));
    SchedulePaint();
    if (was_active != progress_.has_value() && active_changed_) {
      active_changed_.Run();
    }
  }

  raw_ptr<DownloadService> service_ = nullptr;
  base::ScopedObservation<DownloadService, DownloadService::Observer>
      observation_{this};
  std::optional<double> progress_;
  std::set<std::string> running_;
  base::RepeatingClosure active_changed_;
  ValueMotion presence_{0, base::BindRepeating(&views::View::SchedulePaint,
                                               base::Unretained(this))};
  ValueMotion shown_progress_{
      0, base::BindRepeating(&views::View::SchedulePaint,
                             base::Unretained(this))};
  ValueMotion bounce_{0, base::BindRepeating(&views::View::SchedulePaint,
                                             base::Unretained(this))};
};

BEGIN_METADATA(DownloadsButton)
END_METADATA

// 无痕窗口标记：强调色胶囊，包含图标和「无痕」文案。
// AI 智能体临时无痕窗口改用星光图标，空间足够时注明智能体名称并在提示中说明关闭时机；
// 空间不足时依次降级为「无痕」与纯图标。
class PrivateBadge : public views::View {
  METADATA_HEADER(PrivateBadge, views::View)

 public:
  enum class Fit { kFull, kShort, kIcon };

  explicit PrivateBadge(std::optional<std::u16string> agent)
      : agent_(agent.has_value()),
        label_(agent ? L("agent.private.badge", {{"client", Short(*agent)}})
                     : L("toolbar.privateBadge")) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    const std::u16string help =
        agent ? L("agent.private.badge.help", {{"client", *agent}})
              : L("toolbar.privateBadge.help");
    SetTooltipText(help);
    GetViewAccessibility().SetName(help);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  // 放得下的最长的写法。
  void FitWidth(int available) {
    Fit fit = Fit::kIcon;
    for (Fit each : {Fit::kFull, Fit::kShort}) {
      if (SizeFor(each).width() <= available) {
        fit = each;
        break;
      }
    }
    if (fit != fit_) {
      fit_ = fit;
      SchedulePaint();
    }
  }
  void ResetFit() { FitWidth(SizeFor(Fit::kFull).width()); }
  gfx::Size FittedSize() const { return SizeFor(fit_); }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return SizeFor(Fit::kFull);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(theme_.accent, 0x24));
    const gfx::RectF bounds(GetLocalBounds());
    canvas->DrawRoundRect(bounds, bounds.height() / 2, fill);
    const int padding = fit_ == Fit::kIcon ? kIconPadding : kPadding;
    canvas->Save();
    canvas->Translate(gfx::Vector2d(padding, (height() - kGlyph) / 2));
    gfx::PaintVectorIcon(canvas, agent_ ? icons::kSparkle : icons::kEyeSlash,
                         kGlyph, theme_.accent);
    canvas->Restore();
    if (fit_ == Fit::kIcon) {
      return;
    }
    const int x = padding + kGlyph + 4;
    canvas->DrawStringRectWithFlags(
        Text(fit_), UiFont(11, gfx::Font::Weight::SEMIBOLD), theme_.accent,
        gfx::Rect(x, 0, std::max(0, width() - x), height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  static constexpr int kGlyph = 11;
  static constexpr int kPadding = 8;
  static constexpr int kIconPadding = 6;
  static constexpr size_t kNameLimit = 18;

  const std::u16string& Text(Fit fit) const {
    return fit == Fit::kFull ? label_ : L("toolbar.privateBadge");
  }

  gfx::Size SizeFor(Fit fit) const {
    if (fit == Fit::kIcon) {
      return gfx::Size(kIconPadding + kGlyph + kIconPadding, 20);
    }
    const int text =
        gfx::GetStringWidth(Text(fit), UiFont(11, gfx::Font::Weight::SEMIBOLD));
    return gfx::Size(kPadding + kGlyph + 4 + text + kPadding, 20);
  }

  // 客户端报的名字可能很长：标记里只放开头（提示里是全的）。
  static std::u16string Short(const std::u16string& name) {
    return gfx::TruncateString(name, kNameLimit, gfx::CHARACTER_BREAK);
  }

  const bool agent_;
  const std::u16string label_;
  Fit fit_ = Fit::kFull;
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(PrivateBadge)
END_METADATA

UtilityButtons::UtilityButtons(BrowserWindowInterface* browser,
                               ChromePlacement placement)
    : browser_(browser),
      placement_(placement),
      incognito_(browser->GetProfile()->IsOffTheRecord()) {
  layout_ = ChromeLayout::FromPrefs(prefs::PrefsFor(browser->GetProfile()),
                                    /*has_extensions=*/false);
  // 空白处照样可以拖动窗口；右键（按钮上也是）显示「显示的按钮」菜单。
  SetWindowDraggable(this, true);
  set_context_menu_controller(this);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));

  for (const ButtonInfo& info : kButtons) {
    auto slot = std::make_unique<Slot>();
    slot->kind = info.kind;
    auto pressed = base::BindRepeating(&UtilityButtons::RunCommand,
                                       base::Unretained(this),
                                       static_cast<int>(info.kind));
    std::unique_ptr<IconButton> button;
    if (info.kind == UtilityButton::kDownloads) {
      auto downloads = std::make_unique<DownloadsButton>(
          pressed, browser->GetProfile(),
          base::BindRepeating(&UtilityButtons::Update, base::Unretained(this),
                              /*animated=*/true));
      downloads_ = downloads.get();
      button = std::move(downloads);
    } else {
      button = std::make_unique<IconButton>(pressed, IconFor(info.kind),
                                            TooltipFor(info.kind), kButtonSize,
                                            kIconSize);
    }
    button->set_context_menu_controller(this);
    button->SetVisible(false);
    slot->button = AddChildView(std::move(button));
    slots_[static_cast<size_t>(info.kind)] = std::move(slot);
  }
  if (incognito_) {
    private_badge_ = AddChildView(std::make_unique<PrivateBadge>(
        agent::PrivateWindows::Get()->OwnerName(browser->GetProfile())));
    private_badge_->set_context_menu_controller(this);
  }

  PrefService* prefs = prefs::PrefsFor(browser->GetProfile());
  prefs_.Init(prefs);
  const auto changed = base::BindRepeating(&UtilityButtons::Update,
                                           base::Unretained(this),
                                           /*animated=*/true);
  prefs_.Add(prefs::kHiddenUtilityButtons, changed);
  prefs_.Add(prefs::kAssistantEnabled, changed);
  slots_[static_cast<size_t>(UtilityButton::kLayout)]->button->SetIcon(
      layout_.tab_strip() ? icons::kRectangleTopthirdInsetFilled
                          : icons::kSidebarLeft);
  Update(/*animated=*/false);
}

UtilityButtons::~UtilityButtons() = default;

void UtilityButtons::SetTheme(const Theme& theme) {
  theme_ = theme;
  for (const auto& slot : slots_) {
    slot->button->SetTheme(theme);
  }
  if (private_badge_) {
    private_badge_->SetTheme(theme);
  }
}

void UtilityButtons::SetChromeLayout(const ChromeLayout& layout,
                                     bool has_extensions) {
  if (layout_ == layout && has_extensions_ == has_extensions) {
    return;
  }
  layout_ = layout;
  has_extensions_ = has_extensions;
  // 布局按钮的图标跟随当前标签布局。
  slots_[static_cast<size_t>(UtilityButton::kLayout)]->button->SetIcon(
      layout.tab_strip() ? icons::kRectangleTopthirdInsetFilled
                         : icons::kSidebarLeft);
  Update(/*animated=*/GetWidget() != nullptr);
}

int UtilityButtons::PreferredWidth() const {
  int width = 0;
  int count = 0;
  if (private_badge_) {
    width += private_badge_->GetPreferredSize().width();
    ++count;
  }
  for (const auto& slot : slots_) {
    if (slot->shown) {
      width += kButtonSize;
      ++count;
    }
  }
  return width + std::max(0, count - 1) * kSpacing;
}

void UtilityButtons::ShowButtonsMenuAt(const gfx::Point& point) {
  gfx::Point local = point;
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    views::View::ConvertPointToTarget(view, this, &local);
  }
  ShowButtonsMenu(local);
}

void UtilityButtons::SetActivePage(InternalPage page) {
  active_page_ = page;
  const bool panel_open = downloads_panel_open_;
  const bool assistant_open = assistant_open_;
  const auto active = [page, panel_open, assistant_open](UtilityButton button) {
    switch (button) {
      case UtilityButton::kDownloads:
        return page == InternalPage::kDownloads || panel_open;
      case UtilityButton::kHistory:
        return page == InternalPage::kHistory;
      case UtilityButton::kBookmarks:
        return page == InternalPage::kBookmarks;
      case UtilityButton::kExtensions:
        return page == InternalPage::kExtensions;
      case UtilityButton::kSettings:
        return page == InternalPage::kSettings ||
               page == InternalPage::kExtensions;
      case UtilityButton::kAssistant:
        return assistant_open;
      case UtilityButton::kLayout:
        return false;
    }
  };
  for (const auto& slot : slots_) {
    slot->button->SetActive(active(slot->kind));
  }
}

void UtilityButtons::SetDownloadsPanelOpen(bool open) {
  if (downloads_panel_open_ == open) {
    return;
  }
  downloads_panel_open_ = open;
  SetActivePage(active_page_);
  Update(/*animated=*/true);
}

void UtilityButtons::SetAssistantOpen(bool open) {
  if (assistant_open_ == open) {
    return;
  }
  assistant_open_ = open;
  SetActivePage(active_page_);
}

void UtilityButtons::SetAssistantBadge(IconBadge badge) {
  if (const std::unique_ptr<Slot>& slot =
          slots_[static_cast<size_t>(UtilityButton::kAssistant)]) {
    slot->button->SetBadge(badge);
  }
}

void UtilityButtons::BounceDownloadsButton() {
  if (downloads_) {
    downloads_->Bounce();
  }
}

gfx::Rect UtilityButtons::DownloadsButtonBounds() const {
  for (const auto& slot : slots_) {
    if (slot->kind == UtilityButton::kDownloads && slot->shown) {
      return slot->button->bounds();
    }
  }
  return gfx::Rect();
}

bool UtilityButtons::HasContent() const {
  if (private_badge_) {
    return true;
  }
  return std::ranges::any_of(slots_,
                             [](const auto& slot) { return slot->shown; });
}

void UtilityButtons::SetContentChangedCallback(base::RepeatingClosure callback) {
  content_changed_ = std::move(callback);
}

std::vector<std::string> UtilityButtons::VisibleButtonsForTesting() const {
  std::vector<std::string> keys;
  for (const auto& slot : slots_) {
    if (slot->shown) {
      keys.push_back(UtilityButtonKey(slot->kind));
    }
  }
  return keys;
}

void UtilityButtons::OpenLayoutMenuForTesting() {
  ShowLayoutMenu();
}

void UtilityButtons::OpenButtonsMenuForTesting() {
  ShowButtonsMenu(GetLocalBounds().CenterPoint());
}

double UtilityButtons::DownloadProgressForTesting() const {
  return downloads_ && downloads_->HasActiveDownloads()
             ? downloads_->shown_progress()
             : -1;
}

bool UtilityButtons::ShouldShow(UtilityButton button) const {
  const base::ListValue& hidden =
      prefs::PrefsFor(browser_->GetProfile())
          ->GetList(prefs::kHiddenUtilityButtons);
  const bool hidden_by_user =
      std::ranges::find(hidden, base::Value(UtilityButtonKey(button))) !=
      hidden.end();
  switch (button) {
    case UtilityButton::kDownloads:
      return !hidden_by_user || downloads_->HasActiveDownloads() ||
             downloads_panel_open_;
    case UtilityButton::kExtensions:
      // 工具栏里紧挨着扩展菜单时不放（菜单里有「管理扩展…」）。
      return !hidden_by_user && !incognito_ &&
             (placement_ == ChromePlacement::kSidebar || !has_extensions_);
    case UtilityButton::kAssistant:
      // 有横向的顶栏时在右上角（AssistantButton）；设置里关掉了助手时没有。
      return !hidden_by_user &&
             prefs::PrefsFor(browser_->GetProfile())
                 ->GetBoolean(prefs::kAssistantEnabled) &&
             placement_ == ChromePlacement::kSidebar &&
             layout_.assistant_placement() ==
                 ChromeLayout::AssistantPlacement::kSidebar;
    default:
      return !hidden_by_user;
  }
}

void UtilityButtons::Update(bool animated) {
  bool changed = false;
  for (const auto& slot : slots_) {
    const bool show = ShouldShow(slot->kind);
    if (show == slot->shown) {
      continue;
    }
    changed = true;
    slot->shown = show;
    IconButton* button = slot->button;
    ui::Layer* layer = button->layer();
    const gfx::PointF center(button->width() / 2.0f, button->height() / 2.0f);
    if (show) {
      button->SetVisible(true);
      if (animated && layer) {
        LayerPose from;
        from.scale = kAppearScale;
        slot->motion.AnimateOpacityFrom(layer, 0, 1, motion::ChromeLayout());
        slot->motion.AnimateTransformFrom(layer, from, LayerPose(),
                                          motion::ChromeLayout(), center);
      }
    } else if (animated && layer) {
      LayerPose to;
      to.scale = kAppearScale;
      slot->motion.AnimateTransform(layer, to, motion::ChromeLayout(), center);
      slot->motion.AnimateOpacity(
          layer, 0, motion::ChromeLayout(),
          base::BindOnce(
              [](IconButton* button, Slot* slot) {
                if (!slot->shown) {
                  button->SetVisible(false);
                  slot->motion.SetOpacity(button->layer(), 1);
                  slot->motion.SetTransform(button->layer(), LayerPose());
                }
              },
              base::Unretained(button), base::Unretained(slot.get())));
    } else {
      button->SetVisible(false);
    }
  }
  if (changed) {
    Arrange(animated);
    PreferredSizeChanged();
  }
  const bool has_content = HasContent();
  if (has_content != had_content_) {
    had_content_ = has_content;
    if (content_changed_) {
      content_changed_.Run();
    }
  }
}

void UtilityButtons::Layout(PassKey) {
  Arrange(/*animated=*/false);
}

void UtilityButtons::Arrange(bool animated) {
  const int y = (height() - kButtonSize) / 2;
  int left = 0;
  std::vector<std::pair<Slot*, gfx::Rect>> targets;
  if (placement_ == ChromePlacement::kToolbar) {
    // 工具栏里排成一行：无痕标记、靠左的一组、靠右的一组，整组靠右（显示、隐藏按钮时右
    // 边不动，其余的滑过去）。
    left = std::max(0, width() - PreferredWidth());
    if (private_badge_) {
      private_badge_->ResetFit();
      const gfx::Size size = private_badge_->FittedSize();
      private_badge_->SetBounds(left, (height() - size.height()) / 2,
                                size.width(), size.height());
      left += size.width() + kSpacing;
    }
    for (bool trailing : {false, true}) {
      for (const auto& slot : slots_) {
        if (Info(slot->kind).trailing != trailing || !slot->shown) {
          continue;
        }
        targets.emplace_back(slot.get(),
                             gfx::Rect(left, y, kButtonSize, kButtonSize));
        left += kButtonSize + kSpacing;
      }
    }
  }
  for (const auto& slot : slots_) {
    if (placement_ == ChromePlacement::kToolbar) {
      break;
    }
    if (Info(slot->kind).trailing || !slot->shown) {
      continue;
    }
    targets.emplace_back(slot.get(),
                         gfx::Rect(left, y, kButtonSize, kButtonSize));
    left += kButtonSize + kSpacing;
  }
  if (private_badge_ && placement_ == ChromePlacement::kSidebar) {
    // 夹在靠左和靠右的两组中间：放不下就换短的写法，不盖住右边的按钮。
    int trailing = 0;
    for (const auto& slot : slots_) {
      if (Info(slot->kind).trailing && slot->shown) {
        trailing += kButtonSize + kSpacing;
      }
    }
    private_badge_->FitWidth(width() - trailing - left);
    const gfx::Size size = private_badge_->FittedSize();
    private_badge_->SetBounds(left, (height() - size.height()) / 2,
                              size.width(), size.height());
  }
  int right = width();
  for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
    if (placement_ == ChromePlacement::kToolbar) {
      break;
    }
    Slot* slot = it->get();
    if (!Info(slot->kind).trailing || !slot->shown) {
      continue;
    }
    right -= kButtonSize;
    targets.emplace_back(slot, gfx::Rect(right, y, kButtonSize, kButtonSize));
    right -= kSpacing;
  }
  for (auto& [slot, rect] : targets) {
    IconButton* button = slot->button;
    const gfx::Rect old = button->bounds();
    button->SetBoundsRect(rect);
    if (animated && button->layer() && !old.IsEmpty() && old != rect &&
        !slot->motion.IsAnimatingOpacity()) {
      // 留下来的按钮从原来的位置滑过去。
      slot->motion.ShiftAndAnimate(
          button->layer(), old.origin() - rect.origin(), LayerPose(),
          motion::ChromeLayout(),
          gfx::PointF(rect.width() / 2.0f, rect.height() / 2.0f));
    }
  }
}

bool UtilityButtons::DoesIntersectRect(const views::View* target,
                                       const gfx::Rect& rect) const {
  if (placement_ == ChromePlacement::kSidebar) {
    return GetLocalBounds().Intersects(rect);
  }
  // 工具栏里这个视图从工具栏的左边一直铺到自己的右边：只有按钮占着的那一段接鼠标。
  const int width = PreferredWidth();
  return gfx::Rect(std::max(0, this->width() - width), 0, width, height())
      .Intersects(rect);
}

void UtilityButtons::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::mojom::MenuSourceType source_type) {
  gfx::Point local = point;
  views::View::ConvertPointFromScreen(this, &local);
  ShowButtonsMenu(local);
}

void UtilityButtons::ShowButtonsMenu(const gfx::Point& point) {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  // 「显示的按钮」菜单：勾选切换可见性（勾选不关闭菜单），底部为外观设置入口。
  menu_ = std::make_unique<ui::SimpleMenuModel>(this);
  menu_->AddTitle(L("toolbar.buttonsMenu.visibleButtons"));
  for (const ButtonInfo& info : kButtons) {
    const int command = kToggleButtonBase + static_cast<int>(info.kind);
    menu_->AddCheckItem(command, TitleFor(info.kind));
    menu_->SetIcon(menu_->GetItemCount() - 1, MenuIcon(IconFor(info.kind)));
  }
  menu_->AddSeparator(ui::NORMAL_SEPARATOR);
  menu_->AddItemWithIcon(kCustomize, L("toolbar.buttonsMenu.customize"),
                         MenuIcon(icons::kSliderHorizontal3));

  gfx::Point at = point;
  views::View::ConvertPointToTarget(this, view, &at);
  MenuOptions options;
  options.keeps_open = base::BindRepeating([](int command) {
    return command >= kToggleButtonBase &&
           command < kToggleButtonBase + static_cast<int>(kUtilityButtonCount);
  });
  view->ShowMenu(menu_.get(), at, base::DoNothing(), std::move(options));
}

void UtilityButtons::ShowLayoutMenu() {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  // 布局菜单：标签布局三选一（浮动侧栏自动隐藏）；侧栏布局下可将地址栏与扩展按钮
  // 移到顶部；地址栏置顶时可切换紧凑地址栏。末尾为外观设置入口。
  menu_ = std::make_unique<ui::SimpleMenuModel>(this);
  menu_->AddTitle(L("toolbar.layoutMenu.tabLayout"));
  menu_->AddRadioItem(kLayoutDocked, L("layout.dockedSidebar"), 1);
  menu_->AddRadioItem(kLayoutFloating, L("layout.floatingSidebar"), 1);
  menu_->AddRadioItem(kLayoutTop, L("layout.top"), 1);
  menu_->AddSeparator(ui::NORMAL_SEPARATOR);
  if (layout_.is_sidebar()) {
    menu_->AddCheckItem(kAddressOnTop, L("toolbar.layoutMenu.addressBarOnTop"));
    menu_->AddCheckItem(kExtensionsOnTop,
                        L("toolbar.layoutMenu.extensionsOnTop"));
  }
  menu_->AddCheckItem(kCompactAddress,
                      L("toolbar.layoutMenu.compactAddressBar"));
  menu_->AddSeparator(ui::NORMAL_SEPARATOR);
  menu_->AddItem(kLayoutSettings, L("toolbar.layoutMenu.settings"));

  IconButton* button = slots_[static_cast<size_t>(UtilityButton::kLayout)]
                           ->button;
  const gfx::Rect anchor = views::View::ConvertRectToTarget(
      button, view, button->GetLocalBounds());
  MenuOptions options;
  options.anchor = anchor;
  view->ShowMenu(menu_.get(), anchor.origin(), base::DoNothing(),
                 std::move(options));
}

bool UtilityButtons::IsCommandIdChecked(int command_id) const {
  PrefService* prefs = prefs::PrefsFor(browser_->GetProfile());
  if (command_id >= kToggleButtonBase &&
      command_id < kToggleButtonBase + static_cast<int>(kUtilityButtonCount)) {
    const auto button =
        static_cast<UtilityButton>(command_id - kToggleButtonBase);
    const base::ListValue& hidden =
        prefs->GetList(prefs::kHiddenUtilityButtons);
    return std::ranges::find(hidden, base::Value(UtilityButtonKey(button))) ==
           hidden.end();
  }
  const TabLayout tab_layout = ChromeLayout::TabLayoutFromPrefs(prefs);
  switch (command_id) {
    case kLayoutDocked:
      return tab_layout == TabLayout::kDockedSidebar;
    case kLayoutFloating:
      return tab_layout == TabLayout::kFloatingSidebar;
    case kLayoutTop:
      return tab_layout == TabLayout::kTop;
    case kAddressOnTop:
      return prefs->GetBoolean(prefs::kAddressBarOnTop);
    case kExtensionsOnTop:
      return prefs->GetBoolean(prefs::kExtensionsOnTop);
    case kCompactAddress:
      return prefs->GetBoolean(prefs::kCompactAddressBar) &&
             IsCommandIdEnabled(kCompactAddress);
    default:
      return false;
  }
}

bool UtilityButtons::IsCommandIdEnabled(int command_id) const {
  if (command_id ==
      kToggleButtonBase + static_cast<int>(UtilityButton::kAssistant)) {
    // 助手关掉了：显不显示它的按钮没有意义。
    return prefs::PrefsFor(browser_->GetProfile())
        ->GetBoolean(prefs::kAssistantEnabled);
  }
  if (command_id == kCompactAddress) {
    // 地址栏在顶部时才有紧凑地址栏。
    PrefService* prefs = prefs::PrefsFor(browser_->GetProfile());
    return ChromeLayout::TabLayoutFromPrefs(prefs) == TabLayout::kTop ||
           prefs->GetBoolean(prefs::kAddressBarOnTop);
  }
  return true;
}

void UtilityButtons::ExecuteCommand(int command_id, int event_flags) {
  PrefService* prefs = prefs::PrefsFor(browser_->GetProfile());
  if (command_id >= kToggleButtonBase &&
      command_id < kToggleButtonBase + static_cast<int>(kUtilityButtonCount)) {
    ToggleButton(static_cast<UtilityButton>(command_id - kToggleButtonBase));
    return;
  }
  switch (command_id) {
    case kCustomize:
    case kLayoutSettings:
      OpenStarrySettings(browser_, "appearance");
      break;
    case kLayoutDocked:
      prefs->SetBoolean(prefs::kSidebarAutoHide, false);
      prefs->SetString(prefs::kSidebarLayout, prefs::kSidebarLayoutDocked);
      break;
    case kLayoutFloating:
      prefs->SetBoolean(prefs::kSidebarAutoHide, false);
      prefs->SetString(prefs::kSidebarLayout, prefs::kSidebarLayoutFloating);
      break;
    case kLayoutTop:
      prefs->SetBoolean(prefs::kSidebarAutoHide, false);
      prefs->SetString(prefs::kSidebarLayout, prefs::kSidebarLayoutTop);
      break;
    case kAddressOnTop:
    case kExtensionsOnTop:
    case kCompactAddress: {
      const char* pref = command_id == kAddressOnTop ? prefs::kAddressBarOnTop
                         : command_id == kExtensionsOnTop
                             ? prefs::kExtensionsOnTop
                             : prefs::kCompactAddressBar;
      prefs->SetBoolean(pref, !prefs->GetBoolean(pref));
      break;
    }
  }
}

void UtilityButtons::RunCommand(int button_index) {
  const auto button = static_cast<UtilityButton>(button_index);
  if (button == UtilityButton::kLayout) {
    ShowLayoutMenu();
    return;
  }
  if (button == UtilityButton::kAssistant) {
    if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
      view->ToggleAssistant();
    }
    return;
  }
  // 下载按钮：弹出最近下载面板，不直接打开下载页。
  if (button == UtilityButton::kDownloads) {
    if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
      view->ToggleDownloadsPanel();
      return;
    }
  }
  chrome::ExecuteCommand(browser_, CommandFor(button));
}

void UtilityButtons::ToggleButton(UtilityButton button) {
  ScopedListPrefUpdate update(prefs::PrefsFor(browser_->GetProfile()),
                              prefs::kHiddenUtilityButtons);
  base::Value key(UtilityButtonKey(button));
  auto it = std::ranges::find(*update, key);
  if (it != update->end()) {
    update->erase(it);
  } else {
    update->Append(std::move(key));
  }
}

BEGIN_METADATA(UtilityButtons)
END_METADATA

}  // namespace starry
