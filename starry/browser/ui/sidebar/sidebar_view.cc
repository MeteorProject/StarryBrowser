// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/sidebar/sidebar_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/archive/archive_button.h"
#include "starry/browser/ui/archive/archive_page.h"
#include "starry/browser/ui/archive/organize_page.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_background_view.h"
#include "starry/browser/ui/frame/window_controls.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/media/media_dock.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/sidebar/extension_bar.h"
#include "starry/browser/ui/sidebar/sidebar_parts.h"
#include "starry/browser/ui/sidebar/utility_buttons.h"
#include "starry/browser/ui/tabs/sidebar_tabs.h"
#include "starry/browser/ui/toolbar/address_pill.h"
#include "starry/browser/ui/toolbar/navigation_buttons.h"
#include "third_party/metrics_proto/omnibox_event.pb.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/page_transition_types.h"
#include "ui/compositor/layer.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/view_targeter.h"

namespace starry {

namespace {

// 底部工具按钮那一行的高度；全部藏起来时只留 6。
constexpr int kToolRowHeight = 40;
constexpr int kToolRowCollapsedHeight = 6;
// 地址栏下方留 10，扩展栏下方留 8。
constexpr int kAddressBottomGap = 10;
constexpr int kExtensionBottomGap = 8;
// 媒体坞和下面工具按钮那一行隔 4，和上面的标签列表隔 6。
constexpr int kMediaBottomGap = 4;
constexpr int kMediaTopGap = 6;

// 自动隐藏侧栏滑出后的收回延迟与外部保留范围。
constexpr base::TimeDelta kKeepAfterLeaving = base::Milliseconds(150);
constexpr base::TimeDelta kPointerPoll = base::Milliseconds(33);
constexpr int kOutsideReach = 200;
// 收起时多挪出去这么多，阴影也看不见。
constexpr int kHiddenExtra = 32;

int ClampWidth(int width) {
  return std::clamp(width, metrics::kSidebarMinWidth,
                    metrics::kSidebarMaxWidth);
}

int PanelInsetFor(const ChromeLayout& layout) {
  return layout.floating() || layout.auto_hide ? metrics::kFloatingSidebarInset
                                               : 0;
}

}  // namespace

SidebarView::SidebarView(BrowserWindowInterface* browser,
                         MatchedGeometry* matched)
    : browser_(browser),
      matched_(matched->GetWeakPtr()),
      floating_(0,
                base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                    base::Unretained(this))),
      column_(1,
              base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                  base::Unretained(this))),
      width_(metrics::kSidebarWidth,
             base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                 base::Unretained(this))),
      header_height_(metrics::kSidebarHeaderHeight,
                     base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                         base::Unretained(this))),
      address_row_(1,
                   base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                       base::Unretained(this))),
      extension_row_(1,
                     base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                         base::Unretained(this))),
      media_row_(0,
                 base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                     base::Unretained(this))),
      transition_(base::BindRepeating(&SidebarView::NotifyLayoutChanged,
                                      base::Unretained(this))) {
  SetWindowDraggable(this, true);
  // 自己的图层：滑出、收起是这个图层的变换（在合成器上）。图层自己不画东西，子视图都有
  // 自己的图层。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));

  shadow_ = AddChildView(std::make_unique<SidebarShadow>());
  glass_ = AddChildView(std::make_unique<GlassView>());
  glass_->SetCornerRadius(metrics::kFloatingSidebarRadius, true);
  fill_ = AddChildView(std::make_unique<SidebarPanelFill>());
  fill_->SetRadius(metrics::kFloatingSidebarRadius);

  // 内容有自己的图层，才能画在玻璃上面。
  content_ = AddChildView(std::make_unique<views::View>());
  content_->SetPaintToLayer();
  content_->layer()->SetFillsBoundsOpaquely(false);

  nav_ = content_->AddChildView(std::make_unique<NavigationButtons>(browser_));
  archive_button_ =
      content_->AddChildView(std::make_unique<ArchiveButton>(browser_));
  archive_visibility_subscription_ =
      archive_button_->AddVisibleChangedCallback(base::BindRepeating(
          [](SidebarView* sidebar) { sidebar->InvalidateLayout(); },
          base::Unretained(this)));
  address_pill_ = content_->AddChildView(
      std::make_unique<AddressPill>(browser_, AddressPill::Style::kPill));

  // 扩展按钮：地址栏下面一行，没有带按钮的扩展时不占位置。
  extension_bar_ =
      content_->AddChildView(std::make_unique<ExtensionBar>(browser_));
  extension_bar_->SetPaintToLayer();
  extension_bar_->layer()->SetFillsBoundsOpaquely(false);
  extension_bar_->SetContentChangedCallback(base::BindRepeating(
      [](SidebarView* sidebar) {
        sidebar->extension_row_.SetValue(
            sidebar->extension_bar_->HasContent() &&
                    !sidebar->layout_.extensions_in_toolbar
                ? 1
                : 0);
        sidebar->NotifyLayoutChanged();
        if (sidebar->extensions_changed_) {
          sidebar->extensions_changed_.Run();
        }
      },
      base::Unretained(this)));

  tabs_ = content_->AddChildView(std::make_unique<SidebarTabs>(browser_));
  // 翻到归档页时整块标签往左让开、淡出：要自己的图层。
  tabs_->SetPaintToLayer();
  tabs_->layer()->SetFillsBoundsOpaquely(false);
  // 归档页、整理页在标签上面、媒体坞下面（坞展开时一样盖住它们）。
  archive_page_ =
      content_->AddChildView(std::make_unique<ArchivePage>(browser_));
  archive_page_->SetVisible(false);
  organize_page_ =
      content_->AddChildView(std::make_unique<OrganizePage>(browser_, false));
  organize_page_->SetVisible(false);

  utility_ = content_->AddChildView(
      std::make_unique<UtilityButtons>(browser_, ChromePlacement::kSidebar));
  utility_->SetContentChangedCallback(
      base::BindRepeating([](views::View* view) { view->InvalidateLayout(); },
                          base::Unretained(this)));

  SidebarResizeHandle::Callbacks callbacks;
  callbacks.current_width = base::BindRepeating(&SidebarView::sidebar_width,
                                                base::Unretained(this));
  callbacks.resize = base::BindRepeating(
      [](SidebarView* sidebar, int width) {
        sidebar->resizing_ = true;
        sidebar->width_.SetValue(width);
        sidebar->NotifyLayoutChanged();
      },
      base::Unretained(this));
  callbacks.commit = base::BindRepeating(
      [](SidebarView* sidebar, int width) {
        sidebar->resizing_ = false;
        prefs::PrefsFor(sidebar->browser_->GetProfile())
            ->SetInteger(prefs::kSidebarWidth,
                         width == metrics::kSidebarWidth ? 0 : width);
      },
      base::Unretained(this));
  callbacks.reset = base::BindRepeating(
      [](SidebarView* sidebar) {
        sidebar->resizing_ = false;
        prefs::PrefsFor(sidebar->browser_->GetProfile())
            ->SetInteger(prefs::kSidebarWidth, 0);
        // 已经是默认宽度时设置不会变：照样动回去。
        sidebar->width_.AnimateTo(metrics::kSidebarWidth,
                                  motion::ChromeLayout());
      },
      base::Unretained(this));
  resize_handle_ =
      AddChildView(std::make_unique<SidebarResizeHandle>(std::move(callbacks)));

  // 媒体坞：在工具按钮那一行上面，展开时盖住标签列表，还会越过侧栏的右边盖到网页上：
  // 挂在侧栏自己这一层（内容和调宽把手上面），不被内容那一层裁掉、不被把手抢走鼠标，
  // 仍跟着侧栏滑出、淡出。
  media_ = AddChildView(std::make_unique<MediaDock>(browser_));
  media_->SetPresenceChangedCallback(base::BindRepeating(
      [](SidebarView* sidebar) {
        const double target = sidebar->media_->ShouldShow() ? 1 : 0;
        if (sidebar->GetWidget() && sidebar->GetVisible()) {
          sidebar->media_row_.AnimateTo(target,
                                        MotionCurve::Of(motion::kMediaDockRow));
        } else {
          sidebar->media_row_.SetValue(target);
          sidebar->NotifyLayoutChanged();
        }
      },
      base::Unretained(this)));
  // 坞的高度变了（其余的媒体多了、少了）：只有侧栏自己重新排，窗口别的地方不动。
  media_->SetLayoutChangedCallback(base::BindRepeating(
      [](SidebarView* sidebar) { sidebar->InvalidateLayout(); },
      base::Unretained(this)));
  media_row_.SetValue(media_->ShouldShow() ? 1 : 0);

  PrefService* prefs = prefs::PrefsFor(browser_->GetProfile());
  prefs_.Init(prefs);
  prefs_.Add(prefs::kSidebarWidth,
             base::BindRepeating(&SidebarView::ApplyWidth,
                                 base::Unretained(this), /*animated=*/true));
  ApplyWidth(/*animated=*/false);
  SetChromeLayout(ChromeLayout::FromPrefs(prefs, extension_bar_->HasContent()),
                  /*animated=*/false);
  UpdateColors();
}

