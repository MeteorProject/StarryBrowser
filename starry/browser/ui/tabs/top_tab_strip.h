// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TOP_TAB_STRIP_H_
#define STARRY_BROWSER_UI_TABS_TOP_TAB_STRIP_H_

#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "starry/browser/ui/tabs/tabs_view.h"
#include "ui/base/metadata/metadata_header_macros.h"

class BrowserWindowInterface;

namespace starry {

class AssistantButton;
class IconButton;
class MediaCapsule;
class PillButton;
class ToolbarSeparator;

// 顶部标签栏。普通标签共享剩余宽度并可横向滚动，「新标签页」按钮始终可见。
class TopTabStrip : public TabsView {
  METADATA_HEADER(TopTabStrip, TabsView)

 public:
  explicit TopTabStrip(BrowserWindowInterface* browser);
  TopTabStrip(const TopTabStrip&) = delete;
  TopTabStrip& operator=(const TopTabStrip&) = delete;
  ~TopTabStrip() override;

  void SetTheme(const Theme& theme) override;
  // 左右让开窗口边框的按钮（macOS 的红绿灯在左边，Windows、Linux
  // 的按钮在右边）。
  void SetInsets(int leading, int trailing);
  AssistantButton* assistant_button() { return assistant_; }
  MediaCapsule* media_capsule() { return media_; }
  // 调试用：「✦ 整理 N」。
  views::View* organize_button_for_testing();
  // 媒体胶囊放在哪里（还没显示时也是它将来的位置）：媒体岛飞进来的终点。
  gfx::Rect MediaCapsuleTargetBounds() const;
  // 普通标签现在的宽度（调试用）。
  int item_width() const { return item_width_; }

  // 普通标签平分可用的宽度（要给「新标签页」按钮留位置），取整，限制在最小、最大宽度之
  // 间。
  static int TabWidth(int available, int count);

  // TabsView:
  void TabFolderAction(const std::string& id,
                       std::optional<gfx::Point> menu_point) override;

  // views::View:
  void Layout(PassKey) override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;

 protected:
  // TabsView:
  std::optional<DropTarget> DropTargetAt(
      const gfx::Point& point) const override;
  bool IsOverContent(const gfx::Point& point) const override;
  std::optional<int> PreviewEdge(views::View* preview) const override;
  void RevealActive() override;
  void OnSynced(TabListView::Change change) override;
  int PinStripLimit() const override;
  // 胶囊的卡片挂在胶囊下面。
  std::optional<CardAnchor> CardAnchorFor(const std::string& id) const override;

 private:
  // 固定区从哪开始（常用图块和后面的细线之后），标签行能用到哪。
  int PinsStart() const;
  int TrailingEdge() const;
  // 普通标签那一块（列表、「新标签页」按钮和后面的空白）从哪到哪。
  gfx::Rect FlexibleArea() const;
  int ComputeItemWidth() const;
  void ScrollStep();
  // view 放到 target：这次刷新有动效（标签增删、换了选中、开合）时从原来的位置
  // 滑过去。
  void Slide(views::View* view, const gfx::Rect& target, LayerMotion& motion);

  TabListRow* ChipFor(const std::string& id) const;

  int leading_ = 0;
  int trailing_ = 0;
  int item_width_ = 0;
  raw_ptr<ToolbarSeparator> separator_ = nullptr;
  raw_ptr<ToolbarSeparator> pins_separator_ = nullptr;
  raw_ptr<IconButton> new_tab_ = nullptr;
  raw_ptr<AssistantButton> assistant_ = nullptr;
  raw_ptr<MediaCapsule> media_ = nullptr;
  // 普通标签多了时媒体胶囊左边的「✦ 整理 N」：点了
  // 弹出整理的卡片。
  raw_ptr<PillButton> organize_ = nullptr;
  LayerMotion new_tab_motion_;
  LayerMotion separator_motion_;
  LayerMotion pins_motion_;
  LayerMotion pins_separator_motion_;
  LayerMotion scroll_shift_motion_;
  // 这一次刷新的动效：后面的东西跟着滑过去。
  TabListView::Change pending_change_ = TabListView::Change::kNone;
  // 选中的标签滚进视野。
  ValueMotion scroll_motion_;
  bool revealed_ = false;
  // 上次刷新时固定区最多多宽：窗口宽度变了、够不够放变了才重新刷新。
  int synced_limit_ = 0;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TOP_TAB_STRIP_H_
