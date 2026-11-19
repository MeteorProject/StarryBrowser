// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/toolbar/compact_toolbar.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "cc/paint/paint_flags.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/sidebar/extension_bar.h"
#include "starry/browser/ui/sidebar/utility_buttons.h"
#include "starry/browser/ui/toolbar/address_pill.h"
#include "starry/browser/ui/toolbar/navigation_buttons.h"
#include "starry/browser/ui/toolbar/toolbar_parts.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"

namespace starry {

namespace {

constexpr int kSpacing = 4;
constexpr int kPadding = 4;
constexpr int kAddressHeight = 28;
// 第一次等网页画出来，第二次补上载入完才由脚本画出的顶栏。
constexpr base::TimeDelta kFirstSample = base::Milliseconds(150);
constexpr base::TimeDelta kSecondSample = base::Milliseconds(1000);

}  // namespace

CompactToolbar::CompactToolbar(BrowserWindowInterface* browser,
                               MatchedGeometry* matched)
    : browser_(browser),
      matched_(matched->GetWeakPtr()),
      transition_(base::BindRepeating(
          [](views::View* view) { view->InvalidateLayout(); },
          base::Unretained(this))),
      style_motion_(1,
                    base::BindRepeating(&CompactToolbar::ApplyStyle,
                                        base::Unretained(this))) {
  // 自己的图层：从卡片顶上移入、淡入；空白处可以拖动窗口。
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetWindowDraggable(this, true);
  shown_.theme = window_theme_;
  target_ = from_ = shown_;

  nav_ = AddChildView(std::make_unique<NavigationButtons>(browser));
  address_ = AddChildView(
      std::make_unique<AddressPill>(browser, AddressPill::Style::kCompact));
  ext_ = AddChildView(
      std::make_unique<ExtensionBar>(browser, ChromePlacement::kToolbar));
  ext_->SetPaintToLayer();
  ext_->layer()->SetFillsBoundsOpaquely(false);
  ext_->SetContentChangedCallback(
      base::BindRepeating([](views::View* view) { view->InvalidateLayout(); },
                          base::Unretained(this)));
  utility_separator_ = AddChildView(std::make_unique<ToolbarSeparator>());
  utility_ = AddChildView(
      std::make_unique<UtilityButtons>(browser, ChromePlacement::kToolbar));
  utility_->SetPaintToLayer();
  utility_->layer()->SetFillsBoundsOpaquely(false);
  utility_->SetContentChangedCallback(
      base::BindRepeating([](views::View* view) { view->InvalidateLayout(); },
                          base::Unretained(this)));
  assistant_separator_ = AddChildView(std::make_unique<ToolbarSeparator>());
  assistant_ = AddChildView(std::make_unique<AssistantButton>(browser));
  controls_separator_ = AddChildView(std::make_unique<ToolbarSeparator>());
  for (views::View* child : children()) {
    child->SetVisible(false);
  }
  ApplyStyle();
}

CompactToolbar::~CompactToolbar() {
  if (!matched_) {
    return;
  }
  for (views::View* child : children()) {
    matched_->Forget(child);
  }
}

void CompactToolbar::SetWindowTheme(const Theme& theme) {
  if (window_theme_ == theme) {
    return;
  }
  window_theme_ = theme;
  // 不跟网页时就是窗口的配色：直接换（窗口自己的变色已经在动了）。
  target_ =
      CompactToolbarStyle::For(tab_.Get() ? tab_.Get()->GetContents() : nullptr,
                               window_theme_, follows_page_);
  if (!style_motion_.is_animating()) {
    shown_ = from_ = target_;
    ApplyStyle();
  }
}

void CompactToolbar::SetFollowsPage(bool follows_page) {
  if (follows_page_ == follows_page) {
    return;
  }
  follows_page_ = follows_page;
  UpdateStyle(/*tab_changed=*/false);
  ScheduleSample();
}

void CompactToolbar::SetChromeLayout(const ChromeLayout& layout,
                                     bool assistant_shown,
                                     bool animated) {
  utility_->SetChromeLayout(layout, ext_->HasContent());
  if (layout_ == layout && assistant_shown_ == assistant_shown) {
    return;
  }
  if (animated && GetVisible()) {
    transition_.Begin(motion::ChromeLayout());
  }
  const bool became_compact = layout.compact && !layout_.compact;
  layout_ = layout;
  assistant_shown_ = assistant_shown;
  InvalidateLayout();
  if (became_compact) {
    ScheduleSample();
  }
}

void CompactToolbar::SetPane(tabs::TabInterface* tab,
                             bool focused,
                             bool split) {
  const tabs::TabHandle handle = tab ? tab->GetHandle() : tabs::TabHandle();
  const bool tab_changed = handle != tab_;
  if (!tab_changed && focused_ == focused && split_ == split) {
    return;
  }
  tab_ = handle;
  focused_ = focused;
  split_ = split;
  Observe(tab ? tab->GetContents() : nullptr);
  address_->SetPaneTab(tab ? std::optional<tabs::TabHandle>(handle)
                           : std::nullopt);
  InvalidateLayout();
  if (tab_changed) {
    UpdateStyle(/*tab_changed=*/true);
    ScheduleSample();
  }
}

void CompactToolbar::SetTrailingInset(int inset) {
  inset = std::max(0, inset);
  if (trailing_inset_ == inset) {
    return;
  }
  trailing_inset_ = inset;
  InvalidateLayout();
}

void CompactToolbar::SetStyleChangedCallback(base::RepeatingClosure callback) {
  style_changed_ = std::move(callback);
}

void CompactToolbar::UpdateStyle(bool tab_changed) {
  const CompactToolbarStyle target =
      CompactToolbarStyle::For(web_contents(), window_theme_, follows_page_);
  if (target == target_ && (style_motion_.is_animating() || shown_ == target)) {
    return;
  }
  target_ = target;
  from_ = shown_;
  if (!GetWidget() || !GetVisible()) {
    shown_ = from_ = target_;
    style_motion_.SetValue(1);
    ApplyStyle();
    return;
  }
  // 换标签时网页直接换了，底色和地址要马上跟上；同一个网页里变色才慢慢变。
  style_motion_.SetValue(0);
  style_motion_.AnimateTo(
      1, tab_changed ? motion::AddressSwap() : motion::ChromeTint());
}

void CompactToolbar::ApplyStyle() {
  shown_ = CompactToolbarStyle::Lerp(from_, target_, style_motion_.value());
  const Theme& theme = shown_.theme;
  nav_->SetTheme(theme);
  address_->SetTheme(theme);
  ext_->SetTheme(theme);
  utility_separator_->SetTheme(theme);
  utility_->SetTheme(theme);
  assistant_separator_->SetTheme(theme);
  assistant_->SetTheme(theme);
  controls_separator_->SetTheme(theme);
  SchedulePaint();
  if (style_changed_) {
    style_changed_.Run();
  }
}

void CompactToolbar::ScheduleSample() {
  first_sample_.Stop();
  second_sample_.Stop();
  if (!follows_page_ || !layout_.compact || !web_contents()) {
    return;
  }
  first_sample_.Start(
      FROM_HERE, kFirstSample,
      base::BindOnce(&CompactToolbar::Sample, weak_factory_.GetWeakPtr()));
  second_sample_.Start(
      FROM_HERE, kSecondSample,
      base::BindOnce(&CompactToolbar::Sample, weak_factory_.GetWeakPtr()));
}

void CompactToolbar::Sample() {
  content::WebContents* contents = web_contents();
  if (!contents || !CanTintFromPage(contents) || !GetVisible()) {
    return;
  }
  SamplePageTopColor(contents,
                     base::BindOnce(
                         [](base::WeakPtr<CompactToolbar> bar,
                            base::WeakPtr<content::WebContents> contents,
                            std::optional<SkColor> color) {
                           if (!contents) {
                             return;
                           }
                           if (PageTopColor::Get(contents.get()) == color &&
                               color.has_value()) {
                             return;
                           }
                           PageTopColor::Set(contents.get(), color);
                           if (bar && bar->web_contents() == contents.get()) {
                             bar->UpdateStyle(/*tab_changed=*/false);
                           }
                         },
                         weak_factory_.GetWeakPtr(), contents->GetWeakPtr()));
}

void CompactToolbar::DidChangeThemeColor() {
  UpdateStyle(/*tab_changed=*/false);
  ScheduleSample();
}

void CompactToolbar::OnBackgroundColorChanged() {
  UpdateStyle(/*tab_changed=*/false);
  ScheduleSample();
}

void CompactToolbar::DidStartLoading() {
  UpdateStyle(/*tab_changed=*/false);
  ScheduleSample();
}

void CompactToolbar::DidStopLoading() {
  UpdateStyle(/*tab_changed=*/false);
  ScheduleSample();
}

void CompactToolbar::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  // 单页应用在同一个网页里换了地址：重新取色（载入中的地址变化不算，那时新网页可能还没
  // 画出来，载入完会再取）。
  if (navigation_handle->IsInPrimaryMainFrame() &&
      navigation_handle->IsSameDocument() &&
      navigation_handle->HasCommitted()) {
    ScheduleSample();
  }
}

