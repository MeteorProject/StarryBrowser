// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TOOLBAR_ADDRESS_PILL_H_
#define STARRY_BROWSER_UI_TOOLBAR_ADDRESS_PILL_H_

#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/bookmarks/browser/base_bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_model_observer.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/content_blocking/content_blocking_service.h"
#include "starry/browser/content_blocking/content_filter_tab_helper.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point.h"
#include "ui/views/controls/button/button.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace bookmarks {
class BookmarkModel;
}

namespace content {
class WebContents;
}

namespace gfx {
struct VectorIcon;
}

namespace starry {

class ContentBlockingButton;
class GlyphButton;

// 地址栏展示摘要：普通网页为域名与安全状态，内部页面为页面名称，空白页和新标签页为
// 占位提示文字。
struct AddressSummary {
  std::u16string name;
  // 网页标题：和 name 一样或者没有时为空（内部页面总是空的）。紧凑地址栏用。
  std::u16string title;
  bool placeholder = true;
  bool secure = false;
  // 浏览器自己的页面：页面的图标（名字是页面名）。
  raw_ptr<const gfx::VectorIcon> page_icon = nullptr;
  // 普通网页的地址（加书签用）；空白页、内部页面为空。
  GURL url;

  bool operator==(const AddressSummary&) const = default;
};

AddressSummary SummarizeAddress(content::WebContents* contents);

// 地址栏控件（标准胶囊样式与紧凑地址栏样式），点击打开命令栏。
// - kPill：侧栏内占满一行、工具栏内占满剩余宽度，高 34，浅底色，悬停加深。
// - kCompact：紧凑地址栏内显示「网站 / 标题」，高 28，平时无底色，悬停时浮现浅底。
// 右端为书签星号（默认隐藏，悬停显示，已收藏时以强调色常亮，点击直接切换收藏）；
// 星号左侧为内容拦截盾牌（有拦截数或对该站停用拦截时常亮，其余悬停显示，点击打开
// 详情卡片）。切换标签时域名交叉淡化过渡。
//
// 默认显示当前标签；SetPaneTab 之后显示那个标签（分屏时每一格的紧凑地址栏）。
class AddressPill : public views::Button,
                    public TabStripModelObserver,
                    public bookmarks::BaseBookmarkModelObserver,
                    public content_blocking::ContentBlockingService::Observer,
                    public content_blocking::ContentFilterTabHelper::Observer {
  METADATA_HEADER(AddressPill, views::Button)

 public:
  enum class Style { kPill, kCompact };

  AddressPill(BrowserWindowInterface* browser, Style style);
  AddressPill(const AddressPill&) = delete;
  AddressPill& operator=(const AddressPill&) = delete;
  ~AddressPill() override;

  Style style() const { return style_; }
  void SetTheme(const Theme& theme);
  // 分屏的一格：显示这个标签，点一下先让它成为当前标签。空的时候跟着当前标签。
  void SetPaneTab(std::optional<tabs::TabHandle> tab);
  // 按住拖动超过阈值时直接移动窗口（小窗口里点到地址文字也能顺手拖窗）。
  void SetDragWindowOnDrag(bool enabled);
  const AddressSummary& summary() const { return summary_; }

  // views::Button:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void Layout(PassKey) override;
  void PaintButtonContents(gfx::Canvas* canvas) override;
  void StateChanged(ButtonState old_state) override;
  std::u16string GetRenderedTooltipText(const gfx::Point& p) const override;
  void ChildPreferredSizeChanged(views::View* child) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;

  // bookmarks::BaseBookmarkModelObserver:
  void BookmarkModelChanged() override;

  // ContentBlockingService::Observer:
  void OnContentBlockingChanged() override;
  // ContentFilterTabHelper::Observer:
  void OnBlockedCountChanged(content::WebContents* contents) override;

  // 调试步骤：点盾牌、看它的样子。
  void ShowContentBlockingPopover();
  std::u16string DescribeShieldForTesting() const;

 private:
  tabs::TabInterface* ShownTab() const;
  void Refresh();
  void UpdateStar(bool animate);
  void UpdateShield();
  void ToggleBookmark();
  void Pressed();
  void PaintSummary(gfx::Canvas* canvas, const AddressSummary& summary);
  // 星号让出的宽度（有网址时）。
  int AccessoryWidth() const;
  // 按当前内容算出的自然宽度（图标、网站/标题、右端小按钮和内边距）。
  int PreferredContentWidth() const;

  const raw_ptr<BrowserWindowInterface> browser_;
  const Style style_;
  Theme theme_ = Theme::DarkBase();
  std::optional<tabs::TabHandle> pane_tab_;
  std::optional<tabs::TabHandle> shown_tab_;
  AddressSummary summary_;
  // 换标签时淡出的那一份。
  AddressSummary previous_;
  ValueMotion swap_;
  bool bookmarked_ = false;
  raw_ptr<bookmarks::BookmarkModel> bookmarks_ = nullptr;
  raw_ptr<GlyphButton> star_ = nullptr;
  raw_ptr<ContentBlockingButton> shield_ = nullptr;
  // 盾牌的卡片开着（盾牌一直显示）。
  bool shield_popover_open_ = false;
  PrefChangeRegistrar fingerprint_prefs_;
  // 位置模拟的设置：模拟位置的网站也显示盾牌。
  base::CallbackListSubscription location_subscription_;
  LayerMotion star_motion_;
  // 自己点了星号：模型变了的通知里播替换的动画。
  bool star_toggled_ = false;
  bool drag_window_on_drag_ = false;
  bool pending_window_drag_ = false;
  bool running_move_loop_ = false;
  gfx::Point drag_press_location_;
  base::ScopedObservation<bookmarks::BookmarkModel,
                          bookmarks::BookmarkModelObserver>
      bookmarks_observation_{this};
  base::WeakPtrFactory<AddressPill> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TOOLBAR_ADDRESS_PILL_H_