SidebarView::~SidebarView() {
  if (!matched_) {
    return;
  }
  for (views::View* view : {static_cast<views::View*>(nav_.get()),
                            static_cast<views::View*>(address_pill_.get())}) {
    matched_->Forget(view);
  }
}

views::View* SidebarView::address_pill_view() {
  return address_pill_;
}

bool SidebarView::is_floating() const {
  return floating_.target() > 0.5;
}

bool SidebarView::is_auto_hide() const {
  return auto_hide_;
}

int SidebarView::sidebar_width() const {
  return static_cast<int>(std::round(width_.value()));
}

int SidebarView::header_height() const {
  const int panel_y = PanelRect().y();
  const int inset_height =
      std::max(0, window_controls_inset_.height() - panel_y);
  return std::max(static_cast<int>(std::round(header_height_.value())),
                  inset_height);
}

SidebarView::Placement SidebarView::ComputePlacement(
    const gfx::Rect& area) const {
  const double f = std::clamp(floating_.value(), 0.0, 1.0);
  const double c = std::clamp(column_.value(), 0.0, 1.0);
  const int width = sidebar_width();
  const double inset = metrics::kFloatingSidebarInset * f;
  // 浮动时拖动区骑在面板边上，右边多出半个。
  const int handle_overhang = static_cast<int>(
      std::round(metrics::kSidebarResizeHandleWidth / 2.0 * f));
  Placement placement;
  placement.bounds =
      gfx::Rect(area.x(), area.y(),
                static_cast<int>(std::round(inset + width)) + handle_overhang,
                area.height());
  placement.content_left =
      area.x() +
      static_cast<int>(std::round((inset + width) * c +
                                  metrics::kContentInset * std::max(f, 1 - c)));
  placement.visible =
      c > 0.001 || column_.is_animating() || peeking_ || sliding_out_;
  placement.panel_inset =
      static_cast<int>(std::round(metrics::kFloatingSidebarInset * f * c));
  if (auto_hide_ && !peeking_) {
    placement.edge_trigger =
        gfx::Rect(area.x(), area.y(), metrics::kSidebarEdgeTriggerWidth,
                  area.height());
  }
  return placement;
}

