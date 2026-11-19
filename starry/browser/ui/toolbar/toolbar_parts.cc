// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/toolbar/toolbar_parts.h"

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"

namespace starry {

namespace {

const std::u16string& AssistantTooltip() {
#if BUILDFLAG(IS_MAC)
  return L("toolbar.assistant.help");
#else
  return L("utilityButton.assistant");
#endif
}

}  // namespace

AssistantButton::AssistantButton(BrowserWindowInterface* browser)
    : IconButton(base::BindRepeating(
                     [](BrowserWindowInterface* browser) {
                       if (StarryBrowserView* view =
                               StarryBrowserView::From(browser)) {
                         view->ToggleAssistant();
                       }
                     },
                     browser),
                 icons::kSparkle,
                 AssistantTooltip(),
                 metrics::kToolButtonSize,
                 13),
      browser_(browser) {
  set_context_menu_controller(this);
}

AssistantButton::~AssistantButton() = default;

void AssistantButton::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::mojom::MenuSourceType source_type) {
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    gfx::Point local = point;
    views::View::ConvertPointFromScreen(view, &local);
    view->ShowUtilityButtonsMenu(local);
  }
}

BEGIN_METADATA(AssistantButton)
END_METADATA

ToolbarSeparator::ToolbarSeparator() {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
}

ToolbarSeparator::~ToolbarSeparator() = default;

void ToolbarSeparator::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

gfx::Size ToolbarSeparator::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(kWidth, 16);
}

void ToolbarSeparator::OnPaint(gfx::Canvas* canvas) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(theme_.OnSurface(0.12f));
  const gfx::RectF line((width() - 1) / 2.0f, (height() - 16) / 2.0f, 1, 16);
  canvas->DrawRoundRect(line, 0.5f, flags);
}

BEGIN_METADATA(ToolbarSeparator)
END_METADATA

}  // namespace starry
