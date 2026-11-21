// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/popover/content_blocking_popover.h"

#include <algorithm>
#include <utility>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/content_settings/core/common/features.h"
#include "components/url_formatter/url_formatter.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
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
#include "starry/browser/ui/location/location_picker.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/label.h"
#include "url/origin.h"

namespace starry {

namespace {

using content_blocking::ContentBlockingService;
using content_blocking::ContentFilterTabHelper;

constexpr int kWidth = 260;
constexpr int kPadding = 14;
constexpr int kTile = 30;
constexpr int kTileGlyph = 15;
constexpr int kSpacing = 12;
constexpr int kStatusHeight = 16;
constexpr int kToggleHeight = 22;
constexpr int kReloadHeight = 28;
constexpr int kRowHeight = 28;
constexpr int kDividerHeight = 1;
constexpr int kLocationRowPadding = 3;
constexpr int kSettingsRowTopPadding = 6;

}  // namespace

// 「隐私设置…」：齿轮和文字，悬停时垫一层浅底。
class PrivacySettingsRow : public views::Button {
  METADATA_HEADER(PrivacySettingsRow, views::Button)

 public:
  explicit PrivacySettingsRow(PressedCallback callback)
      : views::Button(std::move(callback)) {
    SetAnimationDuration(base::Milliseconds(motion::kHoverDuration * 1000));
    GetViewAccessibility().SetName(L("toolbar.privacy.settings"));
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const float hover = static_cast<float>(hover_animation().GetCurrentValue());
    if (hover > 0) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(0.07f * hover));
      canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kButton,
                            flags);
    }
    constexpr int kGlyph = 13;
    canvas->Save();
    canvas->Translate(gfx::Vector2d(6, (height() - kGlyph) / 2));
    gfx::PaintVectorIcon(canvas, icons::kGearshape, kGlyph, theme_.on_surface);
    canvas->Restore();
    canvas->DrawStringRectWithFlags(
        L("toolbar.privacy.settings"), UiFont(13), theme_.on_surface,
        gfx::Rect(6 + kGlyph + 7, 0, width() - kGlyph - 19, height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(PrivacySettingsRow)
END_METADATA

// 「位置」：左边是名字，右边是定位权限或位置和上下箭头，点开是菜单。
class LocationRow : public views::Button {
  METADATA_HEADER(LocationRow, views::Button)

 public:
  explicit LocationRow(PressedCallback callback)
      : views::Button(std::move(callback)) {
    SetAnimationDuration(base::Milliseconds(motion::kHoverDuration * 1000));
    GetViewAccessibility().SetName(L("toolbar.privacy.location"));
  }

  void SetValue(std::u16string value) {
    value_ = std::move(value);
    GetViewAccessibility().SetDescription(value_);
    SchedulePaint();
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const float hover = static_cast<float>(hover_animation().GetCurrentValue());
    if (hover > 0) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(0.07f * hover));
      canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kButton,
                            flags);
    }
    const std::u16string title = L("toolbar.privacy.location");
    const gfx::FontList& title_font = UiFont(13);
    const int title_width = gfx::GetStringWidth(title, title_font);
    canvas->DrawStringRectWithFlags(title, title_font, theme_.on_surface,
                                    gfx::Rect(6, 0, title_width, height()),
                                    gfx::Canvas::NO_SUBPIXEL_RENDERING);
    constexpr int kChevron = 9;
    const int chevron_x = width() - 6 - kChevron;
    canvas->Save();
    canvas->Translate(gfx::Vector2d(chevron_x, (height() - kChevron) / 2));
    gfx::PaintVectorIcon(canvas, icons::kChevronUpChevronDown, kChevron,
                         theme_.on_surface_variant);
    canvas->Restore();
    const int value_x = 6 + title_width + 12;
    const int value_width = std::max(0, chevron_x - 5 - value_x);
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(value_, UiFont(12), value_width, gfx::ELIDE_TAIL),
        UiFont(12), theme_.on_surface_variant,
        gfx::Rect(value_x, 0, value_width, height()),
        gfx::Canvas::TEXT_ALIGN_RIGHT | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  Theme theme_ = Theme::DarkBase();
  std::u16string value_;
};

BEGIN_METADATA(LocationRow)
END_METADATA

