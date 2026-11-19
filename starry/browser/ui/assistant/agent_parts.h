// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_AGENT_PARTS_H_
#define STARRY_BROWSER_UI_ASSISTANT_AGENT_PARTS_H_

#include <string>

#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_loop.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view.h"

namespace gfx {
class Canvas;
}

// 智能体的提示共用的颜色和小部件：网页上的提示（agent_overlay.h、agent_marks.h）、后台
// 标签的小卡片（agent_background_tasks.h）、标签上的渐变环（agent_ring.h）。
namespace starry {

// 智能体的颜色：紫到天蓝，和网页、浏览器自己的强调色（跟着网页变）都区分得开；
// 在智能体的临时无痕窗口（紫色主题）里，天蓝一端仍然显眼。
inline constexpr SkColor kAgentViolet = SkColorSetRGB(0x7C, 0x5C, 0xFF);
inline constexpr SkColor kAgentSky = SkColorSetRGB(0x38, 0xBD, 0xF8);

// 从 rect 的左上到右下，紫到天蓝。
cc::PaintFlags AgentGradient(const gfx::RectF& rect);
cc::PaintFlags AgentGradientStroke(const gfx::RectF& rect, float width);
// 绕 center 一圈的锥形渐变（紫、天蓝、淡紫、天蓝、紫）。faint 是淡紫那一段的
// 不透明度：网页的描边 0.7，标签的渐变环 0.35。
cc::PaintFlags AgentRingGradient(const gfx::PointF& center, float faint);

// 玻璃上面的底：一层面板色和细边，字盖在网页上也读得清；外面一圈渐变细线，做事时
// 0.8，没在做时 0.4。提示条和后台的小卡片用。
void PaintAgentGlassBody(gfx::Canvas* canvas,
                         const Theme& theme,
                         const gfx::RectF& bounds,
                         float radius,
                         bool working);

// 智能体的星光：渐变的 ✦，做事时在合成器上一明一暗地跳（最暗 0.35）。
// 大小是视图的宽。不接鼠标。
class AgentSparkleView : public views::View {
  METADATA_HEADER(AgentSparkleView, views::View)

 public:
  AgentSparkleView();
  AgentSparkleView(const AgentSparkleView&) = delete;
  AgentSparkleView& operator=(const AgentSparkleView&) = delete;
  ~AgentSparkleView() override;

  void SetPulsing(bool pulsing);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  bool pulsing_ = false;
  LayerLoop pulse_;
};

// 「停止」按钮（提示条、后台小卡片用）：浅底的胶囊，高 26、左右 11，12 号中等字重。
class AgentStopButton : public views::Button {
  METADATA_HEADER(AgentStopButton, views::Button)

 public:
  static constexpr int kHeight = 26;

  explicit AgentStopButton(PressedCallback callback);
  AgentStopButton(const AgentStopButton&) = delete;
  AgentStopButton& operator=(const AgentStopButton&) = delete;
  ~AgentStopButton() override;

  // 按文字算的宽度。
  static int PreferredWidth();

  void SetTheme(const Theme& theme);

  // views::Button:
  void PaintButtonContents(gfx::Canvas* canvas) override;

 private:
  Theme theme_ = Theme::DarkBase();
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_AGENT_PARTS_H_
