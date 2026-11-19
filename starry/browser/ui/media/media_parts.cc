// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_parts.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/text_elider.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace starry {

namespace {

constexpr int kTextFlags = gfx::Canvas::NO_SUBPIXEL_RENDERING;

SkColor WithAlpha(SkColor color, float alpha) {
  return SkColorSetA(
      color,
      static_cast<U8CPU>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255)));
}

// 盒子里的相对坐标（0…1）。
struct Box {
  explicit Box(const gfx::RectF& rect) : r(rect) {}
  SkPoint P(float x, float y) const {
    return SkPoint::Make(r.x() + r.width() * x, r.y() + r.height() * y);
  }
  float S(float v) const { return r.width() * v; }
  SkRect R(float l, float t, float rr, float b) const {
    return SkRect::MakeLTRB(r.x() + r.width() * l, r.y() + r.height() * t,
                            r.x() + r.width() * rr, r.y() + r.height() * b);
  }
  gfx::RectF r;
};

cc::PaintFlags FillFlags(SkColor color) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(color);
  return flags;
}

cc::PaintFlags StrokeFlags(SkColor color, float width) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(color);
  flags.setStyle(cc::PaintFlags::kStroke_Style);
  flags.setStrokeWidth(width);
  flags.setStrokeCap(cc::PaintFlags::kRound_Cap);
  flags.setStrokeJoin(cc::PaintFlags::kRound_Join);
  return flags;
}

// 圆角的实心三角形：填满再用圆角描一圈。
void RoundTriangle(gfx::Canvas* canvas,
                   const Box& b,
                   SkPoint a,
                   SkPoint c,
                   SkPoint d,
                   SkColor color) {
  SkPathBuilder path;
  path.moveTo(a);
  path.lineTo(c);
  path.lineTo(d);
  path.close();
  const SkPath built = path.detach();
  canvas->DrawPath(built, FillFlags(color));
  canvas->DrawPath(built, StrokeFlags(color, b.S(0.1f)));
}

void Line(gfx::Canvas* canvas,
          SkPoint from,
          SkPoint to,
          const cc::PaintFlags& flags) {
  SkPathBuilder path;
  path.moveTo(from);
  path.lineTo(to);
  canvas->DrawPath(path.detach(), flags);
}

void Polyline(gfx::Canvas* canvas,
              std::initializer_list<SkPoint> points,
              const cc::PaintFlags& flags) {
  SkPathBuilder path;
  bool first = true;
  for (const SkPoint& point : points) {
    if (first) {
      path.moveTo(point);
      first = false;
    } else {
      path.lineTo(point);
    }
  }
  canvas->DrawPath(path.detach(), flags);
}

void Speaker(gfx::Canvas* canvas, const Box& b, SkColor color) {
  SkPathBuilder body;
  body.moveTo(b.P(0.12f, 0.38f));
  body.lineTo(b.P(0.3f, 0.38f));
  body.lineTo(b.P(0.52f, 0.2f));
  body.lineTo(b.P(0.52f, 0.8f));
  body.lineTo(b.P(0.3f, 0.62f));
  body.lineTo(b.P(0.12f, 0.62f));
  body.close();
  const SkPath path = body.detach();
  canvas->DrawPath(path, FillFlags(color));
  canvas->DrawPath(path, StrokeFlags(color, b.S(0.06f)));
}