namespace {

// 「位置」菜单的项。
enum LocationCommand {
  kLocationFollow = 1,
  kLocationSimulated,
  kLocationPick,
  kLocationReal,
};

std::u16string DescribeSimulatedLocation(const LocationDecision& decision) {
  return decision.location ? base::UTF8ToUTF16(decision.location->label)
                           : L("toolbar.privacy.locationNotChosen");
}

blink::mojom::PermissionDescriptorPtr LocationPermissionDescriptor() {
  // 位置行显示网站能否读取位置，包含只能读取大致位置的情况。
  return content::PermissionDescriptorUtil::
      CreatePermissionDescriptorForPermissionType(
          base::FeatureList::IsEnabled(
              content_settings::features::kApproximateGeolocationPermission)
              ? blink::PermissionType::GEOLOCATION_APPROXIMATE
              : blink::PermissionType::GEOLOCATION);
}

}  // namespace

ContentBlockingPopover::ContentBlockingPopover(BrowserWindowInterface* browser,
                                               PopoverHost* host,
                                               content::WebContents* contents)
    : browser_(browser),
      host_(host),
      contents_(contents ? contents->GetWeakPtr() : nullptr),
      url_(contents ? contents->GetLastCommittedURL() : GURL()) {
  auto make_label = [this](const gfx::FontList& font) {
    views::Label* label = AddChildView(std::make_unique<views::Label>());
    label->SetFontList(font);
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetAutoColorReadabilityEnabled(false);
    label->SetElideBehavior(gfx::ELIDE_MIDDLE);
    return label;
  };
  title_ = make_label(UiFont(13, gfx::Font::Weight::SEMIBOLD));
  title_->SetText(L("toolbar.privacy.title"));
  site_ = make_label(UiFont(12));
  site_->SetText(base::UTF8ToUTF16(url_formatter::StripWWW(std::string(url_.host()))));
  status_ = make_label(UiFont(12));
  toggle_label_ = make_label(UiFont(13));
  toggle_label_->SetText(L("toolbar.privacy.blockAdsAndTrackers"));
  switch_ = AddChildView(std::make_unique<SwitchButton>(base::BindRepeating(
      &ContentBlockingPopover::SiteToggled, base::Unretained(this))));
  switch_->GetViewAccessibility().SetName(L("toolbar.privacy.blockAdsAndTrackers"));
  fingerprint_label_ = make_label(UiFont(13));
  fingerprint_label_->SetText(L("toolbar.privacy.fingerprinting"));
  fingerprint_switch_ = AddChildView(std::make_unique<SwitchButton>(
      base::BindRepeating(&ContentBlockingPopover::FingerprintToggled,
                          base::Unretained(this))));
  fingerprint_switch_->GetViewAccessibility().SetName(
      L("toolbar.privacy.fingerprinting"));
  reload_ = AddChildView(std::make_unique<PillButton>(
      base::BindRepeating(&ContentBlockingPopover::Reload,
                          base::Unretained(this)),
      L("toolbar.privacy.reloadToApply"), ButtonVariant::kFilled));
  reload_->SetVisible(false);
  location_ = AddChildView(std::make_unique<LocationRow>(base::BindRepeating(
      &ContentBlockingPopover::ShowLocationMenu, base::Unretained(this))));
  location_subscription_ = AddLocationSimulationObserver(
      base::BindRepeating(&ContentBlockingPopover::OnLocationSimulationChanged,
                          base::Unretained(this)));
  settings_ = AddChildView(std::make_unique<PrivacySettingsRow>(base::BindRepeating(
      &ContentBlockingPopover::OpenSettings, base::Unretained(this))));
  ContentBlockingService::Get()->AddObserver(this);
  ContentFilterTabHelper::AddObserver(this);
  fingerprint_prefs_.Init(prefs::PrefsFor(browser_->GetProfile()));
  const auto fingerprint_changed = base::BindRepeating(
      &ContentBlockingPopover::OnFingerprintProtectionChanged,
      base::Unretained(this));
  fingerprint_prefs_.Add(prefs::kFingerprintProtection, fingerprint_changed);
  fingerprint_prefs_.Add(prefs::kFingerprintProtectionExceptions,
                         fingerprint_changed);
  switch_->SetOn(!Allowlisted(), /*animate=*/false);
  fingerprint_switch_->SetOn(
      FingerprintProtectionEnabledForSite(browser_->GetProfile(), url_), false);
  ObserveLocationPermission();
  Update();
}

ContentBlockingPopover::~ContentBlockingPopover() {
  ContentFilterTabHelper::RemoveObserver(this);
  ContentBlockingService::Get()->RemoveObserver(this);
}

bool ContentBlockingPopover::Allowlisted() const {
  return ContentBlockingService::Get()->IsAllowlisted(url_);
}