void SidebarView::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  UpdateColors();
}

void SidebarView::SetChromeLayout(const ChromeLayout& layout, bool animated) {
  const ChromeLayout old = layout_;
  if (has_layout_ && old == layout) {
    utility_->SetChromeLayout(layout, extension_bar_->HasContent());
    return;
  }
  if (!has_layout_) {
    animated = false;
  }
  layout_ = layout;
  const MotionCurve curve = motion::ChromeLayout();
  // 顶部标签布局没有侧栏：归档页、整理页收起来（下次回到侧栏是标签列表）。
  if (layout.tab_strip() && page_ != Page::kTabs) {
    ShowPage(Page::kTabs);
  }
  ApplyShape(old, animated);
  media_->SetSidebarActive(!layout.tab_strip());

  // 顶栏的高度（紧凑地址栏时跟着细栏），地址栏、扩展按钮那一行在不在。
  const double header = layout.SidebarHeaderHeight(PanelInsetFor(layout));
  const double address = layout.address_in_toolbar ? 0 : 1;
  const double extensions =
      extension_bar_->HasContent() && !layout.extensions_in_toolbar ? 1 : 0;
  if (animated) {
    header_height_.AnimateTo(header, curve);
    address_row_.AnimateTo(address, curve);
    extension_row_.AnimateTo(extensions, curve);
    transition_.Begin(curve);
  } else {
    header_height_.SetValue(header);
    address_row_.SetValue(address);
    extension_row_.SetValue(extensions);
  }
  has_layout_ = true;
  utility_->SetChromeLayout(layout, extension_bar_->HasContent());
  NotifyLayoutChanged();
}

void SidebarView::SetWindowControlsInset(const gfx::Size& size) {
  if (window_controls_inset_ == size) {
    return;
  }
  window_controls_inset_ = size;
  InvalidateLayout();
}

void SidebarView::SetUnderlaySource(WindowBackgroundView* background) {
  underlay_source_.SetView(background);
  glass_->SchedulePaint();
}

void SidebarView::SetLayoutChangedCallback(base::RepeatingClosure callback) {
  layout_changed_ = std::move(callback);
}

void SidebarView::SetExtensionsChangedCallback(
    base::RepeatingClosure callback) {
  extensions_changed_ = std::move(callback);
}

void SidebarView::SetContentOnly(bool content_only) {
  if (content_only_ == content_only) {
    return;
  }
  content_only_ = content_only;
  if (content_only_) {
    // 全屏时侧栏不显示：滑出来的收回去。
    Unpeek();
  }
}