// 带「10」字样的快进/快退环形箭头：forward 为顺时针快进。
void Seek10(gfx::Canvas* canvas, const Box& b, bool forward, SkColor color) {
  const float stroke = std::max(1.2f, b.S(0.085f));
  const SkPoint center = b.P(0.5f, 0.54f);
  const float radius = b.S(0.36f);
  const SkRect oval =
      SkRect::MakeLTRB(center.x() - radius, center.y() - radius,
                       center.x() + radius, center.y() + radius);
  SkPathBuilder arc;
  arc.addArc(oval, -90, forward ? -300 : 300);
  canvas->DrawPath(arc.detach(), StrokeFlags(color, stroke));
  // 箭头在正上方，指向逆时针（后退）或顺时针（前进）的方向。
  const float tip = forward ? b.S(0.17f) : -b.S(0.17f);
  const SkPoint top = SkPoint::Make(center.x(), center.y() - radius);
  SkPathBuilder head;
  head.moveTo(SkPoint::Make(top.x() + tip, top.y()));
  head.lineTo(SkPoint::Make(top.x(), top.y() - b.S(0.12f)));
  head.lineTo(SkPoint::Make(top.x(), top.y() + b.S(0.12f)));
  head.close();
  canvas->DrawPath(head.detach(), FillFlags(color));
  const int font_size = std::max(6, static_cast<int>(std::round(b.S(0.34f))));
  const gfx::FontList& font = UiFont(font_size, gfx::Font::Weight::BOLD);
  const int height = font.GetHeight();
  const gfx::Rect text_rect(
      static_cast<int>(std::round(b.r.x())),
      static_cast<int>(std::round(center.y() - height / 2.0f)),
      static_cast<int>(std::round(b.r.width())), height);
  canvas->DrawStringRectWithFlags(u"10", font, color, text_rect,
                                  gfx::Canvas::TEXT_ALIGN_CENTER | kTextFlags);
}

}  // namespace

Theme MediaThemeFor(const MediaEntry* entry, const Theme& window) {
  if (!entry || !entry->artwork_color) {
    return window;
  }
  return Theme::Make(entry->artwork_color, window.is_dark,
                     /*tint_surfaces=*/true);
}