void ContentBlockingPopover::Update() {
  ContentBlockingService* service = ContentBlockingService::Get();
  const bool enabled = service->enabled();
  const bool allowlisted = Allowlisted();
  status_->SetVisible(enabled);
  toggle_label_->SetVisible(enabled);
  switch_->SetVisible(enabled);
  if (allowlisted) {
    status_->SetText(L("toolbar.privacy.allowlisted"));
  } else {
    const int count = ContentFilterTabHelper::BlockedCount(contents_.get());
    status_->SetText(L("toolbar.privacy.blockedCount", {{"count", count}}));
  }
  const bool fingerprint =
      FingerprintProtectionEnabled(browser_->GetProfile()) &&
      url_.SchemeIsHTTPOrHTTPS();
  fingerprint_label_->SetVisible(fingerprint);
  fingerprint_switch_->SetVisible(fingerprint);
  location_->SetVisible(url_.SchemeIsHTTPOrHTTPS());
  UpdateLocationValue();
  reload_->SetVisible(changed_);
  InvalidateLayout();
  SchedulePaint();
}

void ContentBlockingPopover::SiteToggled(bool block) {
  ContentBlockingService::Get()->SetAllowlisted(std::string(url_.host()), !block);
  if (!changed_) {
    changed_ = true;
    Update();
    host_->ContentSizeChanged();
  }
}

void ContentBlockingPopover::ToggleForTesting() {
  SiteToggled(!switch_->on());
  switch_->SetOn(!Allowlisted(), /*animate=*/false);
}

void ContentBlockingPopover::FingerprintToggled(bool protect) {
  changed_ = true;
  SetFingerprintProtectionException(browser_->GetProfile(), url_.spec(),
                                    !protect);
}

void ContentBlockingPopover::OnFingerprintProtectionChanged() {
  changed_ = true;
  fingerprint_switch_->SetOn(
      FingerprintProtectionEnabledForSite(browser_->GetProfile(), url_), true);
  Update();
  host_->ContentSizeChanged();
}

void ContentBlockingPopover::ToggleFingerprintForTesting() {
  FingerprintToggled(!fingerprint_switch_->on());
}

void ContentBlockingPopover::Reload() {
  if (content::WebContents* contents = contents_.get()) {
    contents->GetController().Reload(content::ReloadType::NORMAL,
                                     /*check_for_repost=*/true);
  }
  host_->Close();
}

void ContentBlockingPopover::OpenSettings() {
  host_->Close();
  OpenStarrySettings(browser_, "privacy");
}

content::RenderFrameHost* ContentBlockingPopover::LocationFrame() const {
  if (!contents_ || !url_.SchemeIsHTTPOrHTTPS()) {
    return nullptr;
  }
  auto* frame = contents_->GetPrimaryMainFrame();
  if (!frame || !url::Origin::Create(url_).IsSameOriginWith(
                    url::Origin::Create(frame->GetLastCommittedURL()))) {
    return nullptr;
  }
  return frame;
}

std::u16string ContentBlockingPopover::LocationValue() const {
  auto* frame = LocationFrame();
  if (!frame) {
    return L("toolbar.privacy.locationNotAuthorized");
  }
  const auto status = frame->GetBrowserContext()
                          ->GetPermissionController()
                          ->GetPermissionStatusForCurrentDocument(
                              LocationPermissionDescriptor(), frame);
  switch (status) {
    case content::PermissionStatus::ASK:
      return L("toolbar.privacy.locationNotAuthorized");
    case content::PermissionStatus::DENIED:
      return L("toolbar.privacy.locationBlocked");
    case content::PermissionStatus::GRANTED: {
      const LocationDecision decision =
          ResolveLocation(browser_->GetProfile(), url_);
      return decision.simulated ? DescribeSimulatedLocation(decision)
                                : L("toolbar.privacy.locationReal");
    }
  }
}

void ContentBlockingPopover::ObserveLocationPermission() {
  location_permission_subscription_.reset();
  if (auto* frame = LocationFrame();
      frame && !frame->GetLastCommittedOrigin().opaque()) {
    location_permission_subscription_ =
        frame->GetBrowserContext()
            ->GetPermissionController()
            ->SubscribeToPermissionResultChange(
                LocationPermissionDescriptor(), /*render_process_host=*/nullptr,
                frame, frame->GetLastCommittedOrigin().GetURL(),
                /*should_include_device_status=*/false,
                base::IgnoreArgs<content::PermissionResult>(base::BindRepeating(
                    &ContentBlockingPopover::UpdateLocationValue,
                    base::Unretained(this))));
  }
}