void SidebarView::ApplyWidth(bool animated) {
  int width =
      prefs::PrefsFor(browser_->GetProfile())->GetInteger(prefs::kSidebarWidth);
  width = ClampWidth(width > 0 ? width : metrics::kSidebarWidth);
  if (resizing_) {
    return;
  }
  if (animated) {
    width_.AnimateTo(width, motion::ChromeLayout());
  } else {
    width_.SetValue(width);
  }
  NotifyLayoutChanged();
}

void SidebarView::ApplyShape(const ChromeLayout& old_layout, bool animated) {
  const MotionCurve curve = motion::ChromeLayout();
  const bool top = layout_.tab_strip();
  const bool was_top = has_layout_ && old_layout.tab_strip();
  const bool auto_hide = layout_.auto_hide;

  // 顶部标签布局下侧栏的样子不变（滑出去的时候还是原来的样子）。
  if (!top) {
    const double floating = layout_.floating() || auto_hide ? 1 : 0;
    if (animated) {
      floating_.AnimateTo(floating, curve);
    } else {
      floating_.SetValue(floating);
    }
  }

  if (top) {
    if (was_top && animated) {
      return;
    }
    // 侧栏 → 顶部标签：内容卡片铺开，侧栏向左滑出并淡出。
    pointer_timer_.Stop();
    peeking_ = false;
    auto_hide_ = false;
    LayerPose hidden;
    hidden.offset = gfx::Vector2dF(-HiddenOffset(), 0);
    if (animated) {
      column_.AnimateTo(0, curve);
      sliding_out_ = true;
      slide_motion_.AnimateTransform(
          layer(), hidden, curve, gfx::PointF(),
          base::BindOnce(
              [](base::WeakPtr<SidebarView> sidebar) {
                if (sidebar && sidebar->layout_.tab_strip() &&
                    !sidebar->slide_motion_.IsAnimatingTransform()) {
                  sidebar->sliding_out_ = false;
                  sidebar->NotifyLayoutChanged();
                }
              },
              weak_factory_.GetWeakPtr()));
      slide_motion_.AnimateOpacity(layer(), 0, curve);
    } else {
      column_.SetValue(0);
      sliding_out_ = false;
      slide_motion_.SetTransform(layer(), hidden);
      slide_motion_.SetOpacity(layer(), 0);
    }
    if (shadow_->layer()) {
      shadow_motion_.SetOpacity(shadow_->layer(), 0);
    }
    fill_->SetOverPage(false);
    UpdateGlassMode();
    return;
  }

  if (was_top) {
    // 顶部标签 → 侧栏：从左边滑进来、淡入；自动隐藏的停在收起的地方。
    auto_hide_ = auto_hide;
    fill_->SetOverPage(auto_hide_);
    if (shadow_->layer()) {
      shadow_motion_.SetOpacity(shadow_->layer(), auto_hide_ ? 1 : 0);
    }
    LayerPose hidden;
    hidden.offset = gfx::Vector2dF(-HiddenOffset(), 0);
    if (auto_hide_) {
      column_.SetValue(0);
      sliding_out_ = false;
      slide_motion_.SetTransform(layer(), hidden);
      slide_motion_.SetOpacity(layer(), 1);
    } else if (animated) {
      column_.AnimateTo(1, curve);
      sliding_out_ = false;
      slide_motion_.AnimateTransform(layer(), LayerPose(), curve);
      slide_motion_.AnimateOpacity(layer(), 1, curve);
    } else {
      column_.SetValue(1);
      slide_motion_.SetTransform(layer(), LayerPose());
      slide_motion_.SetOpacity(layer(), 1);
    }
    UpdateGlassMode();
    return;
  }

  if (auto_hide != auto_hide_ || !animated) {
    auto_hide_ = auto_hide;
    if (layer() && !animated) {
      slide_motion_.SetOpacity(layer(), 1);
    }
    if (auto_hide_) {
      // 一列 → 自动隐藏：卡片铺开，侧栏滑出去。正滑出来着的（在布局菜单里点的）先留着，
      // 鼠标离开再收。
      if (animated) {
        column_.AnimateTo(0, curve);
      } else {
        column_.SetValue(0);
      }
      if (peeking_) {
        away_since_.reset();
        pointer_timer_.Start(FROM_HERE, kPointerPoll, this,
                             &SidebarView::CheckPointer);
      } else if (animated) {
        SlideTo(/*shown=*/false, curve);
      } else {
        slide_motion_.SetTransform(
            layer(), LayerPose{gfx::Vector2dF(-HiddenOffset(), 0), 1});
      }
    } else {
      // 自动隐藏 → 一列：滑进来（已经滑出来的不动），卡片让开。
      pointer_timer_.Stop();
      peeking_ = false;
      if (animated) {
        column_.AnimateTo(1, curve);
        SlideTo(/*shown=*/true, curve);
      } else {
        column_.SetValue(1);
        slide_motion_.SetTransform(layer(), LayerPose());
      }
    }
    if (shadow_->layer()) {
      if (animated) {
        shadow_motion_.AnimateOpacity(shadow_->layer(), auto_hide_ ? 1 : 0,
                                      curve);
      } else {
        shadow_motion_.SetOpacity(shadow_->layer(), auto_hide_ ? 1 : 0);
      }
    }
    fill_->SetOverPage(auto_hide_);
  }
  UpdateGlassMode();
}

