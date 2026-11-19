// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/task_manager/usage_chart.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "base/byte_size.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace starry {

namespace {

constexpr int kTilePaddingX = 12;
constexpr int kTilePaddingY = 10;
constexpr int kChartHeight = 30;
constexpr int kChartTop = 4;
constexpr int kMiniChartWidth = 64;
constexpr int kMiniChartHeight = 18;

SkColor Hairline(const Theme& theme) {
  return theme.OnSurface(theme.is_dark ? 0.07f : 0.06f);
}

}  // namespace

std::u16string FormatPercent(double value) {
  return base::UTF8ToUTF16(base::StringPrintf("%.1f%%", value));
}

std::u16string FormatMemory(double bytes) {
  return ui::FormatBytes(
      base::ByteSize(static_cast<uint64_t>(std::max(bytes, 0.0))));
}

std::u16string FormatOptional(std::optional<double> value, UsageFormat format) {
  return value ? format(*value) : u"—";
}

// SparklineLayout

SparklineLayout::SparklineLayout(const UsageHistory& history,
                                 double floor,
                                 const gfx::RectF& bounds,
                                 float marker_size)
    : count_(history.values().size()) {
  // 上、右留出末端圆点（连同外圈）的位置，下面留半根线宽。
  const float edge = marker_size > 0 ? marker_size / 2 + 2 : 2;
  plot_ = gfx::RectF(bounds.x(), bounds.y() + edge,
                     std::max(bounds.width() - edge, 1.0f),
                     std::max(bounds.height() - edge - 1, 1.0f));
  step_ = plot_.width() /
          static_cast<float>(std::max<size_t>(history.capacity() - 1, 1));
  upper_ = std::max(history.peak().value_or(0), floor) * 1.1;
}

gfx::PointF SparklineLayout::Point(size_t index, double value) const {
  const double ratio = upper_ > 0 ? std::clamp(value / upper_, 0.0, 1.0) : 0;
  return gfx::PointF(
      plot_.right() - static_cast<float>(count_ - 1 - index) * step_,
      plot_.bottom() - static_cast<float>(ratio) * plot_.height());
}

std::optional<size_t> SparklineLayout::IndexNearest(float x) const {
  if (count_ == 0 || step_ <= 0) {
    return std::nullopt;
  }
  const long back = std::lround((plot_.right() - x) / step_);
  const long index = static_cast<long>(count_) - 1 - back;
  if (index < 0) {
    return std::nullopt;
  }
  return std::min(static_cast<size_t>(index), count_ - 1);
}

namespace {

// 8 的实心圆点，外面一圈 2 的底色，压在线上也看得清。
void PaintMarker(gfx::Canvas* canvas,
                 const gfx::PointF& point,
                 SkColor ring,
                 SkColor fill) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(ring);
  canvas->DrawCircle(point, kSparklineMarkerSize / 2 + 2, flags);
  flags.setColor(fill);
  canvas->DrawCircle(point, kSparklineMarkerSize / 2, flags);
}

}  // namespace

void PaintSparkline(gfx::Canvas* canvas,
                    const gfx::RectF& bounds,
                    const UsageHistory& history,
                    double floor,
                    std::optional<size_t> highlighted,
                    std::optional<SkColor> ring,
                    const Theme& theme) {
  const SparklineLayout layout(history, floor, bounds,
                               ring ? kSparklineMarkerSize : 0);
  const gfx::RectF& plot = layout.plot();
  cc::PaintFlags baseline;
  baseline.setAntiAlias(true);
  baseline.setColor(theme.OnSurface(theme.is_dark ? 0.1f : 0.08f));
  canvas->DrawRect(gfx::RectF(bounds.x(), plot.bottom(), bounds.width(), 1),
                   baseline);
  const std::vector<double>& values = history.values();
  if (values.empty()) {
    return;
  }
  std::vector<gfx::PointF> points;
  points.reserve(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    points.push_back(layout.Point(i, values[i]));
  }
  if (points.size() > 1) {
    SkPathBuilder line;
    line.moveTo(points[0].x(), points[0].y());
    for (size_t i = 1; i < points.size(); ++i) {
      line.lineTo(points[i].x(), points[i].y());
    }
    SkPathBuilder area;
    area.moveTo(points[0].x(), plot.bottom());
    for (const gfx::PointF& point : points) {
      area.lineTo(point.x(), point.y());
    }
    area.lineTo(points.back().x(), plot.bottom());
    area.close();
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(theme.OnSurface(0.1f));
    canvas->DrawPath(area.detach(), fill);
    cc::PaintFlags stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(cc::PaintFlags::kStroke_Style);
    stroke.setStrokeWidth(2);
    stroke.setStrokeCap(cc::PaintFlags::kRound_Cap);
    stroke.setStrokeJoin(cc::PaintFlags::kRound_Join);
    stroke.setColor(theme.on_surface);
    canvas->DrawPath(line.detach(), stroke);
  }
  if (!ring) {
    return;
  }
  if (highlighted && *highlighted < points.size()) {
    const gfx::PointF& point = points[*highlighted];
    cc::PaintFlags rule;
    rule.setAntiAlias(true);
    rule.setColor(SkColorSetA(
        theme.on_surface_variant,
        static_cast<U8CPU>(SkColorGetA(theme.on_surface_variant) * 0.6f)));
    canvas->DrawRect(
        gfx::RectF(point.x() - 0.5f, bounds.y(), 1, plot.bottom() - bounds.y()),
        rule);
    PaintMarker(canvas, point, *ring, theme.on_surface);
  }
  PaintMarker(canvas, points.back(), *ring, theme.on_surface);
}

