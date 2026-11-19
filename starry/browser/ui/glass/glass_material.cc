// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/glass/glass_material.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "ui/gfx/geometry/skia_conversions.h"

namespace starry {

namespace {

using Filter = cc::StarryShaderPaintFilter;

constexpr size_t kMaxShapes = 4;
constexpr float kShadowRadius = 24;
constexpr float kShadowOffsetY = 8;

// 形状的短边：材质参数均以此为基准缩放。
float ReferenceSize(base::span<const GlassShape> shapes) {
  float size = std::numeric_limits<float>::infinity();
  for (const GlassShape& shape : shapes) {
    size = std::min({size, shape.bounds.width(), shape.bounds.height()});
  }
  return std::isfinite(size) ? size : 0;
}

// 进入「大玻璃」的程度：阴影带上周围的颜色、边缘有溢光。s 到 64 为 0，160 起为 1。
float Largeness(float s, const GlassStyle& style) {
  return style.clear ? 0 : std::clamp((s - 64) / 96, 0.0f, 1.0f);
}

bool IsLargeClass(float s, const GlassStyle& style) {
  return !style.clear && s >= 80;
}

// 柔和模糊尾部查找表：半径 px（设备像素）对应的高斯 sigma，与着色器里的
// wideSigmaFor 保持一致。
float WideSigma(float px) {
  if (px <= 4.76f) {
    return 9;
  }
  if (px <= 5.66f) {
    return std::lerp(9.0f, 15.75f, (px - 4.76f) / 0.9f);
  }
  if (px <= 8) {
    return std::lerp(15.75f, 18.25f, (px - 5.66f) / 2.34f);
  }
  if (px <= 11.3f) {
    return std::lerp(18.25f, 21.5f, (px - 8) / 3.3f);
  }
  if (px <= 16) {
    return std::lerp(21.5f, 29.75f, (px - 11.3f) / 4.7f);
  }
  return 29.75f + 1.755f * (px - 16);
}

struct Material {
  float s = 0;
  float largeness = 0;
  bool large_class = false;
  float inner_amount = 0;
  float outer_amount = 0;
  float blur_radius = 0;
  float shadow_amount = 0;
  float shadow_blur = 0;
  float bleed_amount = 0;
  float bleed_blur = 0;
};

Material MaterialFor(base::span<const GlassShape> shapes,
                     const GlassStyle& style) {
  Material m;
  m.s = ReferenceSize(shapes);
  m.largeness = Largeness(m.s, style);
  m.large_class = IsLargeClass(m.s, style);
  m.inner_amount =
      style.clear ? -std::min(m.s, 60.0f) : -std::min(0.8f * m.s, 60.0f);
  m.outer_amount = 0.2f * m.s;
  m.blur_radius =
      style.clear ? 1 : std::clamp((m.s + 8) / 42, 4.0f / 3, 4.0f);
  m.shadow_amount = std::min(0.625f * m.s, 75.0f);
  m.shadow_blur = m.large_class ? 20 : 0;
  m.bleed_amount = 0.35f * m.s;
  m.bleed_blur = m.large_class ? std::min(0.7f * m.s, 160.0f) / 2 : 0;
  return m;
}

gfx::RectF BoundsOf(base::span<const GlassShape> shapes) {
  gfx::RectF bounds;
  for (const GlassShape& shape : shapes) {
    bounds.Union(shape.bounds);
  }
  return bounds;
}

}  // namespace

float SidebarGlassOverlayOpacity(bool dark, bool over_page) {
  return over_page ? (dark ? 0.45f : 0.52f) : (dark ? 0.12f : 0.16f);
}

float GlassMargin(base::span<const GlassShape> shapes,
                  const GlassStyle& style) {
  Material m = MaterialFor(shapes, style);
  // 阴影：偏移 8、半径 24，画到 erf 的尽头（两个半径）；大玻璃的阴影更深，要画满。
  float margin = style.clear ? 2 : 8 + 24 * (1 + m.largeness);
  if (m.largeness > 0) {
    // 大玻璃的背景边距：阴影和溢光都从该范围采样周围颜色。
    margin = std::max(
        {margin, 0.2f * m.s, m.shadow_amount + 8, m.bleed_amount});
  }
  return std::ceil(margin);
}

cc::StarryShaderPaintFilter::Spec MakeGlassSpec(
    base::span<const GlassShape> all_shapes,
    const GlassStyle& style,
    GlassBackdrop backdrop,
    float margin,
    float scale) {
  base::span<const GlassShape> shapes =
      all_shapes.first(std::min(all_shapes.size(), kMaxShapes));
  Material m = MaterialFor(shapes, style);
  float s = m.s;
  gfx::RectF bounds = BoundsOf(shapes);
  gfx::RectF layer(0, 0, bounds.right() + 2 * margin,
                   bounds.bottom() + 2 * margin);

  Filter::Spec spec;
  spec.shader = Filter::Shader::kGlass;
  spec.scale = scale;
  spec.uniforms = Filter::MakeUniforms(spec.shader);
  auto set = [&spec](std::string_view name,
                     std::initializer_list<float> values) {
    Filter::SetUniform(spec.shader, spec.uniforms, name,
                       base::span<const float>(values));
  };

  set("dpr", {scale});
  set("pixelScale", {scale, scale});
  set("lumaRect", {bounds.x() + margin, bounds.y() + margin, bounds.width(),
                   bounds.height()});
  set("lumaOverride", {style.luma});
  set("backdropMode", {backdrop == GlassBackdrop::kLive ? 1.0f : 0.0f});
  // Keep all blur branches on the same bounded input, including live glass.
  // Mirroring matches the compositor's backdrop boundary condition and lets
  // the downsampled levels be reused for different blur radii.
  spec.crop = gfx::RectFToSkRect(layer);
  // Clear glass needs the finer original background, not the regular 1/4 grid.
  spec.downsample_blurs = !style.clear;

  if (!style.clear && style.debug == 0) {
    // The input keeps its full margin for refraction, shadow color and bleed.
    // Only render the shadow's useful extent (two radii around its offset
    // shape). Beyond it the clamped erf tail is below one 8-bit alpha step.
    // Do not shrink the layer: background damage in the input-only band still
    // needs to invalidate the effect, even when it misses this output crop.
    SkRect output = gfx::RectFToSkRect(bounds);
    output.offset(margin, margin + kShadowOffsetY);
    output.outset(2 * kShadowRadius, 2 * kShadowRadius);
    if (output.intersect(spec.crop) && output != spec.crop) {
      spec.output_crop = output;
    }
  }

  for (size_t i = 0; i < kMaxShapes; ++i) {
    std::string index(1, static_cast<char>('0' + i));
    if (i >= shapes.size()) {
      set("shape" + index, {0, 0, 0, 0});
      set("corner" + index, {0, 0});
      continue;
    }
    const gfx::RectF& r = shapes[i].bounds;
    float radius = shapes[i].radius < 0 ? std::min(r.width(), r.height()) / 2
                                        : shapes[i].radius;
    set("shape" + index, {r.CenterPoint().x() + margin,
                          r.CenterPoint().y() + margin, r.width() / 2,
                          r.height() / 2});
    set("corner" + index, {radius, shapes[i].continuous ? 1.0f : 0.0f});
  }
  set("shapeCount", {static_cast<float>(shapes.size())});
  set("smoothness", {style.smoothness});

  // 按尺寸缩放的 regular / clear 材质参数。
  set("innerAmount", {m.inner_amount});
  set("innerHeight",
      {style.clear ? std::min(0.36f * s, 20.0f) : std::min(0.25f * s, 20.0f)});
  set("outerAmount", {m.outer_amount});
  set("outerHeight", {0.125f * s});
  set("refractionOpacity", {style.clear ? 0.0f : 0.3f});
  set("blurRadius", {m.blur_radius});
  set("blurDist", {-0.5f * s, -1, 0, 0});
  set("blurDist4", {style.clear ? 0 : 0.2f * s});
  set("blurAlpha", {1, 0.5f, 0.5f, 1});
  set("blurAlpha4", {1});

  // 阴影随尺寸变浅的程度：s 从 48 到 160。
  float growth = std::clamp((s - 48) / 112, 0.0f, 1.0f);
  float shadow_base = style.dark ? 0.4f : 0.5f;
  set("shadowOffset", {0, kShadowOffsetY});
  set("shadowRadius", {kShadowRadius});
  set("shadowOpacity",
      {style.clear ? 0 : shadow_base + (0.25f - shadow_base) * growth});
  set("sdrShadowOpacity", {0.08f + 0.16f * growth});
  set("shadowVibrancy", {m.largeness});
  set("shadowAmount", {m.shadow_amount});
  set("shadowHeight", {0.4f * s});
  set("bleedStrength", {m.largeness});
  set("bleedAmount", {m.bleed_amount});
  set("bleedHeight", {0.35f * s});

  set("dark", {style.dark ? 1.0f : 0.0f});
  set("largeClass", {m.large_class ? 1.0f : 0.0f});
  set("clearGlass", {style.clear ? 1.0f : 0.0f});

  // 主光照左上的边缘，补光照右下；都是 90° 的范围，amount 0.5（不额外增强）。
  const float kDiagonal = std::sqrt(0.5f);
  set("highlight", {style.highlight});
  set("keyDir", {-kDiagonal, -kDiagonal});
  set("keyCos", {0});
  set("keyBoost", {0});
  set("fillDir", {kDiagonal, kDiagonal});
  set("fillCos", {0});
  set("fillBoost", {0});
  set("rimWidth", {1});
  set("rimCurvature", {0.7f});
  set("tint", {style.tint.fR * style.tint.fA, style.tint.fG * style.tint.fA,
               style.tint.fB * style.tint.fA, style.tint.fA});
  set("overlay",
      {style.overlay.fR * style.overlay.fA, style.overlay.fG * style.overlay.fA,
       style.overlay.fB * style.overlay.fA, style.overlay.fA});
  set("debugMode", {static_cast<float>(style.debug)});
  set("sampleCulling", {1});

  // 模糊输入：sigma 先按设备像素计算，再换算为图层坐标。
  // blur0：基础背景模糊。regular 使用 1/4 分辨率（4 px 一个 texel），双线性放大后
  // 相当于 sigma 为半个 texel 的高斯；clear 使用 1/2 分辨率。
  float core_sigma = style.clear ? 1.5f : 2;
  // blur1、blur2：模糊半径在整块玻璃上从一半（边缘）变到全部（中间），对应的 sigma。
  float wide_low = WideSigma(0.5f * m.blur_radius * scale);
  float wide_high = WideSigma(m.blur_radius * scale);
  // 着色器用不到的输入设成 0（就是没模糊的背景，不用算）：每个输入每一帧都要算一遍。
  // blur3：阴影带的周围颜色，大玻璃才有。小玻璃这一档不模糊，直接读基础背景。
  float shadow_sigma = 0;
  if (m.largeness > 0) {
    shadow_sigma =
        m.shadow_blur > 0 ? WideSigma(m.shadow_blur * scale) : core_sigma;
  }
  // blur4：溢光取的周围颜色（大玻璃）；着色器自己量平均亮度时也读它（4×3 格，每格读
  // 一个点，sigma 取半格）。
  bool measures_luma = style.luma < 0;
  gfx::SizeF cell(bounds.width() / 4, bounds.height() / 3);
  float far_sigma = 0;
  if (m.bleed_blur > 0) {
    far_sigma = WideSigma(m.bleed_blur * scale);
  } else if (measures_luma) {
    far_sigma = std::max(cell.width(), cell.height()) / 2 * scale;
  } else if (m.largeness > 0) {
    far_sigma = core_sigma;
  }
  set("wideSigma", {wide_low, wide_high});
  for (float sigma :
       {core_sigma, wide_low, wide_high, shadow_sigma, far_sigma}) {
    spec.blur_sigmas.push_back(std::min(sigma / scale, Filter::kMaxSigma));
  }

  // 着色器取样离当前像素最远多少。
  float radius = std::max(std::abs(m.inner_amount), m.outer_amount);
  if (m.largeness > 0) {
    radius = std::max({radius, m.shadow_amount, m.bleed_amount});
  }
  if (measures_luma || style.debug != 0) {
    radius = std::max({radius, layer.width(), layer.height()});
  }
  spec.sample_radius = std::min(radius + 2, Filter::kMaxSampleRadius);
  return spec;
}

}  // namespace starry