void SidebarView::UpdateGlassMode() {
  // 盖在网页上（自动隐藏）时是实时的玻璃；只在窗口的底上面时静态地画一次。
  if (auto_hide_) {
    glass_->SetUnderlay(GlassView::UnderlayPainter());
  } else {
    glass_->SetUnderlay(base::BindRepeating(&SidebarView::PaintGlassUnderlay,
                                            base::Unretained(this)));
  }
}

int SidebarView::HiddenOffset() const {
  return metrics::kFloatingSidebarInset + sidebar_width() + kHiddenExtra;
}

void SidebarView::SlideTo(bool shown, const MotionCurve& curve) {
  LayerPose pose;
  if (!shown) {
    pose.offset = gfx::Vector2dF(-HiddenOffset(), 0);
  }
  sliding_out_ = !shown;
  slide_motion_.AnimateTransform(
      layer(), pose, curve, gfx::PointF(),
      base::BindOnce(
          [](base::WeakPtr<SidebarView> sidebar, bool shown) {
            // 完成回调是排队执行的；旧的收回刚结束又滑出、收回时，不能把新动画藏掉。
            if (sidebar && !shown && sidebar->sliding_out_ &&
                !sidebar->slide_motion_.IsAnimatingTransform()) {
              sidebar->sliding_out_ = false;
              sidebar->NotifyLayoutChanged();
            }
          },
          weak_factory_.GetWeakPtr(), shown));
}

void SidebarView::Peek() {
  if (!auto_hide_ || peeking_) {
    return;
  }
  peeking_ = true;
  away_since_.reset();
  NotifyLayoutChanged();
  SlideTo(/*shown=*/true, MotionCurve::Of(motion::kSidebarPeek));
  pointer_timer_.Start(FROM_HERE, kPointerPoll, this,
                       &SidebarView::CheckPointer);
}

void SidebarView::Unpeek() {
  if (!peeking_) {
    return;
  }
  peeking_ = false;
  pointer_timer_.Stop();
  media_->SetExpanded(false);
  SlideTo(/*shown=*/false, motion::SidebarUnpeek());
  NotifyLayoutChanged();
}

void SidebarView::CheckPointer() {
  if (ShouldStayOpen()) {
    away_since_.reset();
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!away_since_) {
    away_since_ = now;
  } else if (now - *away_since_ >= kKeepAfterLeaving) {
    Unpeek();
  }
}

bool SidebarView::ShouldStayOpen() const {
  if (hold_peek_for_testing_ || tabs_->is_dragging() ||
      resize_handle_->is_dragging()) {
    return true;
  }
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    if ((view->command_bar() && view->command_bar()->GetVisible()) ||
        (view->menu_host() && view->menu_host()->is_showing()) ||
        (view->dialog_host() && view->dialog_host()->is_presenting()) ||
        view->IsDownloadsPanelOpenOrPending()) {
      return true;
    }
  }
  // 扩展的弹出框挂在侧栏的按钮上。
  if (extension_bar_->IsPopupShowing()) {
    return true;
  }
  // 整理页开着（正在问模型、等用户看预览）：是用户点了才开的，放进去或者回去以前不收。
  if (page_ == Page::kOrganize) {
    return true;
  }
  if (!parent() || !GetWidget() || !GetWidget()->IsVisible() ||
      GetWidget()->IsMinimized()) {
    return false;
  }
  gfx::Point point;
  if (pointer_for_testing_) {
    point = *pointer_for_testing_;
  } else {
    point = display::Screen::Get()->GetCursorScreenPoint();
    views::View::ConvertPointFromScreen(parent(), &point);
  }
  // 媒体坞展开后越过了侧栏的右边：鼠标在它上面也算在侧栏里。
  if (media_->GetVisible() && media_->ShouldShow()) {
    gfx::Rect media = views::View::ConvertRectToTarget(media_.get(), parent(),
                                                       media_->PanelRect());
    media.Outset(7);
    if (media.Contains(point)) {
      return true;
    }
  }
  const int right =
      x() + metrics::kFloatingSidebarInset * 2 + sidebar_width();
  return point.x() >= x() - kOutsideReach && point.x() <= right &&
         point.y() >= y() - 7 && point.y() <= bounds().bottom() + 7;
}

