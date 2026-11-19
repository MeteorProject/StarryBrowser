// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SPLIT_SPLIT_OVERLAY_H_
#define STARRY_BROWSER_UI_SPLIT_SPLIT_OVERLAY_H_

#include <array>
#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/split_tabs/split_tab_id.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/split/split_tabs.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/view_targeter_delegate.h"

namespace ui {
class SimpleMenuModel;
}

namespace starry {

class StarryBrowserView;

namespace split_ui {
class CollapseHint;
class Divider;
class DropPreview;
class FocusRing;
class PaneHandle;
class PaneMenu;
}  // namespace split_ui

// 内容区上方的分屏交互与装饰层：
// - 当前格描边：一圈强调色描边，切换当前格时平滑滑动。
// - 分隔线：两格之间的空隙。悬停时光标变为调整大小并显示胶囊；拖动时调整比例，
//   拖过最小尺寸产生阻力，继续拖过 split::kCollapseDistance 胶囊变为强调色且该格
//   显示「松开以移出分屏」，松手即移出；双击平分。
// - 顶部把手：平时为居中细胶囊，悬停展开为带图标和标题的玻璃小条；点击或右键打开
//   该格菜单；按住可拖到另一格边缘换位，拖出内容区即移出分屏。
// - 拖放预览：拖标签到内容区时压暗内容区并按落点预览布局；拖入项以强调色标出，
//   超过上限时提示原因。
//
// 分屏格本身由 Chrome 的 MultiContentsView 中的最多三个 ContentsContainerView 承载，
// 原生描边、工具条与内边距通过 OverrideSplitPaneDecorations 关闭，本层只叠在上方。
// 整层铺满窗口，仅分隔线和把手响应鼠标事件。
class SplitOverlay : public views::View,
                     public views::ViewTargeterDelegate,
                     public views::ViewObserver,
                     public TabStripModelObserver {
  METADATA_HEADER(SplitOverlay, views::View)

 public:
  explicit SplitOverlay(StarryBrowserView* browser_view);
  SplitOverlay(const SplitOverlay&) = delete;
  SplitOverlay& operator=(const SplitOverlay&) = delete;
  ~SplitOverlay() override;

  void SetTheme(const Theme& theme);
  const Theme& theme() const { return theme_; }

  // 窗口排好之后、两格挪了之后：分隔线、把手、描边、窗口底上的卡片跟过去。
  void UpdateGeometry();

  // 侧栏的标签、分屏的把手拖着一个标签在窗口里移动（point 在本视图的坐标）：在内容区上时
  // 预览松手以后的样子。返回在不在内容区上。
  bool DragTabTo(tabs::TabInterface* tab, const gfx::Point& point);
  // 拖动结束：commit 且预览可以分屏时分屏，返回分了没有。
  bool EndTabDrag(bool commit);
  bool is_previewing_drop() const { return drop_.has_value(); }

  // 当前分屏各格的位置（本视图的坐标）；不在分屏时为空。
  gfx::Rect PaneBounds(int index) const;
  gfx::Rect DividerBounds(int index = 0) const;
  gfx::Rect HandleBounds(int index) const;
  // 第 index 格的菜单：at 为空时挂在把手下面。
  void ShowPaneMenu(int index, std::optional<gfx::Point> at);
  // 调试：打印分屏的状态。
  std::string DebugDescription() const;

  // 分隔线的拖动（split_ui::Divider 调用）。
  void BeginResize(int index = 0);
  void Resize(int translation);
  void EndResize(bool commit);
  void Equalize();
  // 把手的拖动（split_ui::PaneHandle 调用），point 在本视图的坐标。
  void PaneDragged(int index, const gfx::Point& point);
  void PaneDragEnded(int index, const gfx::Point& point, bool cancelled);

  // views::View:
  void OnThemeChanged() override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewVisibilityChanged(views::View* observed_view,
                               views::View* starting_view,
                               bool visible) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;
  void OnSplitTabChanged(const SplitTabChange& change) override;

 private:
  struct Pane {
    gfx::Rect bounds;
    tabs::TabHandle tab;
  };
  struct ResizeState {
    explicit ResizeState(split_tabs::SplitTabId id);
    ResizeState(const ResizeState&);
    ResizeState& operator=(const ResizeState&);
    ~ResizeState();
    split_tabs::SplitTabId split;
    bool side_by_side = true;
    split::Layout start_layout;
    split::DividerGeometry divider;
    int divider_index = 0;
    double available = 0;
    double leading = 0;
    double pair = 0;
    // 拖过了头：松手时移出分屏的叶子序号（嵌套一侧可能有两格）。
    std::vector<int> collapsing;
  };

  void ObserveContainers();
  // 内容区（MultiContentsView）在本视图里的位置。
  gfx::Rect CardBounds() const;
  // point 下面是哪一格（不在分屏时是整个内容区）。
  std::optional<Pane> PaneAt(const gfx::Point& point) const;
  void UpdateHandles();
  void SetCollapsing(const std::vector<int>& indices);

  const raw_ptr<StarryBrowserView> browser_view_;
  Theme theme_ = Theme::DarkBase();

  raw_ptr<split_ui::FocusRing> ring_ = nullptr;
  std::array<raw_ptr<split_ui::Divider>, split::kMaxPanes - 1> dividers_{};
  std::vector<split::DividerGeometry> divider_geometry_;
  std::array<raw_ptr<split_ui::PaneHandle>, split::kMaxPanes> handles_{};
  raw_ptr<split_ui::CollapseHint> hint_ = nullptr;
  raw_ptr<split_ui::DropPreview> preview_ = nullptr;

  std::optional<std::vector<Pane>> panes_;
  // 描边现在圈着哪个标签：换了才滑过去，窗口变大小时直接跟上。
  std::optional<tabs::TabHandle> ring_tab_;
  split_tabs::SplitTabLayout layout_ = split_tabs::SplitTabLayout::kSideBySide;
  std::optional<ResizeState> resize_;
  std::optional<split::Drop> drop_;
  // 把手的菜单：开着时模型要活着，到下一次打开才换。
  std::unique_ptr<split_ui::PaneMenu> menu_;

  base::ScopedMultiSourceObservation<views::View, views::ViewObserver>
      container_observations_{this};
  base::WeakPtrFactory<SplitOverlay> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SPLIT_SPLIT_OVERLAY_H_
