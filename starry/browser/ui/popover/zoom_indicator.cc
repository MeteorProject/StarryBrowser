// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/popover/zoom_indicator.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/number_formatting.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/tabs/public/tab_interface.h"
#include "components/zoom/zoom_controller.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/typography.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/views/controls/label.h"

namespace starry {

namespace {

constexpr int kHeight = 40;
constexpr int kPaddingLeft = 12;
constexpr int kPaddingRight = 6;
constexpr int kGlyph = 14;
constexpr int kPercentWidth = 46;
constexpr int kButton = 28;
constexpr int kResetHeight = 26;

}  // namespace

ZoomIndicator::ZoomIndicator(BrowserWindowInterface* browser)
    : browser_(browser) {
  percent_ = AddChildView(std::make_unique<views::Label>());
  percent_->SetFontList(UiFont(13, gfx::Font::Weight::SEMIBOLD));
  percent_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  percent_->SetAutoColorReadabilityEnabled(false);
  minus_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&ZoomIndicator::Run, base::Unretained(this),
                          IDC_ZOOM_MINUS),
      icons::kMinus, L("menu.view.zoomOut"), kButton, 12));
  plus_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&ZoomIndicator::Run, base::Unretained(this),
                          IDC_ZOOM_PLUS),
      icons::kPlus, L("menu.view.zoomIn"), kButton, 12));
  reset_ = AddChildView(std::make_unique<PillButton>(
      base::BindRepeating(&ZoomIndicator::Run, base::Unretained(this),
                          IDC_ZOOM_NORMAL),
      L("zoom.reset"), ButtonVariant::kTertiary));
  Update();
}

ZoomIndicator::~ZoomIndicator() = default;

void ZoomIndicator::Run(int command) {
  chrome::ExecuteCommand(browser_, command);
}

void ZoomIndicator::Update() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  zoom::ZoomController* zoom =
      tab ? zoom::ZoomController::FromWebContents(tab->GetContents()) : nullptr;
  const int percent = zoom ? zoom->GetZoomPercent() : 100;
  at_default_ = !zoom || zoom->IsAtDefaultZoom();
  percent_->SetText(base::FormatPercent(percent));
  reset_->SetVisible(!at_default_);
  minus_->SetEnabled(
      chrome::IsCommandEnabled(browser_, IDC_ZOOM_MINUS));
  plus_->SetEnabled(chrome::IsCommandEnabled(browser_, IDC_ZOOM_PLUS));
  InvalidateLayout();
}

std::u16string ZoomIndicator::TextForTesting() const {
  return std::u16string(percent_->GetText());
}

gfx::Size ZoomIndicator::ContentSize() const {
  int width = kPaddingLeft + kGlyph + 8 + kPercentWidth + kButton + 2 +
              kButton + kPaddingRight;
  if (!at_default_) {
    width += 4 + reset_->GetPreferredSize().width() - 2;
  }
  return gfx::Size(width, kHeight);
}

void ZoomIndicator::SetTheme(const Theme& theme) {
  theme_ = theme;
  percent_->SetEnabledColor(theme.on_surface);
  minus_->SetTheme(theme);
  plus_->SetTheme(theme);
  reset_->SetTheme(theme);
  SchedulePaint();
}

void ZoomIndicator::Layout(PassKey) {
  int x = kPaddingLeft + kGlyph + 8;
  percent_->SetBounds(x, 0, kPercentWidth, height());
  x += kPercentWidth;
  minus_->SetBounds(x, (height() - kButton) / 2, kButton, kButton);
  x += kButton + 2;
  plus_->SetBounds(x, (height() - kButton) / 2, kButton, kButton);
  x += kButton + 4;
  const int reset_width = reset_->GetPreferredSize().width() - 2;
  reset_->SetBounds(x, (height() - kResetHeight) / 2, reset_width,
                    kResetHeight);
}

void ZoomIndicator::OnPaint(gfx::Canvas* canvas) {
  canvas->Save();
  canvas->Translate(gfx::Vector2d(kPaddingLeft, (height() - kGlyph) / 2));
  gfx::PaintVectorIcon(canvas, icons::kMagnifyingGlassPlus, kGlyph,
                       theme_.on_surface_variant);
  canvas->Restore();
}

BEGIN_METADATA(ZoomIndicator)
END_METADATA

}  // namespace starry