void SidebarView::NotifyLayoutChanged() {
  InvalidateLayout();
  if (glass_) {
    glass_->SchedulePaint();
  }
  if (layout_changed_) {
    layout_changed_.Run();
  }
}

void SidebarView::CommitAddress(const std::u16string& text) {
  if (text.empty()) {
    return;
  }
  // 和 Chrome 的地址栏同一套规则：能当网址的补全协议，其他的交给默认搜索引擎。
  AutocompleteMatch match;
  AutocompleteClassifierFactory::GetForProfile(browser_->GetProfile())
      ->Classify(text, /*in_keyword_mode=*/false,
                 /*allow_exact_keyword_match=*/true,
                 ::metrics::OmniboxEventProto::OTHER, &match,
                 /*alternate_nav_url=*/nullptr);
  if (!match.destination_url.is_valid()) {
    return;
  }
  NavigateParams params(
      browser_, match.destination_url,
      ui::PageTransitionFromInt(match.transition |
                                ui::PAGE_TRANSITION_FROM_ADDRESS_BAR));
  params.disposition = WindowOpenDisposition::CURRENT_TAB;
  Navigate(&params);
  FocusActiveTab();
}

gfx::Rect SidebarView::PanelRect() const {
  const double f = std::clamp(floating_.value(), 0.0, 1.0);
  const int inset =
      static_cast<int>(std::round(metrics::kFloatingSidebarInset * f));
  return gfx::Rect(inset, inset, sidebar_width(),
                   std::max(0, height() - 2 * inset));
}

void SidebarView::Layout(PassKey) {
  if (!matched_) {
    return;
  }
  const double f = std::clamp(floating_.value(), 0.0, 1.0);
  const gfx::Rect panel = PanelRect();

  shadow_->SetPanel(panel, metrics::kFloatingSidebarRadius);
  shadow_->SetVisible(auto_hide_ || shadow_motion_.IsAnimatingOpacity());
  const bool glass_visible = f > 0.001;
  glass_->SetVisible(glass_visible);
  fill_->SetVisible(glass_visible);
  if (glass_visible) {
    glass_->SetGlassBounds(panel);
    glass_->layer()->SetOpacity(static_cast<float>(f));
    fill_->SetBoundsRect(panel);
    fill_->layer()->SetOpacity(static_cast<float>(f));
  }
  content_->SetBoundsRect(panel);
  // 固定时拖动区在侧栏里面靠右；浮动时骑在面板边上。
  const int handle_x =
      f > 0.5 ? panel.right() - metrics::kSidebarResizeHandleWidth / 2
              : panel.right() - metrics::kSidebarResizeHandleWidth;
  resize_handle_->SetBounds(handle_x, panel.y(),
                            metrics::kSidebarResizeHandleWidth,
                            panel.height());
  resize_handle_->SetLineInset(f > 0.5 ? metrics::kFloatingSidebarRadius : 12);

  // 归档按钮在顶栏最右边，和导航按钮同一行。
  const int header = header_height();
  const int button = metrics::kToolButtonSize;
  const int right = panel.width() - metrics::kSidebarPadding;
  int nav_right = right;
  if (archive_button_->GetVisible()) {
    const gfx::Size size = archive_button_->GetPreferredSize();
    archive_button_->SetBounds(right - size.width(),
                               (header - size.height()) / 2, size.width(),
                               size.height());
    nav_right -= size.width() + NavigationButtons::kSpacing;
  }
  // macOS 左边让开红绿灯、导航按钮靠右；Windows 的导航按钮靠左。
  const int nav_width = NavigationButtons::Width();
  const int nav_x = WindowsControlsEnabled() ? metrics::kSidebarPadding
                                             : nav_right - nav_width;
  matched_->Place(MatchedGeometry::Id::kNavigation, nav_,
                  gfx::Rect(nav_x, header / 2 - button / 2, nav_width, button),
                  !layout_.toolbar_navigation);

  int y = header;
  const int inner_width = panel.width() - 2 * metrics::kSidebarPadding;
  matched_->Place(MatchedGeometry::Id::kAddress, address_pill_,
                  gfx::Rect(metrics::kSidebarPadding, y, inner_width,
                            metrics::kAddressPillHeight),
                  !layout_.address_in_toolbar);
  y += static_cast<int>(
      std::round((metrics::kAddressPillHeight + kAddressBottomGap) *
                 std::clamp(address_row_.value(), 0.0, 1.0)));
  const bool extensions =
      extension_bar_->HasContent() && !layout_.extensions_in_toolbar;
  transition_.Place(extension_bar_,
                    gfx::Rect(metrics::kSidebarPadding, y, inner_width,
                              ExtensionBar::kHeight),
                    extensions);
  if (extension_bar_->HasContent()) {
    y += static_cast<int>(
        std::round((ExtensionBar::kHeight + kExtensionBottomGap) *
                   std::clamp(extension_row_.value(), 0.0, 1.0)));
  }
  const int row_height =
      utility_->HasContent() ? kToolRowHeight : kToolRowCollapsedHeight;
  const int tools_top = panel.height() - row_height;
  // 媒体坞占的位置：动画里是中间值。坞的视图一直是展开后那么大（高最多到标签列表的
  // 顶上，宽可以越过侧栏、不出窗口），左下角对齐卡片的位置；展开、收回是它自己里面的
  // 事，这里不用跟着重新排。它挂在侧栏这一层，坐标要加上面板的位置。
  const double media = std::clamp(media_row_.value(), 0.0, 1.0);
  const int media_slot = static_cast<int>(std::round(
      (MediaDock::kSlotHeight + kMediaTopGap + kMediaBottomGap) * media));
  media_->SetVisible(media > 0.001);
  if (media_->GetVisible()) {
    media_->SetMaxHeight(tools_top - kMediaBottomGap - y);
    const int media_x = panel.x() + metrics::kSidebarPadding;
    const int max_width =
        parent() ? std::max(inner_width, parent()->width() - x() - media_x -
                                             metrics::kContentInset)
                 : inner_width;
    media_->SetWidths(inner_width, max_width);
    const int media_height = media_->FullHeight();
    media_->SetBounds(media_x,
                      panel.y() + tools_top - kMediaBottomGap - media_height +
                          (MediaDock::kSlotHeight + kMediaTopGap +
                           kMediaBottomGap - media_slot),
                      media_->ExpandedWidth(), media_height);
  }
  tabs_->SetBounds(metrics::kSidebarPadding, y, inner_width,
                   std::max(0, tools_top - media_slot - y));
  archive_page_->SetBoundsRect(tabs_->bounds());
  organize_page_->SetBoundsRect(tabs_->bounds());
  utility_->SetVisible(utility_->HasContent());
  utility_->SetBounds(metrics::kSidebarPadding, tools_top, inner_width,
                      row_height);
}

