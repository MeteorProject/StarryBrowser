// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_GLASS_GLASS_MATERIAL_H_
#define STARRY_BROWSER_UI_GLASS_GLASS_MATERIAL_H_

#include "base/containers/span.h"
#include "starry/cc/paint/starry_shader_paint_filter.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/rect_f.h"

namespace starry {

// Liquid Glass 材质参数：按形状尺寸计算着色器（starry/cc/paint/starry_glass_sksl.cc）
// 的输入。

// 一块玻璃的形状，坐标相对玻璃自己的左上角（不含图层多出来的边距）。
struct GlassShape {
  gfx::RectF bounds;
  // 圆角半径；负数是胶囊（短边的一半）。
  float radius = -1;
  // 连续曲率的圆角（超椭圆）。
  bool continuous = false;
};

struct GlassStyle {
  // clear 玻璃：不压暗、几乎不模糊。
  bool clear = false;
  // 界面是深色外观。亮度自适应可能把玻璃换成另一种风格。
  bool dark = false;
  // 边缘高光的强度，按下时可以加强。
  float highlight = 1;
  // 多个形状融合的平滑距离。
  float smoothness = 0;
  // 有色玻璃的颜色，和盖在整块玻璃（含高光）上面的颜色；都不预乘。
  SkColor4f tint = SkColors::kTransparent;
  SkColor4f overlay = SkColors::kTransparent;
  // 玻璃背后的平均亮度（0…1）。负数表示由着色器自行采样整块玻璃计算，开销较大。
  float luma = -1;
  // 调试输出，见着色器的 debugMode。
  int debug = 0;
};

// 侧栏面板叠色强度。
float SidebarGlassOverlayOpacity(bool dark, bool over_page);

// 图层要比形状的包围框往外多出多少：容纳阴影，并让着色器读得到玻璃周围的画面
// （合成器把背景裁到图层范围，范围外是镜像）。
float GlassMargin(base::span<const GlassShape> shapes, const GlassStyle& style);

// 玻璃后面的画面从哪里来。
enum class GlassBackdrop {
  // 合成器里已经画好的画面，实时的：滤镜是图层的背景滤镜。
  kLive,
  // 调用的人自己画的底：滤镜套在这次绘制上，玻璃连同底一起画进图层。
  kPainted,
};

// 滤镜的参数。`margin` 是图层实际多出来的边距，`scale` 是屏幕倍率。
cc::StarryShaderPaintFilter::Spec MakeGlassSpec(
    base::span<const GlassShape> shapes,
    const GlassStyle& style,
    GlassBackdrop backdrop,
    float margin,
    float scale);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_GLASS_GLASS_MATERIAL_H_
