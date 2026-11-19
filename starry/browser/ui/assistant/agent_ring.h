// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_AGENT_RING_H_
#define STARRY_BROWSER_UI_ASSISTANT_AGENT_RING_H_

#include <memory>
#include <optional>

#include "starry/browser/ui/motion/layer_loop.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"

namespace ui {
class Layer;
class LayerTextured;
}  // namespace ui

namespace starry {

// 智能体在操作这个标签：图标外一圈紫到天蓝的渐变环（和网页卡片外圈的描边同色），
// 1.6 的线，离图标 3.5。做事时匀速转动（减弱动态效果时不转），等下一步时停住、淡到
// 0.6。在后台操作的标签用户看不到网页上的光标和提示条，靠它知道哪些标签在智能体手里。
// 侧栏的行、顶部的标签、固定标签的图块和后台标签的小卡片都用它。
//
// 转的是合成器上的图层变换（同岛的外圈 AssistantIslandRing）：一个不画东西的图层原点在
// 圆心、只有旋转，画着渐变环的图层挂在它下面，不重画、不占主线程（LayerLoop）。不接鼠标。
class AgentRing : public views::View {
  METADATA_HEADER(AgentRing, views::View)

 public:
  // 环和图标之间留出的距离（视图比图标每边大这么多，环画在里面）。
  static constexpr int kOutset = 4;
  // 环在图标外的位置。
  static gfx::Rect BoundsAround(const gfx::Rect& icon);

  AgentRing();
  AgentRing(const AgentRing&) = delete;
  AgentRing& operator=(const AgentRing&) = delete;
  ~AgentRing() override;

  // working：nullopt 是没有智能体（不显示），true 在做事（转），false 在等下一步。
  void SetWorking(std::optional<bool> working);
  std::optional<bool> working() const { return working_; }
  // 调试：在不在转。
  bool spinning() const { return spinning_; }

  // views::View:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;

 private:
  class Painter;

  void LayoutRing();
  void Spin(bool spin);

  std::optional<bool> working_;
  bool spinning_ = false;
  std::unique_ptr<Painter> painter_;
  // 转动的是 pivot_（原点在圆心），ring_ 是它下面画着渐变环的图层。
  std::unique_ptr<ui::Layer> pivot_;
  std::unique_ptr<ui::LayerTextured> ring_;
  LayerLoop spin_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_AGENT_RING_H_
