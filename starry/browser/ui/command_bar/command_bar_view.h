// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_COMMAND_BAR_COMMAND_BAR_VIEW_H_
#define STARRY_BROWSER_UI_COMMAND_BAR_COMMAND_BAR_VIEW_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/omnibox/browser/autocomplete_controller.h"
#include "starry/browser/ui/command_bar/command_bar_home.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/range/range.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/view_tracker.h"

class BrowserWindowInterface;
struct AutocompleteMatch;

namespace views {
class Textfield;
}

namespace starry {

class CommandBarPanel;
class CommandBarRow;
class GlassView;

// 命令栏（地址栏）：⌘L 编辑当前网址，⌘T 和侧栏的「新标签页」在新标签打开，
// 以居中浮层形式显示。
//
// 补全使用 Chrome 地址栏的 AutocompleteController（历史、书签、已打开的标签、
// 搜索引擎建议、快捷方式等）；界面自绘：
// - 第一条是回车执行项；默认项支持内联补全时，补全部分以选中文本显示在输入框中。
// - ↑↓、Tab 切换建议时输入框更新为该条内容，Esc 回到输入文本，再按一次关闭；
//   ⇧⌦ 从历史记录中删除选中项。
// - 已打开的标签右侧显示「切换到标签页」。
//
// 覆于整个窗口之上：压暗遮罩（点击关闭）与玻璃面板。面板使用实时 Liquid Glass，
// 仅在命令栏打开时显示。
//
// 动效：打开时遮罩淡入，面板自上方落下、放大、去模糊并淡入；关闭时整体原地淡出。
// 建议变化时面板高度与行位置平滑过渡，新行淡入、移除的行原地淡出（行按网址或搜索词
// 标识）。触发动画的条件见 RebuildRows。
// 卡片风格新标签页通过 OpenDocked 停靠在页面搜索框上（无遮罩、失焦即关）；
// 全屏浮层风格在输入框为空时展示 CommandBarHome。
class CommandBarView : public views::View,
                       public views::TextfieldController,
                       public views::ViewTargeterDelegate,
                       public views::FocusChangeListener,
                       public CommandBarHome::Delegate,
                       public AutocompleteController::Observer {
  METADATA_HEADER(CommandBarView, views::View)

 public:
  enum class Mode {
    // 编辑当前标签的网址（⌘L、点地址栏）。
    kEdit,
    // 在新标签打开（⌘T、「新标签页」）。
    kNewTab,
  };

  explicit CommandBarView(BrowserWindowInterface* browser);
  CommandBarView(const CommandBarView&) = delete;
  CommandBarView& operator=(const CommandBarView&) = delete;
  ~CommandBarView() override;

  void Open(Mode mode);
  // 停靠到卡片风格新标签页的搜索框上；slot 返回本视图坐标（为空时沿用上次位置），
  // drop 控制是否使用下落入场动画，closed 在关闭或切换停靠目标时触发。
  using DockSlot = base::RepeatingCallback<std::optional<gfx::Rect>()>;
  void OpenDocked(DockSlot slot,
                  const std::u16string& text,
                  bool drop,
                  base::OnceClosure closed);
  void DockMoved();
  void FocusField(const std::u16string& text);
  bool docked_open() const { return open_ && !dock_slot_.is_null(); }
  void Close();
  bool is_open() const { return open_; }
  Mode mode() const { return mode_; }
  void SetTheme(const Theme& theme);
  // 面板离窗口顶边的距离（默认 metrics::kCommandBarTopOffset；小窗口矮，靠上一些）。
  void SetTopOffset(int top_offset);

  // 调试用：输入文字（同用户打字），按键。
  void TypeForTesting(const std::u16string& text);
  void PressKeyForTesting(ui::KeyboardCode key, int flags);
  std::u16string FieldTextForTesting() const;
  std::vector<std::u16string> RowTitlesForTesting() const;

  // 行的回调。
  void RowHovered(CommandBarRow* row, bool hovered);
  void RowClicked(CommandBarRow* row);
  void RowRemoveClicked(CommandBarRow* row);

  // views::View:
  void Layout(PassKey) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // views::FocusChangeListener:
  void OnWillChangeFocus(views::View* focused_before,
                         views::View* focused_now) override {}
  void OnDidChangeFocus(views::View* focused_before,
                        views::View* focused_now) override;

  // CommandBarHome::Delegate:
  void HomeOpenUrl(const GURL& url) override;
  void HomeDismiss() override;

  // views::TextfieldController:
  void ContentsChanged(views::Textfield* sender,
                       const std::u16string& new_contents) override;
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

  // AutocompleteController::Observer:
  void OnResultChanged(AutocompleteController* controller,
                       bool default_match_changed) override;

 private:
  void OpenImpl(Mode mode,
                DockSlot slot,
                const std::u16string& text,
                bool drop,
                base::OnceClosure closed);
  void UpdateHome();
  void StopFocusListening();
  // 面板的宽度、离窗口顶边的距离；高度是正在动的那个。
  gfx::Rect PanelBounds() const;
  // 面板最后的高度（按现在的行数）。
  int TargetPanelHeight() const;
  // 按 PanelBounds() 摆玻璃、输入框、分隔线和裁剪建议列表的那一层（高度动的每一帧）。
  void LayoutPanel();
  void StartQuery();
  // 按补全的结果留下、新建、去掉行。行变了并且 allow_animation 时带动画。
  void RebuildRows(bool allow_animation);
  // 行的样子（选中、悬停、主题）变了，行本身不变。
  void RefreshRows();
  // 第 row 行对应的建议（没有时为空）。
  const AutocompleteMatch* RowMatch(size_t row) const;
  // 固定标签的文件夹名字里有输入的字：这些标签补在建议后面（切换到标签页）。
  std::vector<AutocompleteMatch> FolderMatches(
      const std::vector<std::string>& taken) const;
  std::u16string FolderPathFor(const AutocompleteMatch& match) const;
  // 归档的标签：标题、网址里有输入的字的，补在文件夹的后面（选中
  // 就恢复）。ids 是它们在归档里的 ID。
  std::vector<AutocompleteMatch> ArchiveMatches(
      const std::vector<std::string>& taken,
      std::vector<std::string>* ids) const;
  // 第 row 行是归档的标签时是它的 ID，否则为空。
  std::string RowArchiveId(size_t row) const;
  // 打了「整理」这类词、能整理的标签有 2 个以上时的「整理标签页」一行。
  std::optional<AutocompleteMatch> OrganizeMatch() const;
  // 行摆到各自的位置；animate 时挪了位置的行从原来的地方滑过去。
  void PlaceRows(bool animate);
  // 去掉的行在原处淡出，淡完再删。
  void LeaveRow(CommandBarRow* row, const MotionCurve& curve);
  void RowLeft(base::WeakPtr<CommandBarRow> row);
  void DropLeavingRows();
  // 按 selected_ 把输入框的文字写好：选着别的建议时是那一条的文字，否则是输入的文字
  // 加上选中的内联补全。
  void SyncField();
  void Move(int delta);
  void Submit();
  bool RemoveSelected();
  // 打开第 row 行：归档的标签是恢复，别的同 OpenMatch。
  void OpenRow(size_t row);
  void OpenMatch(const AutocompleteMatch& match);
  // 没改过 ⌘L 带进来的网址就回车：按地址栏的规则重新打开它。
  void OpenText(const std::u16string& text);
  void FinishClose();
  void ApplyTheme();

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<AutocompleteController> controller_;
  Mode mode_ = Mode::kEdit;
  bool open_ = false;
  Theme theme_ = Theme::DarkBase();
  int top_offset_;

  // 用户输入的文字，不含内联补全。
  std::u16string text_;
  // 用户动过输入框。之前的文字（⌘L 带进来的网址）不补全、不查。
  bool edited_ = false;
  // 刚删了字、还在输入法里拼字：不内联补全。
  bool prevent_inline_ = false;
  // 键盘选中的一条（0 是默认建议）。
  size_t selected_ = 0;
  std::optional<size_t> hovered_;
  // 正在往输入框里写文字（不是用户输入的）。
  bool syncing_ = false;
  // 上次同步或用户编辑后的文字和选区；用户自己动过光标后不覆盖。
  std::u16string synced_text_;
  gfx::Range synced_selection_;
  // 按下在遮罩上（松开也在遮罩上才关）。
  bool pressed_backdrop_ = false;

  raw_ptr<views::View> backdrop_ = nullptr;
  raw_ptr<CommandBarHome> home_ = nullptr;
  bool home_enabled_ = false;
  DockSlot dock_slot_;
  mutable gfx::Rect dock_rect_;
  base::OnceClosure dock_closed_;
  raw_ptr<views::FocusManager> focus_listening_ = nullptr;
  raw_ptr<GlassView> glass_ = nullptr;
  raw_ptr<CommandBarPanel> panel_ = nullptr;
  raw_ptr<views::Textfield> field_ = nullptr;
  raw_ptr<views::View> divider_ = nullptr;
  // 建议列表：裁到玻璃里（面板变高时下面的行露出来，变矮时被盖住）。
  raw_ptr<views::View> list_ = nullptr;
  std::vector<raw_ptr<CommandBarRow, VectorExperimental>> rows_;
  // 每一行是补全结果里的第几条：打不开的不显示（单独一个「?」时搜索给的
  // 「<输入搜索字词>」占位）。
  std::vector<size_t> shown_;
  // 补在后面的文件夹里的标签、归档的标签（shown_ 里是 kExtraRow 加上这里的下标）；
  // 归档的在 extra_archive_ids_ 里同一个下标处是它的 ID（文件夹里的是空的）。
  std::vector<AutocompleteMatch> extras_;
  std::vector<std::string> extra_archive_ids_;
  // 去掉了、还在淡出的行。
  std::vector<raw_ptr<CommandBarRow, VectorExperimental>> leaving_;
  LayerMotion backdrop_motion_;
  LayerMotion glass_motion_;
  LayerMotion divider_motion_;
  // 面板的高度动画。
  ValueMotion height_;
  // 打开之前有焦点的视图，关掉后还给它。
  views::ViewTracker previous_focus_;
  int close_generation_ = 0;

  base::WeakPtrFactory<CommandBarView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_COMMAND_BAR_COMMAND_BAR_VIEW_H_