gfx::Rect SidebarView::MediaDockCardBounds(const views::View* target) const {
  if ((auto_hide_ && !peeking_) || layout_.tab_strip()) {
    return gfx::Rect();
  }
  const gfx::Rect panel = PanelRect();
  const int row_height =
      utility_->HasContent() ? kToolRowHeight : kToolRowCollapsedHeight;
  const int tools_top = panel.height() - row_height;
  const gfx::Rect card(metrics::kSidebarPadding,
                       tools_top - kMediaBottomGap - MediaDock::kCardHeight,
                       panel.width() - 2 * metrics::kSidebarPadding,
                       MediaDock::kCardHeight);
  return views::View::ConvertRectToTarget(content_.get(), target, card);
}

views::View* SidebarView::PageView(Page page) {
  switch (page) {
    case Page::kArchive:
      return archive_page_;
    case Page::kOrganize:
      return organize_page_;
    case Page::kTabs:
      break;
  }
  return tabs_;
}

LayerMotion& SidebarView::PageMotion(Page page) {
  switch (page) {
    case Page::kArchive:
      return archive_motion_;
    case Page::kOrganize:
      return organize_motion_;
    case Page::kTabs:
      break;
  }
  return tabs_motion_;
}

void SidebarView::ShowArchive(bool open) {
  if (open) {
    ShowPage(Page::kArchive);
  } else if (page_ == Page::kArchive) {
    ShowPage(Page::kTabs);
  }
}

void SidebarView::ShowOrganize(bool open) {
  if (open) {
    ShowPage(Page::kOrganize);
  } else if (page_ == Page::kOrganize) {
    ShowPage(Page::kTabs);
  }
}