// UsageTile

UsageTile::UsageTile(std::u16string title, double floor, UsageFormat format)
    : title_(std::move(title)), floor_(floor), format_(format) {
  GetViewAccessibility().SetRole(ax::mojom::Role::kGroup);
  GetViewAccessibility().SetName(title_);
  UpdateAccessibility();
}

UsageTile::~UsageTile() = default;

// static
int UsageTile::PreferredHeight() {
  return kTilePaddingY + UiFont(11).GetHeight() + 2 +
         UiFont(18, gfx::Font::Weight::SEMIBOLD).GetHeight() + 2 + kChartTop +
         kChartHeight + kTilePaddingY;
}

void UsageTile::Update(const UsageHistory& history,
                       std::optional<std::u16string> context,
                       std::optional<std::u16string> unavailable) {
  history_ = history;
  context_ = std::move(context);
  unavailable_ = std::move(unavailable);
  if (hovered_ && *hovered_ >= history_.values().size()) {
    hovered_.reset();
  }
  UpdateAccessibility();
  SchedulePaint();
}

void UsageTile::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

gfx::Size UsageTile::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(160, PreferredHeight());
}

gfx::RectF UsageTile::ChartRect() const {
  return gfx::RectF(kTilePaddingX, height() - kTilePaddingY - kChartHeight,
                    std::max(0, width() - 2 * kTilePaddingX), kChartHeight);
}

std::u16string UsageTile::ValueText() const {
  if (hovered_ && *hovered_ < history_.values().size()) {
    return format_(history_.values()[*hovered_]);
  }
  return FormatOptional(history_.latest(), format_);
}

std::u16string UsageTile::CaptionText() const {
  if (hovered_ && *hovered_ < history_.values().size()) {
    const int seconds = static_cast<int>(std::lround(
        static_cast<double>(history_.values().size() - 1 - *hovered_) *
        kTaskSamplingInterval.InSecondsF()));
    return seconds == 0
               ? L("taskManager.chart.now")
               : L("taskManager.chart.secondsAgo", {{"count", seconds}});
  }
  if (context_) {
    return *context_;
  }
  if (const std::optional<double> peak = history_.peak()) {
    return L("taskManager.chart.peak", {{"value", format_(*peak)}});
  }
  return std::u16string();
}

