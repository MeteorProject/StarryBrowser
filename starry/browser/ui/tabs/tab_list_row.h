// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TAB_LIST_ROW_H_
#define STARRY_BROWSER_UI_TABS_TAB_LIST_ROW_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/view.h"

namespace views {
class Textfield;
}

namespace starry {

class GlyphButton;
class TabCellDelegate;

// 顶部标签行的固定区里收不下的那些：最后一枚「…」胶囊，点开是它们的卡片。
inline constexpr char kMorePinsId[] = "@more";
// 树状标签页聚焦一组时的面包屑。
inline constexpr char kTreeHoistId[] = "@hoist";

struct TabListRowData {
  enum class Kind {
    kLabel,
    kFolder,
    // 展开的空文件夹下面的落点。
    kEmpty,
    kDivider,
    kNewTab,
    // 顶部固定区的文件夹胶囊，包含溢出入口 kMorePinsId。
    kChip,
    // 常用图块里的文件夹。
    kTile,
    // 树状标签页聚焦一组时顶上的面包屑（kTreeHoistId）：返回、那个标签的图标和标题、
    // 里面几个，点一下回到整棵树。
    kHoist,
  };

  Kind kind = Kind::kFolder;
  // 排在第几个标签之前。
  size_t before_tab = 0;
  // 文件夹的 ID；别的是 @pins、@divider、@tabs，落点是「文件夹 ID/empty」。
  std::string id;
  std::u16string title;
  int depth = 0;
  bool collapsed = false;
  // 里面（含子文件夹）一共多少个标签；小标题上是固定标签的总数。
  int count = 0;
  // 折叠时右侧叠着的前几个网站图标。
  std::vector<ui::ImageModel> icons;
  // 当前标签就在这个文件夹下面显示着：层级线变亮。胶囊上是当前标签在
  // 里面，挂在胶囊后面（两个一起垫一层托盘）。
  bool active = false;
  // 胶囊只显示图标，或图块显示为顶部标签栏样式。
  bool compact = false;
  bool open = false;
  // 文件夹的颜色（只染图标）；没选是空的。
  std::optional<SkColor> tint;
  // 面包屑：上面各层的标题（最外层在前）。宽的时候写上一层，提示里是整条路径。
  std::vector<std::u16string> path;
  // 收着的文件夹（折叠的行、胶囊）里有网页在出声、有智能体在操作：挂在行上。
  bool audible = false;
  bool agent = false;
  // 「新标签页」那一行右边的「✦ 整理」：能整理的标签几个
  // （不到 2 个没有）； organize_hint：标签多了，不悬停也显示、写上个数，平时悬停才有。
  int organize = 0;
  bool organize_hint = false;
};

class TabListRow : public views::View,
                   public views::ContextMenuController,
                   public views::AnimationDelegateViews,
                   public views::TextfieldController {
  METADATA_HEADER(TabListRow, views::View)

 public:
  explicit TabListRow(TabCellDelegate* delegate);
  TabListRow(const TabListRow&) = delete;
  TabListRow& operator=(const TabListRow&) = delete;
  ~TabListRow() override;

  static int HeightFor(TabListRowData::Kind kind);
  // 侧栏的图块下面写名字的那一行多高。
  static constexpr int kTileCaptionHeight = 15;
  static int ChipWidth(const TabListRowData& data);
  // 第 depth 层的行往右缩多少（每层 20，最多缩 kMaxIndentDepth 层）。
  static int IndentFor(int depth);
  static constexpr int kMaxIndentDepth = 3;

  void SetData(const TabListRowData& data, const Theme& theme);
  const TabListRowData& data() const { return data_; }
  // 文件夹图标的中心（本视图的坐标）：放进去时小条缩到这里。
  gfx::Point IconCenter() const;
  // 图块文件夹的图块那一块（不算下面的名字）：卡片从这里长出来。
  gfx::Rect TileRect() const;

  // 调试用：「✦ 整理」在哪（本视图的坐标），不显示时是空的。
  gfx::Rect organize_bounds_for_testing() const { return OrganizeRect(); }

  // 就地改名（只有文件夹）。
  void BeginEdit();
  bool editing() const { return field_ != nullptr; }
  // 调试用：输入 text 后确定；空的是取消。
  void FinishEditForTesting(std::optional<std::u16string> text);

  // 已经从列表里拿掉、正在淡出：不再接鼠标。
  void SetLeaving();
  // 正被拖着（或者在被拖着的文件夹里）：原地变淡。
  void SetLifted(bool lifted);
  // 拖着的东西停在这一行上，松手会放进来。
  void SetDropTarget(bool target);
  LayerMotion& motion() { return motion_; }
  base::WeakPtr<TabListRow> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  void OnFocus() override;
  void OnBlur() override;
  // 一个列表在 Tab 键的顺序里只占一站（同 TabCell）。
  bool IsGroupFocusTraversable() const override;
  views::View* GetSelectedViewForGroup(int group) override;
  std::u16string GetRenderedTooltipText(const gfx::Point& point) const override;

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override;

  // views::AnimationDelegateViews:
  void AnimationProgressed(const gfx::Animation* animation) override;
  void AnimationEnded(const gfx::Animation* animation) override;

  // views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

  // 输入框失去焦点：按输入的名字存。
  void OnFieldBlurred();

 private:
  bool interactive() const;
  void SetHovered(bool hovered);
  void EnsureButtons();
  void UpdateButtons();
  void FinishEdit(bool commit);
  void Activate(const ui::MouseEvent& event);
  // 「✦ 整理」在哪（本视图的坐标）；不显示时是空的。
  gfx::Rect OrganizeRect() const;
  std::u16string OrganizeText() const;
  void ShowMoreMenu();
  int StackWidth() const;
  static int IndicatorWidth(const TabListRowData& data);
  int FoldedTrailingWidth() const;
  void PaintIndicators(gfx::Canvas* canvas, int x, float alpha);
  void PaintFolder(gfx::Canvas* canvas, float hover);
  void PaintChip(gfx::Canvas* canvas, float hover);
  void PaintTile(gfx::Canvas* canvas, float hover);
  void PaintHoist(gfx::Canvas* canvas, float hover);
  void PaintStack(gfx::Canvas* canvas, float alpha);

  const raw_ptr<TabCellDelegate> delegate_;
  TabListRowData data_;
  Theme theme_ = Theme::DarkBase();

  // 小标题上的「新建文件夹」。
  raw_ptr<GlyphButton> new_folder_button_ = nullptr;
  raw_ptr<GlyphButton> add_button_ = nullptr;
  raw_ptr<GlyphButton> more_button_ = nullptr;
  raw_ptr<views::Textfield> field_ = nullptr;
  gfx::Rect title_bounds_;

  bool hovered_ = false;
  // 鼠标在「✦ 整理」上。
  bool organize_hovered_ = false;
  bool pressed_ = false;
  // 中键按下（松开时全部打开）。
  bool middle_pressed_ = false;
  // 按下的位置；按住移动超过 4 就是拖动（只有文件夹能拖）。
  std::optional<gfx::Point> press_location_;
  bool dragging_ = false;
  bool lifted_ = false;
  bool drop_target_ = false;
  bool leaving_ = false;
  bool finishing_edit_ = false;
  gfx::SlideAnimation hover_animation_{this};
  gfx::SlideAnimation drop_animation_{this};
  LayerMotion motion_;

  base::WeakPtrFactory<TabListRow> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TAB_LIST_ROW_H_
