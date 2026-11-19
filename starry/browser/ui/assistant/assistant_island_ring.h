// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_ISLAND_RING_H_
#define STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_ISLAND_RING_H_

#include <memory>

#include "starry/browser/ui/motion/layer_loop.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/compositor/layer_delegate.h"
#include "ui/views/view.h"

namespace ui {
class Layer;
class LayerTextured;
}  // namespace ui

namespace starry {

// 岛的外圈：贴着玻璃的圆角矩形，1.5 的线往里缩 0.75。
// - 输入框有焦点：紫到天蓝的渐变线，不透明度 0.75。
// - 智能体在做事（不在等用户）：一段紫到天蓝的渐变绕着转，2.8 秒一圈。转的是合成器上的
//   图层变换（一个边长是对角线的方块画着锥形渐变，外面这层用线的形状当遮罩），不占主线程。
//   减弱动态效果时是静止的焦点那种渐变。
// 不接鼠标。
class AssistantIslandRing : public views::View {
  METADATA_HEADER(AssistantIslandRing, views::View)

 public:
  enum class Mode { kNone, kFocus, kBusy };

  AssistantIslandRing();
  AssistantIslandRing(const AssistantIslandRing&) = delete;
  AssistantIslandRing& operator=(const AssistantIslandRing&) = delete;
  ~AssistantIslandRing() override;

  void SetMode(Mode mode);
  void SetRadius(float radius);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;

 private:
  class Painter;

  void UpdateBusy();
  void LayoutBusy();

  Mode mode_ = Mode::kNone;
  float radius_ = 26;
  std::unique_ptr<Painter> mask_painter_;
  std::unique_ptr<Painter> sweep_painter_;
  std::unique_ptr<ui::LayerTextured> mask_;
  // 转动的是 pivot_（原点在玻璃中心，只有旋转、没有平移，合成器上插值不跑偏），sweep_ 是它
  // 里面画着渐变的方块。
  std::unique_ptr<ui::Layer> pivot_;
  std::unique_ptr<ui::LayerTextured> sweep_;
  LayerLoop spin_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_ISLAND_RING_H_