void UsageTile::OnPaint(gfx::Canvas* canvas) {
  const gfx::RectF bounds(GetLocalBounds());
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(theme_.surface_panel);
  canvas->DrawRoundRect(bounds, radius::kCard, fill);
  cc::PaintFlags border;
  border.setAntiAlias(true);
  border.setStyle(cc::PaintFlags::kStroke_Style);
  border.setStrokeWidth(1);
  border.setColor(Hairline(theme_));
  gfx::RectF stroke = bounds;
  stroke.Inset(0.5f);
  canvas->DrawRoundRect(stroke, radius::kCard - 0.5f, border);

  const gfx::FontList& small = UiFont(11);
  const gfx::FontList& title_font = UiFont(11, gfx::Font::Weight::SEMIBOLD);
  const gfx::FontList& value_font = UiFont(18, gfx::Font::Weight::SEMIBOLD);
  const int inner = std::max(0, width() - 2 * kTilePaddingX);
  int y = kTilePaddingY;
  // 名字和右上角的说明同一条基线（字号相同，顶对齐即可）。
  const int title_width =
      std::min(inner, gfx::GetStringWidth(title_, title_font));
  canvas->DrawStringRectWithFlags(
      title_, title_font, theme_.on_surface_variant,
      gfx::Rect(kTilePaddingX, y, title_width + 1, small.GetHeight()),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
  const int caption_x = kTilePaddingX + title_width + 8;
  const std::u16string caption = CaptionText();
  if (!caption.empty() && caption_x < kTilePaddingX + inner) {
    canvas->DrawStringRectWithFlags(
        caption, small, theme_.on_surface_variant,
        gfx::Rect(caption_x, y, kTilePaddingX + inner - caption_x,
                  small.GetHeight()),
        gfx::Canvas::TEXT_ALIGN_RIGHT | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
  y += small.GetHeight() + 2;
  canvas->DrawStringRectWithFlags(
      ValueText(), value_font, theme_.on_surface,
      gfx::Rect(kTilePaddingX, y, inner, value_font.GetHeight()),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);

  const gfx::RectF chart = ChartRect();
  if (unavailable_) {
    // 读不到的说明：最多两行，贴着曲线区的左下角。
    int text_width = static_cast<int>(chart.width());
    int text_height = 0;
    gfx::Canvas::SizeStringInt(*unavailable_, small, &text_width, &text_height,
                               small.GetHeight(), gfx::Canvas::MULTI_LINE);
    text_height = std::min(text_height, small.GetHeight() * 2);
    canvas->DrawStringRectWithFlags(
        *unavailable_, small, theme_.on_surface_variant,
        gfx::Rect(static_cast<int>(chart.x()),
                  static_cast<int>(chart.bottom()) - text_height,
                  static_cast<int>(chart.width()), text_height),
        gfx::Canvas::MULTI_LINE | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    return;
  }
  PaintSparkline(canvas, chart, history_, floor_, hovered_,
                 theme_.surface_panel, theme_);
}

void UsageTile::OnMouseMoved(const ui::MouseEvent& event) {
  std::optional<size_t> index;
  const gfx::RectF chart = ChartRect();
  if (!unavailable_ && chart.Contains(gfx::PointF(event.location()))) {
    index = SparklineLayout(history_, floor_, chart, kSparklineMarkerSize)
                .IndexNearest(event.location().x());
  }
  HoverForTesting(index);
}

void UsageTile::OnMouseExited(const ui::MouseEvent& event) {
  HoverForTesting(std::nullopt);
}

void UsageTile::HoverForTesting(std::optional<size_t> index) {
  if (index && *index >= history_.values().size()) {
    index.reset();
  }
  if (hovered_ == index) {
    return;
  }
  hovered_ = index;
  SchedulePaint();
}

void UsageTile::UpdateAccessibility() {
  if (unavailable_) {
    GetViewAccessibility().SetValue(*unavailable_);
    return;
  }
  const std::optional<double> latest = history_.latest();
  if (!latest) {
    GetViewAccessibility().SetValue(L("taskManager.chart.loading"));
    return;
  }
  std::u16string value =
      L("taskManager.chart.current", {{"value", format_(*latest)}});
  if (const std::optional<double> peak = history_.peak()) {
    value += L("taskManager.chart.clauseSeparator");
    value += L("taskManager.chart.recentPeak", {{"value", format_(*peak)}});
  }
  if (context_) {
    value += L("taskManager.chart.clauseSeparator");
    value += *context_;
  }
  GetViewAccessibility().SetValue(value);
}

BEGIN_METADATA(UsageTile)
END_METADATA

// MiniTrend

MiniTrend::MiniTrend(std::u16string title, double floor, UsageFormat format)
    : title_(std::move(title)), floor_(floor), format_(format) {
  GetViewAccessibility().SetRole(ax::mojom::Role::kGroup);
  GetViewAccessibility().SetName(title_);
}

MiniTrend::~MiniTrend() = default;

void MiniTrend::Update(const UsageHistory& history) {
  history_ = history;
  const std::u16string summary = history_.peak()
                                     ? L("taskManager.chart.recentPeak",
                                         {{"value", format_(*history_.peak())}})
                                     : L("taskManager.chart.loading");
  SetTooltipText(
      L("common.labelValue", {{"label", title_}, {"value", summary}}));
  GetViewAccessibility().SetValue(summary);
  SchedulePaint();
}

void MiniTrend::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

gfx::Size MiniTrend::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  const gfx::FontList& font = UiFont(11, gfx::Font::Weight::MEDIUM);
  return gfx::Size(gfx::GetStringWidth(title_, font) + 6 + kMiniChartWidth,
                   std::max(font.GetHeight(), kMiniChartHeight));
}

void MiniTrend::OnPaint(gfx::Canvas* canvas) {
  const gfx::FontList& font = UiFont(11, gfx::Font::Weight::MEDIUM);
  const int title_width = gfx::GetStringWidth(title_, font);
  canvas->DrawStringRectWithFlags(
      title_, font, theme_.on_surface_variant,
      gfx::Rect(0, (height() - font.GetHeight()) / 2, title_width + 1,
                font.GetHeight()),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
  PaintSparkline(canvas,
                 gfx::RectF(width() - kMiniChartWidth,
                            (height() - kMiniChartHeight) / 2.0f,
                            kMiniChartWidth, kMiniChartHeight),
                 history_, floor_, std::nullopt, std::nullopt, theme_);
}

BEGIN_METADATA(MiniTrend)
END_METADATA

}  // namespace starry