void ContentBlockingPopover::UpdateLocationValue() {
  if (location_->GetVisible()) {
    location_->SetValue(LocationValue());
  }
}

void ContentBlockingPopover::OnLocationSimulationChanged() {
  // 模拟设置也会改变 Chromium 返回的权限状态，重新记录订阅的初始值。
  ObserveLocationPermission();
  UpdateLocationValue();
}

void ContentBlockingPopover::ShowLocationMenu() {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  Profile* profile = browser_->GetProfile();
  const std::string site = NormalizeFingerprintSite(url_.spec());
  const SiteLocationRule rule = GetSiteLocationRule(profile, site);
  const LocationDecision default_location = ResolveDefaultLocation(profile);
  std::u16string default_value;
  if (default_location.simulated) {
    default_value = DescribeSimulatedLocation(default_location);
  } else if (rule.mode == SiteLocationMode::kSimulated) {
    default_value = L("toolbar.privacy.locationRealRequiresPermission");
  } else {
    default_value = LocationValue();
  }
  location_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
  location_menu_->AddCheckItem(
      kLocationFollow,
      L("toolbar.privacy.locationFollow", {{"value", default_value}}));
  if (rule.mode == SiteLocationMode::kSimulated) {
    location_menu_->AddCheckItem(
        kLocationSimulated,
        L("toolbar.privacy.locationSimulated",
          {{"place", base::UTF8ToUTF16(rule.location.label)}}));
  }
  location_menu_->AddItem(kLocationPick, L("toolbar.privacy.locationPick"));
  location_menu_->AddCheckItem(kLocationReal,
                               L("toolbar.privacy.locationUseReal"));
  const gfx::Rect rect = views::View::ConvertRectToTarget(
      location_, view, location_->GetLocalBounds());
  MenuOptions options;
  options.anchor = rect;
  view->ShowMenu(location_menu_.get(), rect.bottom_left(), base::DoNothing(),
                 std::move(options));
}

bool ContentBlockingPopover::IsCommandIdChecked(int command_id) const {
  const SiteLocationRule rule = GetSiteLocationRule(
      browser_->GetProfile(), NormalizeFingerprintSite(url_.spec()));
  switch (command_id) {
    case kLocationFollow:
      return rule.mode == SiteLocationMode::kDefault;
    case kLocationSimulated:
      return rule.mode == SiteLocationMode::kSimulated;
    case kLocationReal:
      return rule.mode == SiteLocationMode::kReal;
    default:
      return false;
  }
}

void ContentBlockingPopover::ExecuteCommand(int command_id, int event_flags) {
  Profile* profile = browser_->GetProfile();
  const std::string site = NormalizeFingerprintSite(url_.spec());
  switch (command_id) {
    case kLocationFollow:
      SetSiteLocationRule(profile, site, {.mode = SiteLocationMode::kDefault});
      break;
    case kLocationReal:
      SetSiteLocationRule(profile, site, {.mode = SiteLocationMode::kReal});
      break;
    case kLocationPick: {
      // 卡片先收起来，选点页在窗口的对话框里。
      BrowserWindowInterface* browser = browser_;
      host_->Close();
      ShowLocationPicker(browser, site);
      break;
    }
    default:
      break;
  }
}

std::u16string ContentBlockingPopover::DescribeForTesting() const {
  return std::u16string(title_->GetText()) + u" · " +
         std::u16string(site_->GetText()) + u" · " +
         std::u16string(status_->GetText()) + u" · block=" +
         (switch_->on() ? u"1" : u"0") + u" · fingerprint=" +
         (fingerprint_switch_->on() ? u"1" : u"0") +
         (reload_->GetVisible() ? u" · reload" : u"");
}

gfx::Size ContentBlockingPopover::ContentSize() const {
  int height = kPadding + kTile + kSpacing;
  if (status_->GetVisible()) {
    height += kStatusHeight + kSpacing + kToggleHeight + kSpacing;
  }
  if (fingerprint_switch_->GetVisible()) {
    height += kToggleHeight + kSpacing;
  }
  if (reload_->GetVisible()) {
    height += kReloadHeight + kSpacing;
  }
  if (location_->GetVisible()) {
    height += kDividerHeight + 2 * kLocationRowPadding + kRowHeight;
  }
  height += kDividerHeight + kSettingsRowTopPadding + kRowHeight + 8;
  return gfx::Size(kWidth, height);
}