void SidebarView::ShowPage(Page page) {
  if (page != Page::kTabs && layout_.tab_strip()) {
    return;
  }
  if (page_ == page) {
    if (page == Page::kArchive) {
      archive_page_->FocusSearch();
    } else if (page == Page::kOrganize) {
      organize_page_->RequestFocus();
    }
    return;
  }
  const Page old = page_;
  page_ = page;
  archive_button_->SetTabsPageVisible(page == Page::kTabs);
  views::View* from = PageView(old);
  views::View* to = PageView(page);
  const bool animated =
      !motion::ReduceMotion() && GetWidget() && GetVisible() && IsDrawn();
  // 翻到一页：它从右边 24 处推进来、淡入，原来的往左让 24、淡出（同一条弹簧）；回到标签
  // 反过来。
  const bool back = page == Page::kTabs;
  const MotionCurve slide = MotionCurve::Of(motion::kFolderOpen);
  const MotionCurve fade = MotionCurve::EaseOut(0.16);
  LayerPose left;
  left.offset = gfx::Vector2dF(-24, 0);
  LayerPose right;
  right.offset = gfx::Vector2dF(24, 0);
  const bool had_focus = from->Contains(
      GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr);
  if (old == Page::kOrganize) {
    organize_page_->Closed();
  }
  if (page == Page::kArchive) {
    archive_page_->Prepare();
  } else if (page == Page::kOrganize) {
    organize_page_->Prepare();
  }
  to->SetBoundsRect(tabs_->bounds());
  to->SetVisible(true);
  tabs_->SetCanProcessEventsWithinSubtree(back);
  LayerMotion& from_motion = PageMotion(old);
  LayerMotion& to_motion = PageMotion(page);
  if (animated) {
    to_motion.AnimateTransformFrom(to->layer(), back ? left : right,
                                   LayerPose(), slide);
    to_motion.AnimateOpacityFrom(to->layer(), 0, 1, fade);
    from_motion.AnimateTransform(from->layer(), back ? right : left, slide);
    from_motion.AnimateOpacity(
        from->layer(), 0, fade,
        base::BindOnce(
            [](base::WeakPtr<SidebarView> sidebar, Page gone) {
              // 动完时又翻回来了的不管。
              if (sidebar && sidebar->page_ != gone) {
                sidebar->PageView(gone)->SetVisible(false);
              }
            },
            weak_factory_.GetWeakPtr(), old));
  } else {
    to_motion.SetTransform(to->layer(), LayerPose());
    to_motion.SetOpacity(to->layer(), 1);
    from_motion.SetOpacity(from->layer(), 0);
    from->SetVisible(false);
  }
  if (page == Page::kArchive) {
    archive_page_->FocusSearch();
  } else if (page == Page::kOrganize) {
    organize_page_->RequestFocus();
  } else if (had_focus) {
    FocusActiveTab();
  }
}

void SidebarView::RemovedFromWidget() {
  pointer_timer_.Stop();
}

bool SidebarView::DoesIntersectRect(const views::View* target,
                                    const gfx::Rect& rect) const {
  // 收起、正在收回的侧栏不挡鼠标；顶部标签布局下没有侧栏。
  if ((auto_hide_ && !peeking_) || layout_.tab_strip()) {
    return false;
  }
  if (GetLocalBounds().Intersects(rect)) {
    return true;
  }
  // 展开的媒体坞越过了侧栏的右边：那一截也接鼠标。
  return media_->GetVisible() &&
         media_->HitTestRect(
             views::View::ConvertRectToTarget(this, media_.get(), rect));
}

void SidebarView::UpdateColors() {
  nav_->SetTheme(theme_);
  archive_button_->SetTheme(theme_);
  address_pill_->SetTheme(theme_);
  utility_->SetTheme(theme_);
  media_->SetTheme(theme_);
  extension_bar_->SetTheme(theme_);
  tabs_->SetTheme(theme_);
  archive_page_->SetTheme(theme_);
  organize_page_->SetTheme(theme_);
  shadow_->SetTheme(theme_);
  fill_->SetTheme(theme_);
  resize_handle_->SetTheme(theme_);
  GlassStyle style;
  style.dark = theme_.is_dark;
  // 大玻璃不按亮度变深浅，只用它决定亮暗两种风格：跟着界面的深浅。
  style.luma = theme_.is_dark ? 0.1f : 0.9f;
  glass_->SetStyle(style);
  glass_->SchedulePaint();
}

void SidebarView::PaintGlassUnderlay(gfx::Canvas* canvas,
                                     const gfx::Rect& bounds) {
  auto* background =
      static_cast<WindowBackgroundView*>(underlay_source_.view());
  if (!background) {
    return;
  }
  gfx::RectF rect(bounds);
  views::View::ConvertRectToTarget(glass_, background, &rect);
  background->PaintUnderlay(canvas, gfx::ToEnclosingRect(rect));
}

void SidebarView::FocusActiveTab() {
  if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
    tab->GetContents()->Focus();
  }
}

BEGIN_METADATA(SidebarView)
END_METADATA

}  // namespace starry