void PaintMediaGlyph(gfx::Canvas* canvas,
                     MediaGlyph glyph,
                     const gfx::RectF& box,
                     SkColor color) {
  const Box b(box);
  const cc::PaintFlags line = StrokeFlags(color, std::max(1.2f, b.S(0.1f)));
  switch (glyph) {
    case MediaGlyph::kPlay:
      RoundTriangle(canvas, b, b.P(0.3f, 0.2f), b.P(0.82f, 0.5f),
                    b.P(0.3f, 0.8f), color);
      break;
    case MediaGlyph::kPause: {
      const float r = b.S(0.06f);
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.24f, 0.18f, 0.41f, 0.82f)),
                            r, FillFlags(color));
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.59f, 0.18f, 0.76f, 0.82f)),
                            r, FillFlags(color));
      break;
    }
    case MediaGlyph::kPrevious:
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.14f, 0.22f, 0.25f, 0.78f)),
                            b.S(0.05f), FillFlags(color));
      RoundTriangle(canvas, b, b.P(0.84f, 0.24f), b.P(0.36f, 0.5f),
                    b.P(0.84f, 0.76f), color);
      break;
    case MediaGlyph::kNext:
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.75f, 0.22f, 0.86f, 0.78f)),
                            b.S(0.05f), FillFlags(color));
      RoundTriangle(canvas, b, b.P(0.16f, 0.24f), b.P(0.64f, 0.5f),
                    b.P(0.16f, 0.76f), color);
      break;
    case MediaGlyph::kBack10:
      Seek10(canvas, b, /*forward=*/false, color);
      break;
    case MediaGlyph::kForward10:
      Seek10(canvas, b, /*forward=*/true, color);
      break;
    case MediaGlyph::kPictureInPicture:
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.08f, 0.2f, 0.92f, 0.8f)),
                            b.S(0.1f), line);
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.5f, 0.48f, 0.78f, 0.68f)),
                            b.S(0.04f), FillFlags(color));
      break;
    case MediaGlyph::kExitPictureInPicture:
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.08f, 0.2f, 0.92f, 0.8f)),
                            b.S(0.1f), line);
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.5f, 0.48f, 0.78f, 0.68f)),
                            b.S(0.04f), FillFlags(color));
      Line(canvas, b.P(0.24f, 0.34f), b.P(0.4f, 0.5f), line);
      Polyline(canvas,
               {b.P(0.24f, 0.46f), b.P(0.24f, 0.34f), b.P(0.36f, 0.34f)}, line);
      break;
    case MediaGlyph::kShowTab:
      Line(canvas, b.P(0.28f, 0.72f), b.P(0.72f, 0.28f), line);
      Polyline(canvas,
               {b.P(0.36f, 0.28f), b.P(0.72f, 0.28f), b.P(0.72f, 0.64f)}, line);
      break;
    case MediaGlyph::kDockSidebar:
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.08f, 0.18f, 0.92f, 0.82f)),
                            b.S(0.12f), line);
      Line(canvas, b.P(0.36f, 0.18f), b.P(0.36f, 0.82f), line);
      Line(canvas, b.P(0.78f, 0.5f), b.P(0.52f, 0.5f), line);
      Polyline(canvas, {b.P(0.62f, 0.4f), b.P(0.52f, 0.5f), b.P(0.62f, 0.6f)},
               line);
      break;
    case MediaGlyph::kDockTabStrip:
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.08f, 0.12f, 0.92f, 0.34f)),
                            b.S(0.08f), line);
      Line(canvas, b.P(0.5f, 0.86f), b.P(0.5f, 0.5f), line);
      Polyline(canvas, {b.P(0.39f, 0.61f), b.P(0.5f, 0.5f), b.P(0.61f, 0.61f)},
               line);
      break;
    case MediaGlyph::kFloat:
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.08f, 0.18f, 0.92f, 0.82f)),
                            b.S(0.12f), line);
      canvas->DrawRoundRect(gfx::SkRectToRectF(b.R(0.5f, 0.48f, 0.78f, 0.7f)),
                            b.S(0.05f), FillFlags(color));
      break;
    case MediaGlyph::kSpeaker: {
      Speaker(canvas, b, color);
      const cc::PaintFlags wave =
          StrokeFlags(color, std::max(1.1f, b.S(0.08f)));
      for (float r : {0.16f, 0.3f}) {
        const SkPoint c = b.P(0.52f, 0.5f);
        const float rr = b.S(r);
        SkPathBuilder arc;
        arc.addArc(
            SkRect::MakeLTRB(c.x() - rr, c.y() - rr, c.x() + rr, c.y() + rr),
            -48, 96);
        canvas->DrawPath(arc.detach(), wave);
      }
      break;
    }
    case MediaGlyph::kMuted:
      Speaker(canvas, b, color);
      Line(canvas, b.P(0.66f, 0.38f), b.P(0.88f, 0.62f), line);
      Line(canvas, b.P(0.88f, 0.38f), b.P(0.66f, 0.62f), line);
      break;
    case MediaGlyph::kCollapse:
      Polyline(canvas, {b.P(0.26f, 0.4f), b.P(0.5f, 0.64f), b.P(0.74f, 0.4f)},
               line);
      break;
    case MediaGlyph::kClose:
      Line(canvas, b.P(0.28f, 0.28f), b.P(0.72f, 0.72f), line);
      Line(canvas, b.P(0.72f, 0.28f), b.P(0.28f, 0.72f), line);
      break;
    case MediaGlyph::kPopOut:
      // 右上角开口的方框，箭头从里面伸出去。
      Polyline(canvas,
               {b.P(0.46f, 0.2f), b.P(0.2f, 0.2f), b.P(0.2f, 0.8f),
                b.P(0.8f, 0.8f), b.P(0.8f, 0.54f)},
               line);
      Line(canvas, b.P(0.46f, 0.54f), b.P(0.84f, 0.16f), line);
      Polyline(canvas, {b.P(0.6f, 0.16f), b.P(0.84f, 0.16f), b.P(0.84f, 0.4f)},
               line);
      break;
  }
}

