// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/frame/window_drag.h"

#include "ui/base/class_property.h"
#include "ui/gfx/geometry/point.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/controls/scrollbar/scroll_bar.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace starry {

namespace {

// 属性的值。用 int：ui/base 已经给它注册过属性类型。
constexpr int kUnset = 0;
constexpr int kDraggable = 1;
constexpr int kNotDraggable = 2;

DEFINE_UI_CLASS_PROPERTY_KEY(int, kWindowDragKey, kUnset)

// 自己处理鼠标的控件：它们上面不能拖动窗口。
bool IsInteractive(views::View* view) {
  return views::IsViewClass<views::Button>(view) ||
         views::IsViewClass<views::Textfield>(view) ||
         views::IsViewClass<views::ScrollBar>(view) ||
         views::IsViewClass<views::NativeViewHost>(view);
}

}  // namespace

void SetWindowDraggable(views::View* view, bool draggable) {
  view->SetProperty(kWindowDragKey, draggable ? kDraggable : kNotDraggable);
}

bool IsWindowDraggableAt(views::View* root, const gfx::Point& point) {
  if (!root->GetLocalBounds().Contains(point)) {
    return false;
  }
  for (views::View* view = root->GetEventHandlerForPoint(point); view;
       view = view->parent()) {
    if (IsInteractive(view)) {
      return false;
    }
    switch (view->GetProperty(kWindowDragKey)) {
      case kDraggable:
        return true;
      case kNotDraggable:
        return false;
      default:
        break;
    }
    if (view == root) {
      break;
    }
  }
  return false;
}

}  // namespace starry
