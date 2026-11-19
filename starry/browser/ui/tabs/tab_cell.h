// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TAB_CELL_H_
#define STARRY_BROWSER_UI_TABS_TAB_CELL_H_

#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/frame_motion.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/view.h"

class TabIcon;

namespace gfx {
class Canvas;
}

namespace starry {

class AgentRing;
class GlyphButton;
class MediaWaveform;
class TabCell;
class TabListRow;

// 标签显示形态：侧栏行、侧栏顶部常用图块（仅图标）、顶部标签栏标签、顶部标签栏固定图块。
enum class TabCellStyle { kRow, kTile, kTopRow, kTopTile };

// 标签显示数据：基于 Chrome 的 TabData，内置页面替换为对应图标。
tabs::TabData StarryTabData(tabs::TabInterface* tab);
// 标签上显示的标题：空白页、新标签页（没有标题，或者标题就是网址）是「新标签页」。
std::u16string TabDisplayTitle(const tabs::TabData& data);

// 分屏托盘内标签行的水平缩进。
inline constexpr int kSplitTrayInset = 3;

// 有键盘焦点的标签、行：沿着 rect 的里边描一圈强调色。
void PaintFocusRing(gfx::Canvas* canvas,
                    const Theme& theme,
                    gfx::RectF rect,
                    float radius);

// 标签单元把点击、拖动、右键交给它所在的列表。
class TabCellDelegate {
 public:
  virtual void TabCellClicked(TabCell* cell, bool reset, bool keep_current) = 0;
  virtual void TabCellCloseRequested(TabCell* cell, bool button = false) = 0;
  virtual void TabCellMuteToggled(TabCell* cell) = 0;
  virtual void TabCellHoverChanged(TabCell* cell, bool hovered) {}
  // 树里的父标签：点了图标处的箭头（all：按着 ⌥，连同里面的各层一起开合）。
  virtual void TabCellTreeToggled(TabCell* cell, bool all) {}
  // 点了标签行右边「放进文件夹」的提示。
  virtual void TabCellSuggestionAccepted(TabCell* cell) {}
  virtual void TabFolderAction(const std::string& id,
                               std::optional<gfx::Point> menu_point) = 0;
  // 文件夹行上的「添加当前标签页」。
  virtual void TabFolderAddCurrent(const std::string& id) {}
  // 中键点文件夹：全部打开。按住 ⌥ 点：连同里面的文件夹一起展开、折叠。
  virtual void TabFolderOpenAll(const std::string& id) {}
  virtual void TabFolderToggleTree(const std::string& id) {}
  // 列表里的标签、行有键盘焦点时按了方向键、Home、End、Esc：在列表里移动焦点、
  // 开合文件夹。返回处理了没有。
  virtual bool TabListKeyPressed(views::View* item, const ui::KeyEvent& event);
  // 就地改名结束：name 为空是按了 Esc。
  virtual void TabFolderRenameFinished(const std::string& id,
                                       std::optional<std::u16string> name) {}
  virtual void TabListGapHovered(bool hovered) {}
  // 悬停时显示标题和网址的提示（标签预览关着时）。
  virtual bool TabCellShowsTooltip();
  // 按住移动超过 4 DIP 就是拖动，location 在标签自己的坐标里。
  virtual void TabCellDragStarted(TabCell* cell, const gfx::Point& location) = 0;
  virtual void TabCellDragged(TabCell* cell, const gfx::Point& location) = 0;
  virtual void TabCellDragEnded(TabCell* cell, bool cancelled) = 0;
  // 拖文件夹的行，同拖标签；location 在这一行自己的坐标里。
  virtual void TabRowDragStarted(TabListRow* row, const gfx::Point& location) {}
  virtual void TabRowDragged(TabListRow* row, const gfx::Point& location) {}
  virtual void TabRowDragEnded(TabListRow* row, bool cancelled) {}
  virtual void TabCellContextMenu(TabCell* cell,
                                  const gfx::Point& screen_point,
                                  ui::mojom::MenuSourceType source) = 0;

