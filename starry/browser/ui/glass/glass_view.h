// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_GLASS_GLASS_VIEW_H_
#define STARRY_BROWSER_UI_GLASS_GLASS_VIEW_H_

#include "base/functional/callback.h"
#include "starry/browser/ui/glass/glass_material.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"

namespace starry {

// 一块 Liquid Glass：折射、模糊它后面已经画好的东西（网页和别的界面），自己带阴影和
// 边缘高光。玻璃上的内容（文字、图标）作为子视图加进来，画在玻璃上面。
//
// 默认是实时的玻璃：图层的背景滤镜（ui::Layer::SetBackdropFilter），在合成器里和网页
// 一起合成，不截图、不拷贝。玻璃后面的画面在动时每一帧都要重算；而且 macOS 上只要画面
// 里有背景滤镜，合成器就不能把网页直接交给系统合成（CoreAnimation），整帧都要自己画。
//
// 所以玻璃后面只有自己画的、不动的界面时（浮动侧栏在网页旁边，后面是窗口底色），用
// SetUnderlay 给一个画底的函数：玻璃连同底一起画进图层，画一次，之后没有开销。
//
// 图层要比玻璃大一圈（阴影在玻璃外面，着色器还要读玻璃周围的画面），所以这个视图的
// bounds 比玻璃大：用 SetGlassBounds 摆位置，子视图放在 glass_rect() 里。
class GlassView : public views::View {
  METADATA_HEADER(GlassView, views::View)

 public:
  GlassView();
  GlassView(const GlassView&) = delete;
  GlassView& operator=(const GlassView&) = delete;
  ~GlassView() override;

  // 玻璃在父视图里的位置和大小。
  void SetGlassBounds(const gfx::Rect& bounds);
  gfx::Rect glass_bounds() const { return glass_bounds_; }
  // 玻璃在这个视图自己坐标里的位置（左上角是边距）。
  gfx::Rect glass_rect() const;

  // 圆角；默认是胶囊。
  void SetCornerRadius(float radius, bool continuous);
  void SetStyle(const GlassStyle& style);
  const GlassStyle& style() const { return style_; }

  // 画玻璃后面的底：往 canvas 上画这个视图范围（`bounds`，视图自己的坐标）里、玻璃后面
  // 的东西。设了之后玻璃是静态的；底变了要调 SchedulePaint。空的回调换回实时的玻璃。
  using UnderlayPainter =
      base::RepeatingCallback<void(gfx::Canvas* canvas,
                                   const gfx::Rect& bounds)>;
  void SetUnderlay(UnderlayPainter painter);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void OnDeviceScaleFactorChanged(float old_device_scale_factor,
                                  float new_device_scale_factor) override;
  void AddedToWidget() override;

 private:
  GlassShape Shape() const;
  void UpdateBounds();
  void UpdateFilter();

  gfx::Rect glass_bounds_;
  float radius_ = -1;
  bool continuous_ = false;
  GlassStyle style_;
  int margin_ = 0;
  UnderlayPainter underlay_;
  // 静态的玻璃用的滤镜，套在 OnPaint 里画底的那一层上。
  sk_sp<cc::PaintFilter> painted_filter_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_GLASS_GLASS_VIEW_H_
