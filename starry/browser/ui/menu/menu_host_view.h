// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MENU_MENU_HOST_VIEW_H_
#define STARRY_BROWSER_UI_MENU_MENU_HOST_VIEW_H_

#include <memory>
#include <optional>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace ui {
class MenuModel;
}

namespace starry {

class MenuCard;

// 打开菜单时的选项。
struct MenuOptions {
  MenuOptions();
  MenuOptions(const MenuOptions&);
  MenuOptions& operator=(const MenuOptions&);
  ~MenuOptions();

  // 有值时菜单挂在这个矩形（MenuHostView 的坐标，通常是按钮）下面、左边对齐，
  // 放不下时放在上面（同系统的弹出式菜单按钮）。没有时从 point 那个角长出来。
  std::optional<gfx::Rect> anchor;
  // 选了以后菜单不关、刷新勾选（用于切换一组开关）；参数是那一项的命令 ID。
  base::RepeatingCallback<bool(int command_id)> keeps_open;
  // 危险的操作：这一项的文字、图标和选中的高亮用危险色；参数是那一项的命令 ID。
  base::RepeatingCallback<bool(int command_id)> destructive;
};

// 自己画的菜单：标签、网页、工具按钮的右键菜单都用它，内容是 Chrome 的 ui::MenuModel
// （标签的 TabMenuModel、网页的 RenderViewContextMenu），执行的也是 Chrome 的命令。
//
// - 画在窗口里（盖在所有东西上面的一层），卡片是实时的 Liquid Glass 加一层比命令栏实一点
//   的面板色、1 的细描边。放不下时贴着窗口边，太长时在卡片里滚动。
// - 每行 28：16 的图标、13 的标题、灰色的快捷键，右边是子菜单的箭头；勾选的项有勾；
//   不可用的用 34% 的 on_surface。选中是一块圆角 8 的强调色浅底，换行时滑过去。
// - 子菜单在父菜单右边（放不下放左边），第一行和那一行对齐，压住 3；鼠标停 0.12 秒打开，
//   斜着移向子菜单时经过别的行不切换。
// - ↑↓ 选择（跳过分隔线、小标题和不可用的，到头绕回），→ 打开子菜单，← 收起，回车、
//   空格选中，打字跳到开头相同的项，Esc 关闭。点菜单外面关闭。
// - 打开时从鼠标那个角由 0.95 放大、0.09 秒淡入；选中时高亮闪一下，菜单 0.12 秒淡出，
//   动作马上执行。
class MenuHostView : public views::View, public views::WidgetObserver {
  METADATA_HEADER(MenuHostView, views::View)

 public:
  MenuHostView();
  MenuHostView(const MenuHostView&) = delete;
  MenuHostView& operator=(const MenuHostView&) = delete;
  ~MenuHostView() override;

  // 在 point（本视图的坐标，通常是鼠标处）打开 model 的右键菜单。model 要活到 closed
  // 被调用；关掉（选了一项、点了外面、Esc）时调用 closed。
  void ShowMenuAt(ui::MenuModel* model,
                  const gfx::Point& point,
                  base::OnceClosure closed,
                  MenuOptions options = MenuOptions());
  // 关掉菜单，不执行任何一项。
  void Close();
  // 网页销毁时用：窗口还在、显示的仍是这个根菜单时才关闭。
  base::OnceClosure GetCloseCallbackFor(ui::MenuModel* model);
  bool is_showing() const { return !cards_.empty(); }
  void SetTheme(const Theme& theme);

  // 调试用：当前菜单各行的标题（第 depth 层）。
  std::vector<std::u16string> ItemTitlesForTesting(size_t depth) const;
  void SelectForTesting(size_t depth, size_t row);
  void ActivateSelectedForTesting();

  // views::View:
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  bool SkipDefaultKeyEventProcessing(const ui::KeyEvent& event) override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;

  // views::WidgetObserver:
  // 窗口关掉时菜单还开着：在任何视图拆掉之前关菜单、通知模型。到析构时再通知就晚了：
  // 菜单的模型常常归侧栏这些视图所有，它们比菜单层先拆。
  void OnWidgetDestroying(views::Widget* widget) override;

 private:
  struct Hit {
    size_t depth = 0;
    std::optional<size_t> row;
  };

  std::optional<Hit> HitAt(const gfx::Point& point) const;
  void Hover(const gfx::Point& point);
  // 第 depth 层选中 row（nullopt 是不选），按需安排打开子菜单。
  void Select(size_t depth, std::optional<size_t> row, bool from_keyboard);
  void OpenSubmenu(size_t depth, size_t row, bool select_first);
  void CloseSubmenus(size_t keep_depth);
  void Activate(size_t depth, size_t row);
  void MoveSelection(int delta);
  void TypeAhead(char16_t ch);
  void Finish(bool animated);
  // notify：通知模型关闭、调用 closed（选了一项时由 Activate 自己做）。
  void Finish(bool animated, bool notify);
  void RemoveCards();

  Theme theme_ = Theme::DarkBase();
  std::vector<raw_ptr<MenuCard, VectorExperimental>> cards_;
  base::OnceClosure closed_;
  MenuOptions options_;
  views::ViewTracker previous_focus_;
  // 鼠标上一次的位置：判断是不是正斜着移向子菜单。
  std::optional<gfx::Point> last_mouse_;
  base::OneShotTimer submenu_timer_;
  // 等着切换的行（斜着移向子菜单时先不切换）。
  std::optional<Hit> pending_hover_;
  base::OneShotTimer grace_timer_;
  bool pressed_outside_ = false;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};

  base::WeakPtrFactory<MenuHostView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MENU_MENU_HOST_VIEW_H_
