// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FRAME_WINDOW_DRAG_H_
#define STARRY_BROWSER_UI_FRAME_WINDOW_DRAG_H_

namespace gfx {
class Point;
}

namespace views {
class View;
}

namespace starry {

// 窗口没有标题栏，界面的空白处用来拖动窗口（双击缩放等也走这里）。
//
// 给视图标上「空白处可以拖动窗口」（true）或者「这里不行」（false）；没标的跟着父视图。
// 按钮、输入框、滚动条和网页不用标，它们总是自己处理鼠标。
void SetWindowDraggable(views::View* view, bool draggable);

// `point`（root 的坐标）处能不能拖动窗口：找到那里最上面的视图，往上找到第一个标过的。
bool IsWindowDraggableAt(views::View* root, const gfx::Point& point);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FRAME_WINDOW_DRAG_H_
