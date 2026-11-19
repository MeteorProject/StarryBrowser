// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/glass/glass_view.h"

#include <utility>

#include "cc/paint/paint_flags.h"
#include "starry/cc/paint/starry_shader_paint_filter.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/scoped_canvas.h"

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/glass/glass_devtools.h"
#endif

namespace starry {

GlassView::GlassView() {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
}

GlassView::~GlassView() = default;

void GlassView::SetGlassBounds(const gfx::Rect& bounds) {
  if (glass_bounds_ == bounds) {
    return;
  }
  glass_bounds_ = bounds;
  UpdateBounds();
}

gfx::Rect GlassView::glass_rect() const {
  return gfx::Rect(margin_, margin_, glass_bounds_.width(),
                   glass_bounds_.height());
}

void GlassView::SetCornerRadius(float radius, bool continuous) {
  if (radius_ == radius && continuous_ == continuous) {
    return;
  }
  radius_ = radius;
  continuous_ = continuous;
  UpdateFilter();
}

void GlassView::SetStyle(const GlassStyle& style) {
  style_ = style;
  UpdateBounds();
  UpdateFilter();
}

void GlassView::SetUnderlay(UnderlayPainter painter) {
  underlay_ = std::move(painter);
  UpdateFilter();
}

void GlassView::OnPaint(gfx::Canvas* canvas) {
  if (!underlay_ || !painted_filter_) {
    return;
  }
  // 底画进一个临时的层，滤镜把它变成玻璃（玻璃外面只留阴影）。滤镜是按物理像素
  // 构建的，建这一层时画布不能带缩放，否则 Skia 会按 1 倍画完再放大。
  gfx::ScopedCanvas scoped_canvas(canvas);
  float scale = canvas->UndoDeviceScaleFactor();
  cc::PaintFlags flags;
  flags.setImageFilter(painted_filter_);
  canvas->SaveLayerWithFlags(flags);
  canvas->Scale(scale, scale);
  underlay_.Run(canvas, GetLocalBounds());
  canvas->Restore();
}

void GlassView::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  UpdateFilter();
}

void GlassView::OnDeviceScaleFactorChanged(float old_device_scale_factor,
                                           float new_device_scale_factor) {
  UpdateFilter();
}

void GlassView::AddedToWidget() {
  UpdateFilter();
}

GlassShape GlassView::Shape() const {
  GlassShape shape;
  shape.bounds = gfx::RectF(glass_bounds_.width(), glass_bounds_.height());
  shape.radius = radius_;
  shape.continuous = continuous_;
  return shape;
}

void GlassView::UpdateBounds() {
  GlassShape shape = Shape();
  margin_ = static_cast<int>(GlassMargin(base::span_from_ref(shape), style_));
  gfx::Rect bounds = glass_bounds_;
  bounds.Inset(-margin_);
  SetBoundsRect(bounds);
}

void GlassView::UpdateFilter() {
  if (glass_bounds_.IsEmpty()
#if defined(STARRY_DEVTOOLS)
      || devtools::IsGlassDisabled()
#endif
  ) {
    layer()->SetBackdropFilter(nullptr);
    painted_filter_ = nullptr;
    SchedulePaint();
    return;
  }
#if defined(STARRY_DEVTOOLS)
  if (devtools::UsePlainBlurForGlass()) {
    layer()->SetBackgroundBlur(9);
    return;
  }
#endif
  GlassShape shape = Shape();
  GlassBackdrop backdrop =
      underlay_ ? GlassBackdrop::kPainted : GlassBackdrop::kLive;
  cc::StarryShaderPaintFilter::Spec spec =
      MakeGlassSpec(base::span_from_ref(shape), style_, backdrop, margin_,
                    layer()->device_scale_factor());
#if defined(STARRY_DEVTOOLS)
  devtools::ApplyGlassSpecOverrides(spec, !underlay_.is_null());
#endif
  sk_sp<cc::PaintFilter> current =
      underlay_ ? painted_filter_ : layer()->backdrop_filter();
  if (current && current->type() == cc::PaintFilter::Type::kStarryShader &&
      static_cast<const cc::StarryShaderPaintFilter*>(current.get())->spec() ==
          spec) {
    return;
  }
  auto filter = sk_make_sp<cc::StarryShaderPaintFilter>(std::move(spec));
  if (underlay_) {
    layer()->SetBackdropFilter(nullptr);
    painted_filter_ = std::move(filter);
    SchedulePaint();
  } else {
    painted_filter_ = nullptr;
    layer()->SetBackdropFilter(std::move(filter));
  }
}

BEGIN_METADATA(GlassView)
END_METADATA

}  // namespace starry
