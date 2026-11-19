// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TREE_STICKY_HEADER_H_
#define STARRY_BROWSER_UI_TABS_TREE_STICKY_HEADER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/frame_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/views/view.h"

namespace starry {

// 树状标签页的父标签吸顶：在长子树中向下滚动时，将最上方可见行的最近两层祖先固定在
// 列表顶部。每行压成 28 高、字是次要色，缩进同它在列表里的样子，下面一道淡线；点击
// 滚回该行。切换祖先组时交叉淡化。
//
// 自己没有底色（侧栏是玻璃）：TabsView 给滚动区域加一层渐变遮罩，让滚过去的行在
// 它下面消失（VisibleHeight）。
class TreeStickyHeader : public views::View {
  METADATA_HEADER(TreeStickyHeader, views::View)

 public:
  struct Row {
    tabs::TabHandle handle;
    ui::ImageModel icon;
    std::u16string title;
    // 往右缩多少（同它在列表里的缩进）。
    int indent = 0;
  };
  static constexpr int kRowHeight = 28;

  // clicked：点了哪一行；changed：显示的高度变了（交叉淡化的每一帧也算）。
  TreeStickyHeader(base::RepeatingCallback<void(tabs::TabHandle)> clicked,
                   base::RepeatingClosure changed);
  TreeStickyHeader(const TreeStickyHeader&) = delete;
  TreeStickyHeader& operator=(const TreeStickyHeader&) = delete;
  ~TreeStickyHeader() override;

  void SetRows(std::vector<Row> rows, const Theme& theme);
  const std::vector<Row>& rows() const { return rows_; }
  // 下面的行要让出来的高度：交叉淡化时从旧的过渡到新的。
  float VisibleHeight() const;
  // 放下所有行要多高（交叉淡化时取新旧两组里高的）。
  int PreferredHeight() const;

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  std::u16string GetRenderedTooltipText(const gfx::Point& point) const override;

 private:
  void PaintRows(gfx::Canvas* canvas, const std::vector<Row>& rows);
  std::optional<size_t> RowAt(const gfx::Point& point) const;
  void OnStep();

  const base::RepeatingCallback<void(tabs::TabHandle)> clicked_;
  const base::RepeatingClosure changed_;
  Theme theme_ = Theme::DarkBase();
  std::vector<Row> rows_;
  // 交叉淡化时淡出的那一组。
  std::vector<Row> old_rows_;
  std::optional<size_t> hovered_;
  std::optional<size_t> pressed_;
  // 0 是旧的那一组，1 是新的。
  FrameMotion fade_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TREE_STICKY_HEADER_H_
