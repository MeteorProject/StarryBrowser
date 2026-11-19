// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_AGENT_MARKS_H_
#define STARRY_BROWSER_UI_ASSISTANT_AGENT_MARKS_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/view.h"

namespace starry {

namespace agent {
struct AgentActivity;
}

class AgentCursor;
class AgentEdgeGlow;
class AgentHighlightBox;
class AgentScanSweep;

// 智能体在用户看着的一格网页上画的东西，除了提示条（在 AgentOverlay 里）：
// - 外圈描边：卡片里贴着边一圈宽而淡的光晕和一圈清楚的渐变线。正在做一步时呼吸，
//   等下一步时停在较淡的一档。
// - 扫描光带：读页面（快照、正文、截图）时一道光带从上往下扫过一次。
// - 高亮框：要点击、输入、选择的元素外面一圈圆角渐变框，输入时边粗一点；换到另一个
//   元素时滑过去。
// - 光标：渐变的箭头，旁边是客户端名字的小胶囊。第一次出现时从底部提示条那里出发，之后
//   平滑移到要操作的元素上；点下时箭头轻压回弹、尖上一圈波纹扩散淡出；拖动时压低一点、
//   尖上一个半透明的圆点，按住元素滑到目标。
// 减弱动态效果时光标直接到位，没有波纹和扫描光带，描边不呼吸。
//
// 铺满这一格（内容卡片的圆角裁掉外面的），全是图层，不接鼠标。坐标都从 AgentActivity 里
// 网页的 CSS 像素换算过来：网页的坐标变了才动画，只是这一格挪了（窗口变大小、侧栏在动）
// 时直接跟上。
class AgentPaneMarks : public views::View {
  METADATA_HEADER(AgentPaneMarks, views::View)

 public:
  AgentPaneMarks();
  AgentPaneMarks(const AgentPaneMarks&) = delete;
  AgentPaneMarks& operator=(const AgentPaneMarks&) = delete;
  ~AgentPaneMarks() override;

  // card：这一格（父视图的坐标）。page_origin：网页坐标的原点在这一格里的位置，zoom：
  // 网页的缩放（CSS 像素 × zoom = 界面的 DIP）。client：光标旁的名字。
  void Update(const agent::AgentActivity& activity,
              const gfx::Rect& card,
              const gfx::PointF& page_origin,
              float zoom);
  // 这一格不再有智能体（断开、换了标签）：淡出，放完调用 done（不在动画回调里删自己）。
  void Leave(base::OnceClosure done);
  bool leaving() const { return leaving_; }

  // 调试：描边、光标、高亮框、扫描。坐标是这一格里的。
  std::string DebugDescription() const;

 private:
  // 网页坐标换成这一格里的。
  gfx::PointF ToLocal(const gfx::PointF& page) const;
  gfx::RectF ToLocal(const gfx::RectF& page) const;

  raw_ptr<AgentScanSweep> scan_ = nullptr;
  raw_ptr<AgentHighlightBox> highlight_ = nullptr;
  raw_ptr<AgentCursor> cursor_ = nullptr;
  raw_ptr<AgentEdgeGlow> glow_ = nullptr;
  LayerMotion motion_;
  bool leaving_ = false;
  // 第一次 Update：已经有的光标、高亮框直接放好，不从底部出发，不补放扫描。
  bool placed_ = false;
  gfx::PointF page_origin_;
  float zoom_ = 1;
  // 上次看到的（网页的坐标、计数），变了才动画。
  std::optional<gfx::PointF> cursor_page_;
  std::optional<gfx::RectF> target_page_;
  int clicks_ = 0;
  int scans_ = 0;
  bool holding_ = false;
  base::WeakPtrFactory<AgentPaneMarks> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_AGENT_MARKS_H_
