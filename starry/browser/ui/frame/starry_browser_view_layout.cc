// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/frame/starry_browser_view_layout.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/numerics/safe_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/custom_floating_corner.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_delegate.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_params.h"
#include "chrome/browser/ui/views/frame/multi_contents_background_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_resize_area.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_background_blur_backdrop.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "chrome/browser/ui/views/tabs/organizer/organizer_tray_view.h"
#include "starry/browser/ui/archive/archive_toast.h"
#include "starry/browser/ui/update/update_toast.h"
#include "starry/browser/ui/assistant/agent_overlay.h"
#include "starry/browser/ui/assistant/assistant_island.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/downloads/download_launch.h"
#include "starry/browser/ui/downloads/downloads_panel.h"
#include "starry/browser/ui/find/find_bar_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_background_view.h"
#include "starry/browser/ui/frame/window_controls.h"
#include "starry/browser/ui/media/media_island.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/mini_window/mini_window.h"
#include "starry/browser/ui/mini_window/mini_window_toolbar.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "starry/browser/ui/split/split_overlay.h"
#include "starry/browser/ui/split/split_tabs.h"
#include "starry/browser/ui/swipe/swipe_indicator.h"
#include "starry/browser/ui/tabs/pinned_peek.h"
#include "starry/browser/ui/tabs/tab_preview.h"
#include "starry/browser/ui/tabs/top_tab_strip.h"
#include "starry/browser/ui/toolbar/browser_toolbar.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/views/view.h"

