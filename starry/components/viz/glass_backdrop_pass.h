// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_VIZ_GLASS_BACKDROP_PASS_H_
#define STARRY_COMPONENTS_VIZ_GLASS_BACKDROP_PASS_H_

#include "components/viz/common/quads/aggregated_render_pass.h"

namespace cc {
class FilterOperations;
class StarryShaderPaintFilter;
}  // namespace cc

namespace starry {

// 实时的 Liquid Glass 在界面那边是图层的背景滤镜。照背景滤镜画，合成器画到玻璃时要把
// 已经画好的画面读回来（打断正在画的 render pass、拷一份），而且 macOS 上有背景滤镜的帧
// 整个不能交给系统合成（CoreAnimation），网页、视频都改由 Chromium 自己合成一遍，开销
// 跟着窗口大小走。
//
// 这里在合成一帧之前把它改写成不读回的画法：玻璃下面的那些 quad 再画一份到一张只有
// 玻璃那么大、1/2 分辨率的 render pass（背景 pass），玻璃的着色器拿这张图直接画到目标
// 上（带透明度，盖在原来的画面上）。网页和视频照旧交给系统合成，玻璃自己是一张图层；
// 玻璃后面没变的帧不重画。
//
// 改写不了的情况（玻璃带旋转之类）保持背景滤镜的画法；玻璃要用到比 1/2 分辨率更细的
// 背景时（clear、调试输出）背景 pass 是原分辨率，玻璃走普通的滤镜。
void RewriteGlassBackdrops(viz::AggregatedRenderPassList* passes);

// RewriteGlassBackdrops 生成的玻璃 quad 的滤镜；不是的话返回空。合成器靠它认出玻璃：
// CoreAnimation 的图层可以带它（由 Skia 先画进图层的缓冲），SkiaRenderer 对
// draws_material() 的直接画。
const cc::StarryShaderPaintFilter* GlassPassFilter(
    const cc::FilterOperations& filters);

}  // namespace starry

#endif  // STARRY_COMPONENTS_VIZ_GLASS_BACKDROP_PASS_H_