 protected:
  virtual ~TabCellDelegate() = default;
};

// 单个标签单元：
// - 行：[图标] [标题] [弹性空白] [休眠] [摄像头] [声音] [关闭]，间距 10，左 10 右 6。
// - 图块：图标居中，右上角显示单个状态图标（休眠、摄像头/麦克风或声音）。
// 选中高亮由外层列表统一绘制在其下方，以支持标签间平滑滑动。
// 智能体操作该标签时，图标外圈绘制渐变环（AgentRing），并在悬停提示与无障碍名称中
// 说明当前动作。
//
// 自己有一个图层：插入、关闭、挪动的动画都是这个图层的变换和不透明度，在合成器线程上跑。
class TabCell : public views::View,
                public views::ContextMenuController,
                public views::AnimationDelegateViews {
  METADATA_HEADER(TabCell, views::View)

 public:
  TabCell(TabCellStyle style, TabCellDelegate* delegate);
  TabCell(const TabCell&) = delete;
  TabCell& operator=(const TabCell&) = delete;
  ~TabCell() override;

  TabCellStyle style() const { return style_; }
  tabs::TabHandle handle() const { return handle_; }
  const tabs::TabData& data() const { return data_; }
  bool selected() const { return selected_; }
  bool hovered() const { return hovered_; }

  void SetData(tabs::TabHandle handle,
               const tabs::TabData& data,
               bool selected,
               const Theme& theme);
  // 换样子：顶部标签行里散着的固定标签平时只有图标，选中时撑开成带标题的
  // 标签。
  void SetStyle(TabCellStyle style);
  // 在分屏里：行在托盘里左右缩进，图块右下角一个分屏的小标（显示着的那组用强调色）。
  // tray_insets
  // 是行在托盘里左右缩进多少：侧栏两边都缩，标签行只缩一组头尾的外侧。
  void SetSplit(bool in_split, bool active_group);
  void SetTrayInsets(int left, int right);
  // 树状标签页：有子标签时悬停在这一行上，网站图标换成折叠箭头（点它开合）；折着时
  // 右边是里面一共几个标签，行够宽时前面叠着前几个子标签的图标。
  void SetTree(bool parent,
               bool collapsed,
               int count,
               std::vector<ui::ImageModel> icons);
  bool tree_parent() const { return tree_parent_; }
  bool tree_collapsed() const { return tree_collapsed_; }
  // 按着 ⌥ 开始拖的（树里只拖这一个，子标签留下）。
  bool drag_alone() const { return drag_alone_; }
  // 标签自己的区域（去掉托盘的缩进）。
  gfx::Rect ContentRect() const;
  // 网站图标的中心（本视图的坐标）。
  gfx::Point IconCenter() const;
  bool is_tile() const {
    return style_ == TabCellStyle::kTile || style_ == TabCellStyle::kTopTile;
  }
  bool is_top() const {
    return style_ == TabCellStyle::kTopRow || style_ == TabCellStyle::kTopTile;
  }
  bool in_split() const { return in_split_; }
  // 正被拖着：原地变淡。
  void SetLifted(bool lifted);
  // 拖着的标签停在这个图块上，松手会和它合成一个图块文件夹：描一圈边。
  void SetDropTarget(bool target);
  // 已经从列表里拿掉、正在淡出：不再接鼠标。
  void SetLeaving();
  bool leaving() const { return leaving_; }
  // 点标签时图标轻压再回弹。
  void Bounce();
  // 侧栏的行右边「放进文件夹」的提示（按网站的规则，tab_organizer::SuggestFolder）：
  // 文件夹的名字，没有时是空的；调试用的位置。
  const std::u16string& suggestion() const { return suggest_name_; }
  gfx::Rect suggestion_bounds_for_testing() const { return suggest_bounds_; }
  // 智能体在这个标签里的样子（调试）：none、working、idle。
  std::string AgentRingDescription() const;
  // 加载中的转圈往前走一步；返回还要不要接着走。
  bool StepLoadingAnimation(base::TimeDelta elapsed);

  LayerMotion& motion() { return motion_; }
  base::WeakPtr<TabCell> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  std::u16string GetRenderedTooltipText(const gfx::Point& p) const override;
  // 键盘：回车、空格切过去，方向键交给列表（TabListKeyPressed）。一个列表在
  // Tab 键的顺序里只占一站（同一组，停在选中的标签上）。
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  void OnFocus() override;
  void OnBlur() override;
  bool IsGroupFocusTraversable() const override;
  views::View* GetSelectedViewForGroup(int group) override;

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(views::View* source,
                                  const gfx::Point& point,
                                  ui::mojom::MenuSourceType source_type) override;

  // views::AnimationDelegateViews:
  void AnimationProgressed(const gfx::Animation* animation) override;
  void AnimationEnded(const gfx::Animation* animation) override;

 private:
  void SetHovered(bool hovered);
  // 不是图块时才有关闭按钮（换成标签的样子时才建）。
  void EnsureCloseButton();
  // 关闭按钮在不在（悬停或选中时）。
  bool ClosePresent() const;
  void UpdateButtons();
  void UpdateMeter();
  void LayoutMeter();
  void LayoutRow();
  void LayoutTile();
  bool ShowsCapture() const;
  bool ShowsAudio() const;
  bool Muted() const;
  // 智能体的状态变了（agent::AgentActivities）：渐变环、辅助功能的名字。
  void UpdateAgent();
  void UpdateAccessibleName();
  // 父标签悬停时网站图标淡出（箭头在 OnPaint 里淡入）。
  void UpdateIconFade();
  // 图标那一块（往外放一点，好点）。
  gfx::Rect TreeToggleBounds() const;
  void PaintTree(gfx::Canvas* canvas);

  TabCellStyle style_;
  const raw_ptr<TabCellDelegate> delegate_;
  tabs::TabHandle handle_;
  tabs::TabData data_;
  bool selected_ = false;
  Theme theme_ = Theme::DarkBase();

  // 图标放在一个容器里：回弹时只给容器建图层。
  raw_ptr<views::View> icon_holder_ = nullptr;
  raw_ptr<TabIcon> icon_ = nullptr;
  raw_ptr<GlyphButton> audio_button_ = nullptr;
  // 在放声音时代替喇叭的电平条（第一次有声音时建）。
  raw_ptr<MediaWaveform> meter_ = nullptr;
  raw_ptr<GlyphButton> close_button_ = nullptr;
  // 智能体第一次操作这个标签时才建：大多数标签一直用不到它。
  raw_ptr<AgentRing> agent_ring_ = nullptr;
  // 智能体在做什么（「Claude Code：点击「登录」」），没有智能体时为空。
  std::u16string agent_status_;
  base::CallbackListSubscription agent_subscription_;
  PrefChangeRegistrar media_prefs_;
  gfx::Rect title_bounds_;
  gfx::Rect sleep_bounds_;
  // 「放进文件夹」的提示：文件夹的名字、颜色（没选是空的）、位置，鼠标在不在上面。
  std::u16string suggest_name_;
  std::optional<SkColor> suggest_tint_;
  gfx::Rect suggest_bounds_;
  bool suggest_hovered_ = false;
  bool suggest_pressed_ = false;
  gfx::Rect capture_bounds_;

  bool hovered_ = false;
  bool in_split_ = false;
  bool split_active_ = false;
  int tray_left_ = 0;
  int tray_right_ = 0;
  bool lifted_ = false;
  bool drop_target_ = false;
  bool leaving_ = false;
  bool tree_parent_ = false;
  bool tree_collapsed_ = false;
  int tree_count_ = 0;
  std::vector<ui::ImageModel> tree_icons_;
  gfx::Rect tree_count_bounds_;
  gfx::Rect tree_stack_bounds_;
  // 开合的箭头转到哪了（0 展开，1 折着）、折着时数字跳一下、悬停的父标签网站图标
  // 缩到多大。
  FrameMotion tree_fold_{
      0, base::BindRepeating(&TabCell::SchedulePaint, base::Unretained(this))};
  FrameMotion tree_bump_{
      0, base::BindRepeating(&TabCell::SchedulePaint, base::Unretained(this))};
  float icon_scale_ = 1;
  std::optional<gfx::Point> press_location_;
  bool dragging_ = false;
  bool drag_alone_ = false;
  // 悬停时底色与关闭按钮的淡入淡出动画。
  gfx::SlideAnimation hover_animation_{this};
  gfx::SlideAnimation drop_animation_{this};
  LayerMotion motion_;
  LayerMotion bounce_motion_;

  base::WeakPtrFactory<TabCell> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TAB_CELL_H_