namespace starry {

namespace {

// 向导铺满窗口时的顶部拖动条高度。
constexpr int kOnboardingDragHeight = 56;

}  // namespace

StarryBrowserViewLayout::StarryBrowserViewLayout(
    std::unique_ptr<BrowserViewLayoutDelegate> delegate,
    BrowserViewLayoutViews views,
    StarryBrowserView* browser_view)
    : BrowserViewLayoutImpl(std::move(delegate), std::move(views)),
      browser_view_(browser_view) {}

StarryBrowserViewLayout::~StarryBrowserViewLayout() = default;

gfx::Size StarryBrowserViewLayout::GetMinimumSize(
    const views::View* host) const {
  if (browser_view_->is_mini_window()) {
    return gfx::Size(mini_window::kMinWidth, mini_window::kMinHeight);
  }
  return gfx::Size(metrics::kWindowMinWidth, metrics::kWindowMinHeight);
}

bool StarryBrowserViewLayout::IsContentOnly() const {
  return delegate().GetBrowserWindowState() == WindowState::kFullscreen;
}

void StarryBrowserViewLayout::DoPreLayoutComputations(
    const BrowserLayoutParams& params) {
  if (browser_view_->is_mini_window()) {
    browser_view_->SetContentOnly(IsContentOnly());
    MultiContentsView* contents = views().multi_contents_view;
    if (views::View* background = contents->background_view_for_testing();
        background && background->layer() &&
        background->layer()->opacity() != 0) {
      background->layer()->SetOpacity(0);
    }
    return;
  }
  // 侧栏顶栏要让开窗口边框在左上角占的地方。macOS 的红绿灯是 Starry
  // 自己摆的（跟着侧栏 顶栏或标签行，见
  // ChromeLayout::WindowButtonsOrigin），不用让。
#if BUILDFLAG(IS_MAC)
  browser_view_->sidebar()->SetWindowControlsInset(gfx::Size());
#else
  const gfx::SizeF leading = params.leading_exclusion.ContentWithPadding();
  browser_view_->sidebar()->SetWindowControlsInset(
      gfx::Size(base::ClampCeil(leading.width()),
                base::ClampCeil(leading.height())));
#endif
  // 全屏进出：红绿灯要不要藏起来、紧凑地址栏占不占位置跟着变。
  browser_view_->sidebar()->SetContentOnly(IsContentOnly());
  browser_view_->SetContentOnly(IsContentOnly());

  // 分屏：两格铺满内容区、中间留 split::kGap（Chrome 的是四周 8、中间 10），格子
  // 后面不要 Chrome 工具栏颜色的底：露出窗口底色和各自的卡片（WindowBackgroundView）。
  MultiContentsView* contents = views().multi_contents_view;
  contents->SetSplitViewInsets(gfx::Insets());
  // 紧凑地址栏的细栏在每一格的顶上：网页从它下面开始。
  for (ContentsContainerView* container :
       contents->contents_container_views()) {
    browser_view_->ApplyPaneInsets(container);
  }
  if (views::View* resize = contents->resize_area_for_testing()) {
    const bool side_by_side =
        contents->GetSplitLayout() == split_tabs::SplitTabLayout::kSideBySide;
    const gfx::Size gap = side_by_side ? gfx::Size(split::kGap, 24)
                                       : gfx::Size(24, split::kGap);
    if (resize->GetPreferredSize() != gap) {
      resize->SetPreferredSize(gap);
    }
  }
  if (views::View* background = contents->background_view_for_testing();
      background && background->layer() &&
      background->layer()->opacity() != 0) {
    background->layer()->SetOpacity(0);
  }
}

gfx::Rect StarryBrowserViewLayout::MiniWindowCard(const gfx::Rect& area) const {
  if (IsContentOnly()) {
    return area;
  }
  gfx::Rect card = area;
  card.Inset(gfx::Insets::TLBR(metrics::kToolbarHeight, metrics::kContentInset,
                               metrics::kContentInset, metrics::kContentInset));
  card.set_width(std::max(card.width(), 1));
  card.set_height(std::max(card.height(), 1));
  return card;
}

StarryBrowserViewLayout::ProposedLayout
StarryBrowserViewLayout::CalculateMiniWindowLayout(
    const BrowserLayoutParams& params) const {
  ProposedLayout layout;
  const gfx::Rect area = params.visual_client_area;
  const views::View* const host = views().browser_view;
  const auto hide = [&layout, host](views::View* view) {
    if (IsParentedTo(view, host)) {
      layout.AddChild(view, gfx::Rect(), false);
    }
  };
  hide(views().main_background_region);
  hide(views().main_shadow_overlay);
  hide(views().side_panel_content_transition_scrim);
  hide(views().horizontal_tab_strip_region_view);
  hide(views().vertical_tab_strip_region_view);
  hide(views().vertical_tab_strip_background_blur_backdrop);
  hide(views().vertical_tab_strip_top_corner);
  hide(views().vertical_tab_strip_bottom_corner);
  hide(views().organizer_tray);
  hide(views().side_panel);
  if (IsParentedTo(views().window_scrim, host)) {
    layout.AddChild(views().window_scrim, area);
  }
  if (IsParentedTo(views().top_container, host)) {
    auto& top_container_layout =
        layout.AddChild(views().top_container, gfx::Rect());
    const gfx::Rect local_bounds = CalculateTopContainerLayout(
        top_container_layout, params.InLocalCoordinates(area),
        /*needs_exclusion=*/false);
    top_container_layout.bounds =
        GetTopContainerBoundsInParent(local_bounds, params);
  }

  const bool content_only = IsContentOnly();
  const bool maximized =
      delegate().GetBrowserWindowState() == WindowState::kMaximized;
  layout.AddChild(browser_view_->background_view(), area);
  // 工具栏在上方，下方是内容卡片。
  WindowControlsView* const controls = browser_view_->window_controls();
  if (controls) {
    browser_view_->mini_toolbar()->SetTrailingInset(
        controls->ReservedBarWidth());
    const gfx::Size visual = controls->VisualSize();
    const int visual_right = area.right() - 6;
    const int visual_y =
        area.y() + (metrics::kToolbarHeight - visual.height()) / 2;
    const gfx::Insets pad =
        maximized
            ? gfx::Insets::TLBR(std::max(0, visual_y - area.y()), 0, 0,
                                std::max(0, area.right() - visual_right))
            : gfx::Insets();
    controls->SetHitPadding(pad);
    layout.AddChild(controls,
                    gfx::Rect(visual_right - visual.width(),
                              visual_y - pad.top(),
                              visual.width() + pad.right(),
                              visual.height() + pad.top()),
                    !content_only);
  } else {
    browser_view_->mini_toolbar()->SetTrailingInset(0);
    if (WindowControlsView* const raw_controls =
            browser_view_->raw_window_controls()) {
      layout.AddChild(raw_controls, gfx::Rect(), false);
    }
  }
  layout.AddChild(
      browser_view_->mini_toolbar(),
      gfx::Rect(area.x(), area.y(), area.width(), metrics::kToolbarHeight),
      !content_only);
  gfx::Rect card = MiniWindowCard(area);
  if (IsParentedTo(views().infobar_container, host)) {
    gfx::Rect infobar_bounds;
    const bool infobar_visible = delegate().IsInfobarVisible();
    if (infobar_visible) {
      infobar_bounds =
          gfx::Rect(card.x(), card.y(), card.width(),
                    views().infobar_container->GetPreferredSize().height());
      card.Inset(gfx::Insets::TLBR(infobar_bounds.height(), 0, 0, 0));
    }
    layout.AddChild(views().infobar_container, infobar_bounds, infobar_visible);
  }

  // 触控板返回的指示器：铺满窗口、不接鼠标。
  layout.AddChild(browser_view_->swipe_indicator(), area, true);
  FindBarView* const find_bar = browser_view_->find_bar_view();
  layout.AddChild(find_bar, area, find_bar->GetVisible());
  views::View* const download_launch = browser_view_->download_launch();
  layout.AddChild(download_launch, area, true);
  views::View* const popover_host = browser_view_->popover_host();
  layout.AddChild(popover_host, area, popover_host->GetVisible());
  CommandBarView* const command_bar = browser_view_->command_bar();
  layout.AddChild(command_bar, area, command_bar->GetVisible());
  views::View* const dialog_host = browser_view_->dialog_host();
  layout.AddChild(dialog_host, area, dialog_host->GetVisible());
  views::View* const menu_host = browser_view_->menu_host();
  layout.AddChild(menu_host, area, menu_host->GetVisible());

  CHECK(IsParentedToAndVisible(views().multi_contents_view, host));
  card.set_height(std::max(card.height(), 1));
  layout.AddChild(views().multi_contents_view, card);
  return layout;
}

StarryBrowserViewLayout::ProposedLayout
StarryBrowserViewLayout::CalculateProposedLayout(
    const BrowserLayoutParams& params) const {
  if (browser_view_->is_mini_window()) {
    return CalculateMiniWindowLayout(params);
  }
  ProposedLayout layout;
  const gfx::Rect area = params.visual_client_area;
  const views::View* const host = views().browser_view;

  // Chrome 的界面里用不上的部分：藏起来。
  const auto hide = [&layout, host](views::View* view) {
    if (IsParentedTo(view, host)) {
      layout.AddChild(view, gfx::Rect(), false);
    }
  };
  hide(views().main_background_region);
  hide(views().main_shadow_overlay);
  hide(views().side_panel_content_transition_scrim);
  hide(views().horizontal_tab_strip_region_view);
  hide(views().vertical_tab_strip_region_view);
  hide(views().vertical_tab_strip_background_blur_backdrop);
  hide(views().vertical_tab_strip_top_corner);
  hide(views().vertical_tab_strip_bottom_corner);
  hide(views().organizer_tray);
  hide(views().side_panel);

  // 挡住整个窗口的遮罩（窗口级的对话框开着时）。
  if (IsParentedTo(views().window_scrim, host)) {
    layout.AddChild(views().window_scrim, area);
  }

  // 顶栏容器是空的，但要排（上游的命中测试等要用）。
  if (IsParentedTo(views().top_container, host)) {
    auto& top_container_layout =
        layout.AddChild(views().top_container, gfx::Rect());
    const gfx::Rect local_bounds = CalculateTopContainerLayout(
        top_container_layout, params.InLocalCoordinates(area),
        /*needs_exclusion=*/false);
    top_container_layout.bounds =
        GetTopContainerBoundsInParent(local_bounds, params);
  }

  const bool content_only = IsContentOnly();
  layout.AddChild(browser_view_->background_view(), area);

  // 设置向导：卡片铺满窗口并隐藏外框；结束时随 presence 1→0 缩回原位并推入各栏。
  const double onboarding =
      content_only ? 0
                   : std::clamp(browser_view_->onboarding_presence(), 0.0, 1.0);
  const bool onboarding_only = onboarding > 0.999;

  // 侧栏：固定、浮动时占一列；自动隐藏时盖在卡片上（收起时不显示，左边缘是感应条）。
  // 在动的时候（切换样子、调宽度）每一帧重新排。
  SidebarView* const sidebar = browser_view_->sidebar();
  const SidebarView::Placement placement = sidebar->ComputePlacement(area);
  gfx::Rect sidebar_bounds = placement.bounds;
  if (onboarding > 0) {
    sidebar_bounds.Offset(
        -static_cast<int>(
            std::lround((placement.content_left - area.x()) * onboarding)),
        0);
  }
  layout.AddChild(sidebar, content_only ? gfx::Rect() : sidebar_bounds,
                  !content_only && placement.visible && !onboarding_only);
  views::View* const edge = browser_view_->sidebar_edge();
  layout.AddChild(
      edge, placement.edge_trigger,
      !content_only && !placement.edge_trigger.IsEmpty() && onboarding <= 0);

  // 标签行（顶部标签布局）：从顶边移入并淡入。Windows 自绘系统控件或 Linux 窗口边框
  // 按钮位于这一行右侧，布局时让开它们。
  WindowControlsView* const controls = browser_view_->window_controls();
  WindowRailView* const rail_view = browser_view_->window_rail();
  const bool starry_controls = controls != nullptr;
  const bool maximized =
      delegate().GetBrowserWindowState() == WindowState::kMaximized;
  const gfx::SizeF trailing_exclusion =
      params.trailing_exclusion.ContentWithPadding();
  const int controls_reserve =
      starry_controls ? controls->ReservedBarWidth() : 0;
  const int caption_height =
      starry_controls ? 0 : base::ClampCeil(trailing_exclusion.height());
  const int caption_width = starry_controls
                                ? controls_reserve
                                : base::ClampCeil(trailing_exclusion.width());
  const double strip = std::clamp(browser_view_->strip_presence(), 0.0, 1.0);
  const int strip_shown =
      static_cast<int>(std::lround(metrics::kTabStripHeight * strip));
  const double toolbar =
      std::clamp(browser_view_->toolbar_presence(), 0.0, 1.0);
  // 向导结束时，标签行与工具栏随卡片顶边自上方推入。
  const int chrome_slide = static_cast<int>(std::lround(
      (strip_shown +
       (placement.panel_inset + metrics::kToolbarHeight) * toolbar) *
      onboarding));
  TopTabStrip* const tab_strip = browser_view_->tab_strip();
  if (starry_controls) {
    tab_strip->SetInsets(metrics::kContentInset,
                         metrics::kContentInset + caption_width);
  } else {
#if BUILDFLAG(IS_MAC)
    tab_strip->SetInsets(metrics::kTabStripLeading, metrics::kContentInset);
#else
    tab_strip->SetInsets(
        metrics::kContentInset +
            base::ClampCeil(
                params.leading_exclusion.ContentWithPadding().width()),
        metrics::kContentInset + caption_width);
#endif
  }
  layout.AddChild(tab_strip,
                  gfx::Rect(area.x(),
                            area.y() + strip_shown - metrics::kTabStripHeight -
                                chrome_slide,
                            area.width(), metrics::kTabStripHeight),
                  !content_only && strip > 0.001 && !onboarding_only);

  // 工具栏：和内容卡片左右对齐，浮动侧栏时跟面板一起下移；淡入淡出。
  const int card_left = content_only ? area.x() : placement.content_left;
  // 助手停在右边时，卡片给它让出一列（工具栏还是通到右边）。
  const int dock = content_only ? 0
                                : static_cast<int>(std::lround(
                                      browser_view_->assistant_dock_inset()));
  const int right = area.right() - metrics::kContentInset;
  const int toolbar_right = strip > 0.001 ? right : right - caption_width;
  layout.AddChild(
      browser_view_->toolbar(),
      gfx::Rect(card_left,
                area.y() + strip_shown + placement.panel_inset - chrome_slide,
                std::max(0, toolbar_right - card_left),
                metrics::kToolbarHeight),
      !content_only && toolbar > 0.001 && !onboarding_only);

  // 纤细顶檐模式（Windows / Linux）：纯侧栏无顶栏时在内容卡片上方保留 30px 拖动栏。
  const double rail = std::clamp(browser_view_->rail_presence(), 0.0, 1.0);
  const int rail_shown =
      static_cast<int>(std::lround(metrics::kWindowRailHeight * rail));
  if (rail_view) {
    const int rail_right = area.right() - controls_reserve - 4;
    layout.AddChild(
        rail_view,
        gfx::Rect(card_left, area.y(), std::max(0, rail_right - card_left),
                  metrics::kWindowRailHeight),
        !content_only && rail > 0.001 && !onboarding_only);
  } else if (WindowRailView* const raw_rail =
                 browser_view_->raw_window_rail()) {
    layout.AddChild(raw_rail, gfx::Rect(), false);
  }

  // 内容卡片：位于标签行、工具栏下方。悬浮玻璃胶囊模式下纯侧栏时直接顶至 8px；
  // 纤细顶檐模式下位于 30px 顶檐下方；Linux 非自绘控件沿用窗口边框排除高度。
  gfx::Rect card = area;
  if (!content_only) {
    int top =
        area.y() + strip_shown +
        static_cast<int>(std::lround(
            (placement.panel_inset + metrics::kToolbarHeight) * toolbar)) +
        static_cast<int>(std::lround(browser_view_->content_top_inset()));
    if (rail_shown > 0) {
      top = std::max(top, area.y() + rail_shown);
    }
    if (!starry_controls && strip <= 0.001 && toolbar <= 0.001) {
      top = std::max(top, area.y() + caption_height);
    }
    card = gfx::Rect(card_left, top, std::max(0, right - dock - card_left),
                     std::max(0, area.bottom() - metrics::kContentInset - top));
    if (onboarding > 0) {
      card = gfx::Tween::RectValueBetween(onboarding, card, area);
    }
  }
  layout.AddChild(
      browser_view_->onboarding_drag_bar(),
      gfx::Rect(area.x(), area.y(), area.width(), kOnboardingDragHeight),
      !content_only && browser_view_->in_onboarding());

  // 紧凑栏里的按钮要等每个分屏的实际位置确定以后再排（UpdateChromeGeometry）。
  if (controls && controls->presentation() !=
                      WindowControlsView::Presentation::kCompactBar) {
    const gfx::Size visual = controls->VisualSize();
    int visual_right = area.right() - 6;
    int visual_y;
    bool extend_edge = maximized;
    if (controls->presentation() ==
        WindowControlsView::Presentation::kFloatingGlass) {
      const int margin = maximized ? 4 : 14;
      visual_right = area.right() - margin;
      visual_y = area.y() + margin;
      extend_edge = maximized || controls->auto_hide();
    } else if (controls->presentation() ==
               WindowControlsView::Presentation::kSlimRail) {
      visual_y = area.y() + (metrics::kWindowRailHeight - visual.height()) / 2;
    } else {
      const int bar_top =
          strip > 0.001 ? area.y() + strip_shown - metrics::kTabStripHeight
                        : area.y() + placement.panel_inset;
      const int bar_height =
          strip > 0.001 ? metrics::kTabStripHeight : metrics::kToolbarHeight;
      visual_y = bar_top + (bar_height - visual.height()) / 2;
    }
    const gfx::Insets pad =
        extend_edge
            ? gfx::Insets::TLBR(std::max(0, visual_y - area.y()), 0, 0,
                                std::max(0, area.right() - visual_right))
            : gfx::Insets();
    controls->SetHitPadding(pad);
    layout.AddChild(controls,
                    gfx::Rect(visual_right - visual.width(),
                              visual_y - pad.top(),
                              visual.width() + pad.right(),
                              visual.height() + pad.top()),
                    !content_only);
  } else if (WindowControlsView* const raw_controls =
                 browser_view_->raw_window_controls();
             raw_controls && !controls) {
    layout.AddChild(raw_controls, gfx::Rect(), false);
  }

  // 信息栏在卡片上面。
  if (IsParentedTo(views().infobar_container, host)) {
    gfx::Rect infobar_bounds;
    const bool infobar_visible = delegate().IsInfobarVisible();
    if (infobar_visible) {
      infobar_bounds =
          gfx::Rect(card.x(), card.y(), card.width(),
                    views().infobar_container->GetPreferredSize().height());
      card.Inset(gfx::Insets::TLBR(infobar_bounds.height(), 0, 0, 0));
    }
    layout.AddChild(views().infobar_container, infobar_bounds, infobar_visible);
  }

  // 自动整理与软件更新提示条：铺满内容卡片，提示条在左下角，只有提示条接鼠标。
  if (views::View* const archive_toast = browser_view_->archive_toast()) {
    layout.AddChild(archive_toast, card, !content_only);
  }
  if (views::View* const update_toast = browser_view_->update_toast()) {
    layout.AddChild(update_toast, card, !content_only);
  }

  auto* peek = browser_view_->pinned_peek();
  const bool peeking = peek && peek->preview() && !content_only;
  if (peek) {
    layout.AddChild(peek, card, peeking);
  }
  if (peeking) {
    card = PinnedPeek::PageBounds(card);
  }

  // 分屏的分隔线、把手、描边、落点预览：铺满窗口，只有分隔线和把手接鼠标。
  layout.AddChild(browser_view_->split_overlay(), area, true);
  // 智能体在网页上的提示、触控板返回的指示器：铺满窗口、不接鼠标。
  layout.AddChild(browser_view_->agent_overlay(), area, true);
  layout.AddChild(browser_view_->swipe_indicator(), area, true);
  // AI 助手的岛：铺满窗口，只有玻璃接鼠标。
  AssistantIsland* const island = browser_view_->assistant_island();
  layout.AddChild(island, area, island->GetVisible());
  // 媒体岛：铺满窗口，只有胶囊、卫星和展开的卡片接鼠标。
  MediaIsland* const media_island = browser_view_->media_island();
  layout.AddChild(media_island, area, media_island->GetVisible());
  // 标签的预览卡片：铺满窗口、不接鼠标。
  layout.AddChild(browser_view_->tab_preview(), area, true);

  // 查找栏（在卡片右上角）、命令栏、菜单盖住整个窗口；开没开由它们自己管。
  FindBarView* const find_bar = browser_view_->find_bar_view();
  layout.AddChild(find_bar, area, find_bar->GetVisible());

  views::View* const download_launch = browser_view_->download_launch();
  layout.AddChild(download_launch, area, true);
  views::View* const downloads_panel = browser_view_->downloads_panel();
  layout.AddChild(downloads_panel, area, downloads_panel->GetVisible());
  views::View* const popover_host = browser_view_->popover_host();
  layout.AddChild(popover_host, area, popover_host->GetVisible());
  CommandBarView* const command_bar = browser_view_->command_bar();
  layout.AddChild(command_bar, area, command_bar->GetVisible());
  views::View* const dialog_host = browser_view_->dialog_host();
  layout.AddChild(dialog_host, area, dialog_host->GetVisible());
  views::View* const menu_host = browser_view_->menu_host();
  layout.AddChild(menu_host, area, menu_host->GetVisible());

  CHECK(IsParentedToAndVisible(views().multi_contents_view, host));
  card.set_width(std::max(card.width(), 1));
  card.set_height(std::max(card.height(), 1));
  layout.AddChild(views().multi_contents_view, card);

  return layout;
}

gfx::Rect StarryBrowserViewLayout::CalculateTopContainerLayout(
    ProposedLayout& layout,
    BrowserLayoutParams params,
    bool needs_exclusion) const {
  const views::View* const top_container = views().top_container;
  const auto hide = [&layout, top_container](views::View* view) {
    if (IsParentedTo(view, top_container)) {
      layout.AddChild(view, gfx::Rect(), false);
    }
  };
  hide(views().horizontal_tab_strip_region_view);
  hide(views().toolbar);
  hide(views().bookmark_bar);
  hide(views().top_container_separator);
  return gfx::Rect(params.visual_client_area.x(), params.visual_client_area.y(),
                   params.visual_client_area.width(), 0);
}

void StarryBrowserViewLayout::DoPostLayoutVisualAdjustments(
    const BrowserLayoutParams& params) {
  const bool content_only = IsContentOnly();
  const float radius = content_only ? 0 : radius::kContent;
  MultiContentsView* const contents = views().multi_contents_view;
  if (browser_view_->is_mini_window()) {
    contents->SetBackgroundRadii(gfx::RoundedCornersF(radius));
    browser_view_->find_bar_view()->SetCardBounds(browser_view_->PageBounds());
    browser_view_->UpdateMiniWindowGeometry(
        content_only
            ? gfx::Rect()
            : views::View::ConvertRectToTarget(
                  contents->parent(), browser_view_.get(), contents->bounds()));
    return;
  }
  // 设置向导铺满窗口时卡片没有圆角，缩回去的路上长出来。
  const double onboarding =
      content_only ? 0
                   : std::clamp(browser_view_->onboarding_presence(), 0.0, 1.0);
  const float card_radius = radius * static_cast<float>(1 - onboarding);
  // 紧凑地址栏时网页的上边不圆：细栏自己画卡片上边的圆角。
  contents->SetBackgroundRadii(
      browser_view_->compact_inset() > 0
          ? gfx::RoundedCornersF(0, 0, card_radius, card_radius)
          : gfx::RoundedCornersF(card_radius));
  // 分屏时每一格的圆角是自己设的（见
  // OverrideSplitPaneDecorations）：开关紧凑地址栏时跟 上。
  if (contents->IsInSplitView()) {
    for (ContentsContainerView* container :
         contents->contents_container_views()) {
      container->SetRoundedCorners(contents->GetBackgroundRadii());
    }
  }
  // 先确定紧凑栏和窗口按钮的位置，再排需要让开它们的组件。
  browser_view_->UpdateChromeGeometry();
  browser_view_->find_bar_view()->SetCardBounds(
      browser_view_->BoundsBelowWindowControls(browser_view_->PageBounds()));
  // 标签行、工具栏、顶檐淡入淡出（设置向导走完时也跟着淡入）。
  browser_view_->tab_strip()->layer()->SetOpacity(
      static_cast<float>(std::clamp(browser_view_->strip_presence(), 0.0, 1.0) *
                         (1 - onboarding)));
  browser_view_->toolbar()->layer()->SetOpacity(static_cast<float>(
      std::clamp(browser_view_->toolbar_presence(), 0.0, 1.0) *
      (1 - onboarding)));
  if (WindowRailView* rail_view = browser_view_->window_rail()) {
    rail_view->layer()->SetOpacity(static_cast<float>(
        std::clamp(browser_view_->rail_presence(), 0.0, 1.0) *
        (1 - onboarding)));
  }
  WindowBackgroundView* const background = browser_view_->background_view();
  // 窗口底上的卡片（分屏时每格一张）、分屏的把手和描边。
  browser_view_->split_overlay()->UpdateGeometry();
  // 助手的岛在内容卡片下方正中。
  AssistantIsland* const island = browser_view_->assistant_island();
  const gfx::Rect card = views::View::ConvertRectToTarget(
      contents->parent(), browser_view_.get(), contents->bounds());
  const int dock_right =
      params.visual_client_area.right() - metrics::kContentInset;
  const gfx::Rect dock(dock_right - metrics::kAssistantDockWidth, card.y(),
                       metrics::kAssistantDockWidth, card.height());
  island->SetLayoutBounds(
      views::View::ConvertRectToTarget(browser_view_.get(), island, card),
      views::View::ConvertRectToTarget(
          browser_view_.get(), island,
          browser_view_->BoundsBelowWindowControls(dock)));
  // 媒体岛在内容卡片右下角。
  MediaIsland* const media_island = browser_view_->media_island();
  media_island->SetCardBounds(views::View::ConvertRectToTarget(
      contents->parent(), media_island, contents->bounds()));

  // 窗口边框在角上画的按钮（Linux）：底在那里留空；Windows 由 Starry 自绘，不抠洞。
  std::vector<gfx::Rect> frame_controls;
#if !BUILDFLAG(IS_MAC)
  if (!browser_view_->window_controls()) {
    const gfx::Size leading =
        gfx::ToCeiledSize(params.leading_exclusion.content);
    const gfx::Size trailing =
        gfx::ToCeiledSize(params.trailing_exclusion.content);
    if (!leading.IsEmpty()) {
      frame_controls.emplace_back(gfx::Point(), leading);
    }
    if (!trailing.IsEmpty()) {
      frame_controls.emplace_back(background->width() - trailing.width(), 0,
                                  trailing.width(), trailing.height());
    }
  }
#endif
  background->SetFrameControlsRects(std::move(frame_controls));
  // 智能体提示跟着各个分屏，也要让开刚排好的助手面板。
  browser_view_->agent_overlay()->Update();
}

}  // namespace starry