void PaintArtwork(gfx::Canvas* canvas,
                  const gfx::RectF& rect,
                  float radius,
                  const MediaEntry& entry,
                  const Theme& theme) {
  if (rect.IsEmpty()) {
    return;
  }
  gfx::ScopedCanvas scoped(canvas);
  const SkRRect rrect =
      SkRRect::MakeRectXY(gfx::RectFToSkRect(rect), radius, radius);
  canvas->ClipPath(SkPath::RRect(rrect), /*do_anti_alias=*/true);
  const gfx::ImageSkia& image = entry.artwork;
  if (!image.isNull() && image.width() > 0 && image.height() > 0) {
    // 铺满，居中裁掉多出来的。
    const float scale =
        std::max(rect.width() / image.width(), rect.height() / image.height());
    const float w = image.width() * scale;
    const float h = image.height() * scale;
    canvas->DrawImageInt(
        image, 0, 0, image.width(), image.height(),
        static_cast<int>(std::floor(rect.x() + (rect.width() - w) / 2)),
        static_cast<int>(std::floor(rect.y() + (rect.height() - h) / 2)),
        static_cast<int>(std::ceil(w)) + 1, static_cast<int>(std::ceil(h)) + 1,
        /*filter=*/true);
  } else {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    const SkPoint points[2] = {gfx::PointFToSkPoint(rect.origin()),
                               gfx::PointFToSkPoint(rect.bottom_right())};
    const SkColor4f colors[2] = {SkColor4f::FromColor(theme.primary_container),
                                 SkColor4f::FromColor(theme.surface_bright)};
    flags.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr,
                                                        2, SkTileMode::kClamp));
    canvas->DrawRect(rect, flags);
    const float icon =
        std::round(std::min(rect.width(), rect.height()) * 0.42f);
    if (!entry.favicon.isNull() && icon >= 8) {
      canvas->DrawImageInt(
          entry.favicon, 0, 0, entry.favicon.width(), entry.favicon.height(),
          static_cast<int>(std::round(rect.CenterPoint().x() - icon / 2)),
          static_cast<int>(std::round(rect.CenterPoint().y() - icon / 2)),
          static_cast<int>(icon), static_cast<int>(icon), /*filter=*/true);
    } else if (icon >= 8) {
      const gfx::RectF box(rect.CenterPoint().x() - icon / 2,
                           rect.CenterPoint().y() - icon / 2, icon, icon);
      PaintMediaGlyph(canvas,
                      entry.has_video ? MediaGlyph::kPictureInPicture
                                      : MediaGlyph::kSpeaker,
                      box, WithAlpha(theme.on_surface, 0.7f));
    }
  }
  // 一圈很淡的细线：浅色封面放在浅色底上也分得开。
  cc::PaintFlags edge = StrokeFlags(WithAlpha(SK_ColorWHITE, 0.14f), 1);
  gfx::RectF inset = rect;
  inset.Inset(0.5f);
  canvas->DrawRoundRect(inset, std::max(0.0f, radius - 0.5f), edge);
}

void PaintProgressRing(gfx::Canvas* canvas,
                       const gfx::PointF& center,
                       float radius,
                       float stroke,
                       double progress,
                       SkColor track,
                       SkColor fill) {
  canvas->DrawCircle(center, radius, StrokeFlags(track, stroke));
  const float sweep =
      360.0f * static_cast<float>(std::clamp(progress, 0.0, 1.0));
  if (sweep <= 0.5f) {
    return;
  }
  SkPathBuilder arc;
  arc.addArc(SkRect::MakeLTRB(center.x() - radius, center.y() - radius,
                              center.x() + radius, center.y() + radius),
             -90, sweep);
  canvas->DrawPath(arc.detach(), StrokeFlags(fill, stroke));
}

std::u16string FormatMediaTime(base::TimeDelta time) {
  const int64_t total = std::max<int64_t>(0, time.InSeconds());
  const int64_t hours = total / 3600;
  const int64_t minutes = (total / 60) % 60;
  const int64_t seconds = total % 60;
  const auto two = [](int64_t value) {
    std::u16string text = base::NumberToString16(value);
    return value < 10 ? u"0" + text : text;
  };
  if (hours > 0) {
    return base::NumberToString16(hours) + u":" + two(minutes) + u":" +
           two(seconds);
  }
  return base::NumberToString16(minutes) + u":" + two(seconds);
}