void CompactToolbar::PrimaryPageChanged(content::Page& page) {
  // 换了网页：上一个网页的取色不能再用。
  if (web_contents()) {
    PageTopColor::Set(web_contents(), std::nullopt);
  }
  UpdateStyle(/*tab_changed=*/false);
}

void CompactToolbar::VisibilityChanged(views::View* starting_from,
                                       bool is_visible) {
  if (is_visible) {
    UpdateStyle(/*tab_changed=*/true);
    ScheduleSample();
  }
}

void CompactToolbar::ChildPreferredSizeChanged(views::View* child) {
  InvalidateLayout();
}

void CompactToolbar::Layout(PassKey) {
  if (!matched_) {
    return;
  }
  const int h = height();
  const int button = metrics::kToolButtonSize;
  const int y = (h - button) / 2;
  const bool bar = layout_.compact;
  const bool nav = bar && focused_ && layout_.toolbar_navigation;
  const bool ext =
      bar && focused_ && layout_.extensions_in_toolbar && ext_->HasContent();
  const bool utility =
      bar && focused_ && layout_.toolbar_utilities() && utility_->HasContent();
  const bool assistant = bar && focused_ && assistant_shown_ &&
                         layout_.assistant_placement() ==
                             ChromeLayout::AssistantPlacement::kToolbar;

  int right = width() - kPadding;
  const bool controls_line = bar && trailing_inset_ > 0;
  gfx::Rect controls_line_rect;
  if (trailing_inset_ > 0) {
    right -= trailing_inset_;
    controls_line_rect =
        gfx::Rect(right - ToolbarSeparator::kWidth, 0,
                  ToolbarSeparator::kWidth, h);
    right -= ToolbarSeparator::kWidth + kSpacing;
  }
  const gfx::Rect assistant_rect(right - button, y, button, button);
  if (assistant) {
    right -= button + kSpacing;
  }
  const bool assistant_line = assistant && ext;
  const gfx::Rect assistant_line_rect(right - ToolbarSeparator::kWidth, 0,
                                      ToolbarSeparator::kWidth, h);
  if (assistant_line) {
    right -= ToolbarSeparator::kWidth + kSpacing;
  }
  const gfx::Rect utility_rect(0, y, std::max(0, right), button);
  if (utility) {
    right -= utility_->PreferredWidth() + kSpacing;
  }
  const bool utility_line = utility && ext;
  const gfx::Rect utility_line_rect(right - ToolbarSeparator::kWidth, 0,
                                    ToolbarSeparator::kWidth, h);
  if (utility_line) {
    right -= ToolbarSeparator::kWidth + kSpacing;
  }
  const int ext_width = ext_->PreferredWidth();
  const gfx::Rect ext_rect(right - ext_width, y, ext_width, button);
  if (ext) {
    right -= ext_width + kSpacing;
  }
  const int nav_width = NavigationButtons::Width();
  const gfx::Rect nav_rect(kPadding, y, nav_width, button);
  const int x = kPadding + (nav ? nav_width + kSpacing : 0);
  const gfx::Rect address_rect(x, (h - kAddressHeight) / 2,
                               std::max(0, right - x), kAddressHeight);

  // 分屏时同一个东西有好几条：不和侧栏、工具栏里的一起飞。
  if (split_) {
    transition_.Place(nav_, nav_rect, nav);
    transition_.Place(address_, address_rect, bar);
    transition_.Place(assistant_, assistant_rect, assistant);
  } else {
    matched_->Place(MatchedGeometry::Id::kNavigation, nav_, nav_rect, nav);
    matched_->Place(MatchedGeometry::Id::kAddress, address_, address_rect, bar);
    matched_->Place(MatchedGeometry::Id::kAssistant, assistant_, assistant_rect,
                    assistant);
  }
  transition_.Place(ext_, ext_rect, ext);
  transition_.Place(utility_separator_, utility_line_rect, utility_line);
  transition_.Place(utility_, utility_rect, utility);
  transition_.Place(assistant_separator_, assistant_line_rect, assistant_line);
  transition_.Place(controls_separator_, controls_line_rect, controls_line);
}

void CompactToolbar::OnPaint(gfx::Canvas* canvas) {
  // 底色（上边的圆角同内容卡片）和网页之间一条很淡的线：颜色相同时也看得出地址栏的边界。
  const float radius = radius::kContent;
  const SkVector radii[4] = {
      {radius, radius}, {radius, radius}, {0, 0}, {0, 0}};
  SkRRect rrect;
  rrect.setRectRadii(gfx::RectToSkRect(GetLocalBounds()), radii);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(shown_.background);
  canvas->sk_canvas()->drawRRect(rrect, flags);
  cc::PaintFlags line;
  line.setColor(shown_.theme.OnSurface(shown_.theme.is_dark ? 0.08f : 0.06f));
  canvas->DrawRect(gfx::Rect(0, height() - 1, width(), 1), line);
}

BEGIN_METADATA(CompactToolbar)
END_METADATA

}  // namespace starry
