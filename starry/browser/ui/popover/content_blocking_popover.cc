// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/popover/content_blocking_popover.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/url_formatter/url_formatter.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/privacy/fingerprint_protection.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/label.h"

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
  height += 1 + 6 + kRowHeight + 8;
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
  // 分隔线在 y，下面是「隐私设置…」。
  y += 1 + 6;
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
  // 「隐私设置…」上面的分隔线。
  const int divider = settings_->y() - 7;
  canvas->FillRect(gfx::Rect(kPadding, divider, width() - 2 * kPadding, 1),
                   theme_.OnSurface(0.1f));
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