void DrawMediaText(gfx::Canvas* canvas,
                   const std::u16string& text,
                   int size,
                   bool semibold,
                   SkColor color,
                   const gfx::Rect& rect,
                   bool align_right) {
  if (rect.width() <= 0 || text.empty()) {
    return;
  }
  const gfx::FontList& font = UiFont(
      size, semibold ? gfx::Font::Weight::SEMIBOLD : gfx::Font::Weight::NORMAL);
  const std::u16string elided =
      gfx::ElideText(text, font, rect.width(), gfx::ELIDE_TAIL);
  canvas->DrawStringRectWithFlags(elided, font, color, rect,
                                  (align_right ? gfx::Canvas::TEXT_ALIGN_RIGHT
                                               : gfx::Canvas::TEXT_ALIGN_LEFT) |
                                      kTextFlags);
}

// MediaButton ---------------------------------------------------------------

MediaButton::MediaButton(PressedCallback callback,
                         MediaGlyph glyph,
                         const std::u16string& tooltip,
                         int size,
                         int glyph_size,
                         Style style)
    : views::Button(std::move(callback)),
      glyph_(glyph),
      size_(size),
      glyph_size_(glyph_size),
      style_(style) {
  SetTooltipText(tooltip);
  GetViewAccessibility().SetName(tooltip);
  SetAnimationDuration(base::Milliseconds(180));
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
}

MediaButton::~MediaButton() = default;

void MediaButton::SetGlyph(MediaGlyph glyph, const std::u16string& tooltip) {
  if (glyph_ != glyph) {
    glyph_ = glyph;
    SchedulePaint();
  }
  if (GetTooltipText() != tooltip) {
    SetTooltipText(tooltip);
    GetViewAccessibility().SetName(tooltip);
  }
}

void MediaButton::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  SchedulePaint();
}

void MediaButton::SetGlyphColor(std::optional<SkColor> color) {
  glyph_color_ = color;
  SchedulePaint();
}

gfx::Size MediaButton::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(size_, size_);
}

void MediaButton::PaintButtonContents(gfx::Canvas* canvas) {
  const float hover =
      GetEnabled() ? static_cast<float>(hover_animation().GetCurrentValue())
                   : 0;
  const gfx::RectF bounds(GetLocalBounds());
  const float radius = std::min(bounds.width(), bounds.height()) / 2;
  SkColor fill = SK_ColorTRANSPARENT;
  SkColor glyph = glyph_color_.value_or(theme_.OnSurface(0.85f));
  switch (style_) {
    case Style::kGhost:
      fill = glyph_color_ ? WithAlpha(*glyph_color_, 0.16f * hover)
                          : theme_.OnSurface(0.1f * hover);
      break;
    case Style::kTonal:
      fill = glyph_color_ ? WithAlpha(*glyph_color_, 0.2f + 0.08f * hover)
                          : theme_.OnSurface(0.1f + 0.05f * hover);
      break;
    case Style::kFilled:
      fill = WithAlpha(theme_.primary, 1 - 0.08f * hover);
      glyph = theme_.on_primary;
      break;
    case Style::kOverlay:
      fill = SkColorSetA(SK_ColorBLACK,
                         static_cast<U8CPU>((0.3f + 0.12f * hover) * 255));
      glyph = glyph_color_.value_or(SK_ColorWHITE);
      break;
  }
  if (!GetEnabled()) {
    glyph = WithAlpha(glyph, 0.3f);
  }
  if (SkColorGetA(fill) > 0) {
    canvas->DrawCircle(bounds.CenterPoint(), radius, FillFlags(fill));
  }
  if (style_ == Style::kOverlay) {
    cc::PaintFlags edge =
        StrokeFlags(SkColorSetA(SK_ColorWHITE, 0.18f * 255), 1);
    canvas->DrawCircle(bounds.CenterPoint(), radius - 0.5f, edge);
  }
  const gfx::RectF box(bounds.CenterPoint().x() - glyph_size_ / 2.0f,
                       bounds.CenterPoint().y() - glyph_size_ / 2.0f,
                       glyph_size_, glyph_size_);
  PaintMediaGlyph(canvas, glyph_, box, glyph);
}