void ContentBlockingPopover::SetTheme(const Theme& theme) {
  theme_ = theme;
  title_->SetEnabledColor(theme.on_surface);
  site_->SetEnabledColor(theme.on_surface_variant);
  status_->SetEnabledColor(theme.on_surface_variant);
  toggle_label_->SetEnabledColor(theme.on_surface);
  switch_->SetTheme(theme);
  fingerprint_label_->SetEnabledColor(theme.on_surface);
  fingerprint_switch_->SetTheme(theme);
  reload_->SetTheme(theme);
  location_->SetTheme(theme);
  settings_->SetTheme(theme);
  SchedulePaint();
}

void ContentBlockingPopover::Layout(PassKey) {
  const int inner = width() - 2 * kPadding;
  const int text_x = kPadding + kTile + 10;
  title_->SetBounds(text_x, kPadding, width() - text_x - kPadding, 17);
  site_->SetBounds(text_x, kPadding + 16, width() - text_x - kPadding, 14);
  int y = kPadding + kTile + kSpacing;
  if (status_->GetVisible()) {
    status_->SetBounds(kPadding, y, inner, kStatusHeight);
    y += kStatusHeight + kSpacing;
    const gfx::Size toggle = switch_->GetPreferredSize();
    switch_->SetBounds(width() - kPadding - toggle.width(),
                       y + (kToggleHeight - toggle.height()) / 2,
                       toggle.width(), toggle.height());
    toggle_label_->SetBounds(kPadding, y, inner - toggle.width() - 8,
                             kToggleHeight);
    y += kToggleHeight + kSpacing;
  }
  if (fingerprint_switch_->GetVisible()) {
    const gfx::Size toggle = fingerprint_switch_->GetPreferredSize();
    fingerprint_switch_->SetBounds(width() - kPadding - toggle.width(),
                                   y + (kToggleHeight - toggle.height()) / 2,
                                   toggle.width(), toggle.height());
    fingerprint_label_->SetBounds(kPadding, y, inner - toggle.width() - 8,
                                  kToggleHeight);
    y += kToggleHeight + kSpacing;
  }
  if (reload_->GetVisible()) {
    reload_->SetBounds(kPadding, y, inner, kReloadHeight);
    y += kReloadHeight + kSpacing;
  }
  // 「位置」在两条分隔线之间，上下留同样的距离。
  if (location_->GetVisible()) {
    y += kDividerHeight + kLocationRowPadding;
    location_->SetBounds(kPadding - 6, y, inner + 12, kRowHeight);
    y += kRowHeight + kLocationRowPadding;
  }
  y += kDividerHeight + kSettingsRowTopPadding;
  settings_->SetBounds(kPadding - 6, y, inner + 12, kRowHeight);
}

void ContentBlockingPopover::OnPaint(gfx::Canvas* canvas) {
  const bool allowlisted =
      (!ContentBlockingService::Get()->enabled() || Allowlisted()) &&
      !(fingerprint_switch_->GetVisible() && fingerprint_switch_->on());
  const SkColor tint = allowlisted ? theme_.on_surface : theme_.accent;
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(SkColorSetA(tint, 26));
  canvas->DrawRoundRect(gfx::RectF(kPadding, kPadding, kTile, kTile), 8, flags);
  canvas->Save();
  canvas->Translate(gfx::Vector2d(kPadding + (kTile - kTileGlyph) / 2,
                                  kPadding + (kTile - kTileGlyph) / 2));
  gfx::PaintVectorIcon(
      canvas, allowlisted ? icons::kShieldSlash : icons::kShieldLefthalfFilled,
      kTileGlyph, allowlisted ? theme_.on_surface_variant : theme_.accent);
  canvas->Restore();
  const auto paint_divider = [&](int row_y, int top_padding) {
    const int divider_y = row_y - top_padding - kDividerHeight;
    canvas->FillRect(gfx::Rect(kPadding, divider_y, width() - 2 * kPadding,
                               kDividerHeight),
                     theme_.OnSurface(0.1f));
  };
  if (location_->GetVisible()) {
    paint_divider(location_->y(), kLocationRowPadding);
  }
  paint_divider(settings_->y(), kSettingsRowTopPadding);
}

void ContentBlockingPopover::OnContentBlockingChanged() {
  switch_->SetOn(!Allowlisted(), /*animate=*/true);
  const bool was_visible = status_->GetVisible();
  Update();
  if (was_visible != status_->GetVisible()) {
    host_->ContentSizeChanged();
  }
}

void ContentBlockingPopover::OnBlockedCountChanged(
    content::WebContents* contents) {
  if (contents == contents_.get()) {
    Update();
  }
}

BEGIN_METADATA(ContentBlockingPopover)
END_METADATA

}  // namespace starry
