// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/agent_parts.h"

#include <memory>
#include <utility>

#include "cc/paint/paint_shader.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkBlendMode.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace starry {

namespace {

// 星光跳动（0.8 秒一趟，最暗 0.35）。
constexpr base::TimeDelta kPulseHalf = base::Milliseconds(400);
constexpr float kPulseLow = 0.35f;
constexpr int kStopPadding = 11;

const gfx::FontList& StopFont() {
  return UiFont(12, gfx::Font::Weight::MEDIUM);
}

}  // namespace

cc::PaintFlags AgentGradient(const gfx::RectF& rect) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  const SkPoint points[2] = {gfx::PointFToSkPoint(rect.origin()),
                             gfx::PointFToSkPoint(rect.bottom_right())};
  const SkColor4f colors[2] = {SkColor4f::FromColor(kAgentViolet),
                               SkColor4f::FromColor(kAgentSky)};
  flags.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr,
                                                      2, SkTileMode::kClamp));
  return flags;
}

cc::PaintFlags AgentGradientStroke(const gfx::RectF& rect, float width) {
  cc::PaintFlags flags = AgentGradient(rect);
  flags.setStyle(cc::PaintFlags::kStroke_Style);
  flags.setStrokeWidth(width);
  return flags;
}

cc::PaintFlags AgentRingGradient(const gfx::PointF& center, float faint) {
  const SkColor4f violet = SkColor4f::FromColor(kAgentViolet);
  const SkColor4f sky = SkColor4f::FromColor(kAgentSky);
  SkColor4f pale = violet;
  pale.fA = faint;
  const SkColor4f colors[] = {violet, sky, pale, sky, violet};
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setShader(cc::PaintShader::MakeSweepGradient(
      center.x(), center.y(), colors, nullptr, std::size(colors),
      SkTileMode::kClamp, 0, 360));
  return flags;
}

void PaintAgentGlassBody(gfx::Canvas* canvas,
                         const Theme& theme,
                         const gfx::RectF& bounds,
                         float radius,
                         bool working) {
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(
      SkColorSetA(theme.surface_panel, (theme.is_dark ? 0.66f : 0.76f) * 255));
  canvas->DrawRoundRect(bounds, radius, fill);
  gfx::RectF stroke = bounds;
  stroke.Inset(0.5f);
  cc::PaintFlags border;
  border.setAntiAlias(true);
  border.setStyle(cc::PaintFlags::kStroke_Style);
  border.setStrokeWidth(1);
  border.setColor(theme.OnSurface(0.1f));
  canvas->DrawRoundRect(stroke, radius - 0.5f, border);
  cc::PaintFlags line = AgentGradientStroke(stroke, 1);
  line.setAlphaf(working ? 0.8f : 0.4f);
  canvas->DrawRoundRect(stroke, radius - 0.5f, line);
}

AgentSparkleView::AgentSparkleView() {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
}

AgentSparkleView::~AgentSparkleView() = default;

void AgentSparkleView::SetPulsing(bool pulsing) {
  pulsing = pulsing && !motion::ReduceMotion();
  if (pulsing_ == pulsing) {
    return;
  }
  pulsing_ = pulsing;
  pulse_.Stop();
  layer()->SetOpacity(1);
  if (pulsing) {
    pulse_.Pulse(layer(), 1.0f, kPulseLow, kPulseHalf);
  }
}

void AgentSparkleView::OnPaint(gfx::Canvas* canvas) {
  // 图标画成白的，再用渐变按图标的形状盖上去。
  const gfx::RectF bounds(GetLocalBounds());
  canvas->SaveLayerAlpha(255);
  gfx::PaintVectorIcon(canvas, icons::kSparkle, width(), SK_ColorWHITE);
  cc::PaintFlags flags = AgentGradient(bounds);
  flags.setBlendMode(SkBlendMode::kSrcIn);
  canvas->DrawRect(bounds, flags);
  canvas->Restore();
}

BEGIN_METADATA(AgentSparkleView)
END_METADATA

AgentStopButton::AgentStopButton(PressedCallback callback)
    : views::Button(std::move(callback)) {
  SetTooltipText(L("agent.stop.help"));
  GetViewAccessibility().SetName(L("agent.stop"));
}

AgentStopButton::~AgentStopButton() = default;

// static
int AgentStopButton::PreferredWidth() {
  return gfx::GetStringWidth(L("agent.stop"), StopFont()) + 2 * kStopPadding;
}

void AgentStopButton::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void AgentStopButton::PaintButtonContents(gfx::Canvas* canvas) {
  const gfx::RectF bounds(GetLocalBounds());
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(theme_.OnSurface(0.08f));
  canvas->DrawRoundRect(bounds, bounds.height() / 2, fill);
  canvas->DrawStringRectWithFlags(
      L("agent.stop"), StopFont(), theme_.on_surface, GetLocalBounds(),
      gfx::Canvas::NO_SUBPIXEL_RENDERING | gfx::Canvas::TEXT_ALIGN_CENTER);
}

BEGIN_METADATA(AgentStopButton)
END_METADATA

}  // namespace starry
