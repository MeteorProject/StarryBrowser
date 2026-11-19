// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FRAME_WINDOW_BUTTONS_H_
#define STARRY_BROWSER_UI_FRAME_WINDOW_BUTTONS_H_

#include <optional>

#include "ui/gfx/geometry/point.h"

namespace views {
class Widget;
}

namespace starry {

// 摆放窗口左上角的系统按钮（macOS 红绿灯）：origin 为关闭按钮左上角相对窗口左上角的
// 坐标；为空时隐藏（如自动隐藏侧栏收起时，避免浮在网页上）。位置变化时先 0.1s 淡出、
// 移动、再 0.14s 淡入，并真正隐藏按钮以免透明状态下仍响应点击。系统重新排布标题栏
// （如窗口缩放或标题栏高度变化）后自动恢复位置；全屏时不干预。Windows/Linux 上无操作。
void SetWindowButtonsPlacement(views::Widget* widget,
                               std::optional<gfx::Point> origin,
                               bool animated);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FRAME_WINDOW_BUTTONS_H_
