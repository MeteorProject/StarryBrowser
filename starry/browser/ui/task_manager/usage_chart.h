// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TASK_MANAGER_USAGE_CHART_H_
#define STARRY_BROWSER_UI_TASK_MANAGER_USAGE_CHART_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/task_manager/task_manager_model.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/view.h"

namespace gfx {
class Canvas;
}

namespace starry {

// 数值怎么写：CPU、GPU 是「12.3%」，内存是「1.2 GB」；没有时是「—」。
using UsageFormat = std::u16string (*)(double value);
std::u16string FormatPercent(double value);
std::u16string FormatMemory(double bytes);
std::u16string FormatOptional(std::optional<double> value, UsageFormat format);

// 曲线几何布局：最新读数位于右端，相邻读数间距按总容量均分宽度（样本不足时从右端起画），
// 纵轴从零开始且量程下限为 floor（避免空闲抖动被放大）。绘制与命中测试共用同一份计算。
class SparklineLayout {
 public:
  SparklineLayout(const UsageHistory& history,
                  double floor,
                  const gfx::RectF& bounds,
                  float marker_size);

  const gfx::RectF& plot() const { return plot_; }
  gfx::PointF Point(size_t index, double value) const;
  // 离 x 最近的读数；在最早的读数左边（还没有读数的地方）为空。
  std::optional<size_t> IndexNearest(float x) const;

 private:
  size_t count_;
  float step_;
  gfx::RectF plot_;
  double upper_;
};

// 趋势折线图：2 DIP 折线、10% 透明填充区、末端带外圈圆点及底部淡基线。ring 为圆点外圈
// 颜色（通常取背景色）；为空时不绘制圆点（用于底栏迷你曲线）。highlighted 指定鼠标悬停
// 对应的采样点，绘制参考竖线与高亮圆点。
inline constexpr float kSparklineMarkerSize = 8;
void PaintSparkline(gfx::Canvas* canvas,
                    const gfx::RectF& bounds,
                    const UsageHistory& history,
                    double floor,
                    std::optional<size_t> highlighted,
                    std::optional<SkColor> ring,
                    const Theme& theme);

// 概览指标卡片：展示标题、当前值及最近 2 分钟的趋势曲线。鼠标悬停曲线时切换显示对应时刻
// 的读数和「N 秒前」。
class UsageTile : public views::View {
  METADATA_HEADER(UsageTile, views::View)

 public:
  UsageTile(std::u16string title, double floor, UsageFormat format);
  UsageTile(const UsageTile&) = delete;
  UsageTile& operator=(const UsageTile&) = delete;
  ~UsageTile() override;

  // context：右上角的补充说明，没有时显示 2 分钟内的峰值。unavailable：读不到这项数据时
  // 的说明，代替曲线。
  void Update(const UsageHistory& history,
              std::optional<std::u16string> context,
              std::optional<std::u16string> unavailable);
  void SetTheme(const Theme& theme);
  std::optional<size_t> hovered() const { return hovered_; }

  // 卡片高度：上下内边距各 10、标题行、数值及 4 + 30 的曲线区。
  static int PreferredHeight();

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void OnPaint(gfx::Canvas* canvas) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;

  // 调试用：当作鼠标停在曲线上第 index 个读数（空的是移开）。
  void HoverForTesting(std::optional<size_t> index);
  std::u16string ValueText() const;
  std::u16string CaptionText() const;

 private:
  gfx::RectF ChartRect() const;
  void UpdateAccessibility();

  const std::u16string title_;
  const double floor_;
  const UsageFormat format_;
  Theme theme_ = Theme::DarkBase();
  UsageHistory history_;
  std::optional<std::u16string> context_;
  std::optional<std::u16string> unavailable_;
  std::optional<size_t> hovered_;
};

// 底栏中选中进程的迷你趋势图：标题加无圆点曲线，峰值通过悬停提示展示。
class MiniTrend : public views::View {
  METADATA_HEADER(MiniTrend, views::View)

 public:
  MiniTrend(std::u16string title, double floor, UsageFormat format);
  MiniTrend(const MiniTrend&) = delete;
  MiniTrend& operator=(const MiniTrend&) = delete;
  ~MiniTrend() override;

  void Update(const UsageHistory& history);
  void SetTheme(const Theme& theme);

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  const std::u16string title_;
  const double floor_;
  const UsageFormat format_;
  Theme theme_ = Theme::DarkBase();
  UsageHistory history_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TASK_MANAGER_USAGE_CHART_H_
