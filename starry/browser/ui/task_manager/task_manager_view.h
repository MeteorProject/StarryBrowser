// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_VIEW_H_
#define STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_VIEW_H_

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/task_manager/task_manager_model.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/view.h"

namespace starry {

class DialogHostView;
class MenuHostView;
class UsageTile;

namespace task_manager_ui {
class Footer;
class HeaderText;
class ProcessList;
class Reveal;
class TableCard;
}  // namespace task_manager_ui

// 任务管理器窗口内容视图：标题与汇总、CPU / 内存 / GPU 概览卡片、可排序进程表格以及
// 底部选中进程操作栏；右键菜单与确认框均采用 Starry 内置弹层。
//
// 动效：
// - 打开时标题、汇总淡入，卡片和表格上移 10 淡入（Motion.reveal，依次晚 0.01 秒），第一批
//   行逐行出现（Motion.staggerStep）。
// - 进程增减、刷新后换了顺序、点表头排序：行滑到新位置，新的一行从上面 4 淡入，没了的
//   淡出（Motion.listEdit）。
// - 选中的底色滑到另一行，高度一起过渡到那一行的高度（Motion.selection）；选中的行换位置、
//   多了少了子项时跟着行动（Motion.listEdit）。悬停的浅底淡入淡出（Motion.hover）。
// - 表头：当前那一列变亮，箭头淡入、升降序时转半圈（Motion.hover）；按下缩到 0.97。
// - 数字每次刷新直接换上，不做过渡。
class TaskManagerView : public views::View {
  METADATA_HEADER(TaskManagerView, views::View)

 public:
  TaskManagerView();
  TaskManagerView(const TaskManagerView&) = delete;
  TaskManagerView& operator=(const TaskManagerView&) = delete;
  ~TaskManagerView() override;

  // height：和红绿灯同一行的高度（macOS 的标题栏；别的平台没有调用，系统画了标题栏，
  // 标题和汇总单独一行）。inset：标题从这里开始（红绿灯右边）。
  void SetTitlebar(int height, int inset);
  // 打开时键盘在表格上。
  views::View* focus_target();

  TaskManagerModel& model() { return *model_; }
  MenuHostView* menu_host() { return menu_host_; }
  DialogHostView* dialog_host() { return dialog_host_; }
  const Theme& theme() const { return theme_; }

  // 选中、打开（双击、回车）、结束（先问一下）、右键菜单。
  void Select(std::optional<base::ProcessId> pid);
  void Open(base::ProcessId pid);
  void RequestEnd(base::ProcessId pid);
  void ShowProcessMenu(base::ProcessId pid, const gfx::Point& point_in_view);
  // 点了一行：键盘交给表格（↑↓、回车、删除键）。
  void FocusTable();
  // 键盘：↑↓、回车、删除键。处理了返回 true。
  bool HandleKey(const ui::KeyEvent& event);

  // 状态查询与自动化辅助接口。
  std::string DebugDescription() const;
  void SortForTesting(TaskManagerModel::Column column);
  void HoverTileForTesting(size_t tile, std::optional<size_t> index);

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  void OnThemeChanged() override;
  void AddedToWidget() override;
  gfx::Size GetMinimumSize() const override;

 private:
  void OnModelChanged();
  void UpdateHeader();
  void UpdateTiles();
  void ApplyTheme();
  void ExecuteMenuCommand(base::ProcessId pid, int command);

  int titlebar_height_ = 0;
  int title_inset_ = 0;
  Theme theme_ = Theme::DarkBase();
  bool theme_applied_ = false;
  std::unique_ptr<TaskManagerModel> model_;

  raw_ptr<views::View> header_ = nullptr;
  raw_ptr<task_manager_ui::HeaderText> title_ = nullptr;
  raw_ptr<task_manager_ui::HeaderText> summary_ = nullptr;
  std::array<raw_ptr<UsageTile>, 3> tiles_{};
  raw_ptr<views::View> overview_ = nullptr;
  raw_ptr<task_manager_ui::TableCard> table_ = nullptr;
  raw_ptr<task_manager_ui::Footer> footer_ = nullptr;
  raw_ptr<MenuHostView> menu_host_ = nullptr;
  raw_ptr<DialogHostView> dialog_host_ = nullptr;

  // 打开时的淡入：标题、汇总、卡片、表格。
  std::array<std::unique_ptr<task_manager_ui::Reveal>, 4> reveals_;
  bool revealed_ = false;
  // 第一批行已经放进表格（之后的变化用 Motion.listEdit）。
  bool rows_synced_ = false;
  base::TimeTicks appeared_at_;

  std::unique_ptr<ui::SimpleMenuModel> menu_model_;
  std::unique_ptr<ui::SimpleMenuModel::Delegate> menu_delegate_;

  base::WeakPtrFactory<TaskManagerView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_VIEW_H_
