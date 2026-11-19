// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/frame/window_background_view.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"

namespace starry {

WindowBackgroundView::WindowBackgroundView() {
  SetWindowDraggable(this, true);
}

WindowBackgroundView::~WindowBackgroundView() = default;

void WindowBackgroundView::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  SchedulePaint();
}

void WindowBackgroundView::SetCards(std::vector<gfx::Rect> cards,
                                    int radius) {
  if (cards_ == cards && card_radius_ == radius) {
    return;
  }
  cards_ = std::move(cards);
  card_radius_ = radius;
  SchedulePaint();
}

void WindowBackgroundView::SetCardOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.f, 1.f);
  if (card_opacity_ == opacity) {
    return;
  }
  card_opacity_ = opacity;
  SchedulePaint();
}

void WindowBackgroundView::SetFrameControlsRects(std::vector<gfx::Rect> rects) {
  if (frame_controls_rects_ == rects) {
    return;
  }
  frame_controls_rects_ = std::move(rects);
  SchedulePaint();
}

void WindowBackgroundView::PaintUnderlay(gfx::Canvas* canvas,
                                         const gfx::Rect& rect) const {
  gfx::ScopedCanvas scoped_canvas(canvas);
  canvas->Translate(-rect.OffsetFromOrigin());
  PaintContents(canvas);
}

void WindowBackgroundView::OnPaint(gfx::Canvas* canvas) {
  gfx::ScopedCanvas scoped_canvas(canvas);
  for (const gfx::Rect& rect : frame_controls_rects_) {
    canvas->sk_canvas()->clipRect(gfx::RectToSkRect(rect),
                                  SkClipOp::kDifference);
  }
  PaintContents(canvas);
}

void WindowBackgroundView::PaintContents(gfx::Canvas* canvas) const {
  canvas->DrawColor(theme_.surface);
  if (card_opacity_ <= 0) {
    return;
  }
  // 卡片的底和阴影。阴影只加在这块底上，不包住网页的图层。
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(SkColorSetA(
      theme_.surface_panel,
      std::lround(SkColorGetA(theme_.surface_panel) * card_opacity_)));
  const SkColor shadow =
      SkColorSetA(SK_ColorBLACK,
                  std::lround((theme_.is_dark ? 0x59 : 0x1F) * card_opacity_));
  flags.setLooper(gfx::CreateShadowDrawLooper(
      {gfx::ShadowValue(gfx::Vector2d(0, 2), 12, shadow)}));
  for (const gfx::Rect& card : cards_) {
    if (!card.IsEmpty()) {
      canvas->DrawRoundRect(gfx::RectF(card), card_radius_, flags);
    }
  }
}

BEGIN_METADATA(WindowBackgroundView)
END_METADATA

}  // namespace starry