void MediaButton::StateChanged(ButtonState old_state) {
  views::Button::StateChanged(old_state);
  const bool pressed = GetState() == STATE_PRESSED;
  if (pressed == pressed_scale_) {
    return;
  }
  pressed_scale_ = pressed;
  LayerPose pose;
  pose.scale = pressed ? motion::kPressScale : 1;
  press_motion_.AnimateTransform(layer(), pose, motion::Press(),
                                 gfx::PointF(width() / 2.0f, height() / 2.0f));
}

BEGIN_METADATA(MediaButton)
END_METADATA

// MediaSeekBar --------------------------------------------------------------

MediaSeekBar::MediaSeekBar(base::RepeatingCallback<void(double)> seek)
    : seek_(std::move(seek)) {}

MediaSeekBar::~MediaSeekBar() = default;

void MediaSeekBar::Set(std::optional<double> progress,
                       SkColor fill,
                       const Theme& theme) {
  progress_ = progress;
  fill_ = fill;
  theme_ = theme;
  SchedulePaint();
}

double MediaSeekBar::FractionAt(int x) const {
  return width() > 0 ? std::clamp(static_cast<double>(x) / width(), 0.0, 1.0)
                     : 0;
}

void MediaSeekBar::OnPaint(gfx::Canvas* canvas) {
  constexpr float kTrack = 4;
  const float y = height() / 2.0f;
  const gfx::RectF track(0, y - kTrack / 2, width(), kTrack);
  canvas->DrawRoundRect(track, kTrack / 2, FillFlags(theme_.OnSurface(0.16f)));
  const std::optional<double> shown = drag_ ? drag_ : progress_;
  if (!shown) {
    return;
  }
  const float x = static_cast<float>(*shown * width());
  if (x > 0.5f) {
    canvas->DrawRoundRect(gfx::RectF(0, track.y(), x, kTrack), kTrack / 2,
                          FillFlags(fill_));
  }
  if (hovered_ || drag_) {
    cc::PaintFlags knob = FillFlags(SK_ColorWHITE);
    knob.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
        gfx::Vector2d(0, 1), 3, SkColorSetA(SK_ColorBLACK, 0.4f * 255))}));
    canvas->DrawCircle(gfx::PointF(std::clamp(x, 6.0f, width() - 6.0f), y), 6,
                       knob);
  }
}

bool MediaSeekBar::OnMousePressed(const ui::MouseEvent& event) {
  if (!progress_ || !event.IsOnlyLeftMouseButton()) {
    return false;
  }
  drag_ = FractionAt(event.x());
  SchedulePaint();
  return true;
}

bool MediaSeekBar::OnMouseDragged(const ui::MouseEvent& event) {
  if (!drag_) {
    return false;
  }
  drag_ = FractionAt(event.x());
  SchedulePaint();
  return true;
}

void MediaSeekBar::OnMouseReleased(const ui::MouseEvent& event) {
  if (!drag_) {
    return;
  }
  const double fraction = FractionAt(event.x());
  drag_.reset();
  // 先当成已经跳过去了，等网页报新的位置。
  progress_ = fraction;
  SchedulePaint();
  seek_.Run(fraction);
}

void MediaSeekBar::OnMouseCaptureLost() {
  drag_.reset();
  SchedulePaint();
}

void MediaSeekBar::OnMouseEntered(const ui::MouseEvent& event) {
  hovered_ = true;
  SchedulePaint();
}

void MediaSeekBar::OnMouseExited(const ui::MouseEvent& event) {
  hovered_ = false;
  SchedulePaint();
}

BEGIN_METADATA(MediaSeekBar)
END_METADATA

}  // namespace starry
