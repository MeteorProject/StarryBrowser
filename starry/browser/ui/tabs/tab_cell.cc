// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tab_cell.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/views/tabs/tab/tab_icon.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_alert.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/agent/agent_activity.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/tab_organizer/organizer_memory.h"
#include "starry/browser/ui/assistant/agent_ring.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/media/media_waveform.h"
#include "starry/browser/ui/motion/frame_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "starry/browser/ui/tabs/tab_list_view.h"
#include "starry/common/internal_urls.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace starry {

namespace {

// 侧栏的行：左 10 右 6，元素间距 10。顶部的标签：左 10 右 5，间距 8。图块：图标
// 18，状 态带 4 的边距；顶部标签行里的图块图标 16，状态小一些。
constexpr int kRowLeading = 10;
constexpr int kRowTrailing = 6;
constexpr int kRowSpacing = 10;
constexpr int kTopRowTrailing = 5;
constexpr int kTopRowSpacing = 8;
constexpr int kTileIconSize = 18;
constexpr int kTopTileIconSize = 16;
constexpr int kCloseButtonSize = 18;
constexpr int kCloseGlyphSize = 10;
constexpr int kAudioGlyphSize = 12;
constexpr int kAudioButtonSize = 16;
constexpr int kCaptureDotSize = 6;
constexpr int kSleepGlyphSize = 10;
// 「放进文件夹」的提示：高 20，左右 6，文件夹图标 11，图标和名字之间 4，名字最宽 64。
constexpr int kSuggestHeight = 20;
constexpr int kSuggestPadding = 6;
constexpr int kSuggestIcon = 11;
constexpr int kSuggestGap = 4;
constexpr int kSuggestMaxText = 64;
// 按住移动这么远就是拖动。
constexpr int kDragThreshold = 4;

std::u16string DisplayTitle(const tabs::TabData& data) {
  return TabDisplayTitle(data);
}

// 图块右下角分屏的小标。
constexpr int kSplitBadgeSize = 8;

// 树里折着的父标签：右边的数字（高 16、左右各 5，至少 18 宽），前面叠着前几个子标签
// 的图标（14 大、后一个压住前一个 5、之间留一圈 1.5 的空，同折叠的文件夹），行至少
// 这么宽时才叠图标。箭头 12，点的范围比图标大一圈。
constexpr int kTreeCountHeight = 16;
constexpr int kTreeCountPadding = 5;
constexpr int kTreeCountMinWidth = 18;
constexpr int kTreeCountFontSize = 10;
constexpr int kTreeStackIcon = 14;
constexpr int kTreeStackStep = 9;
constexpr size_t kTreeStackMax = 3;
constexpr float kTreeStackGap = 1.5f;
constexpr int kTreeStackCountGap = 4;
constexpr int kTreeStackMinWidth = 176;
constexpr int kTreeChevronSize = 12;
constexpr int kTreeToggleOutset = 6;
// 悬停父标签：网站图标缩到这么大淡出，箭头从这么大放大淡入。
constexpr float kTreeHoverIconScale = 0.6f;
// 折着的一组里多了标签：数字放大到这么大再回来。
constexpr float kTreeCountBumpScale = 1.28f;
constexpr double kTreeCountBumpDuration = 0.42;

}  // namespace

bool TabCellDelegate::TabCellShowsTooltip() {
  return true;
}

bool TabCellDelegate::TabListKeyPressed(views::View* item,
                                        const ui::KeyEvent& event) {
  return false;
}

void PaintFocusRing(gfx::Canvas* canvas,
                    const Theme& theme,
                    gfx::RectF rect,
                    float radius) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setStyle(cc::PaintFlags::kStroke_Style);
  flags.setStrokeWidth(2);
  flags.setColor(SkColorSetA(theme.accent, 0xE0));
  rect.Inset(1);
  canvas->DrawRoundRect(rect, std::max(0.0f, radius - 1), flags);
}

tabs::TabData StarryTabData(tabs::TabInterface* tab) {
  tabs::TabData data = tabs::TabData::FromTabInterface(tab);
  auto name = pinned_tabs::DisplayName(tab);
  if (!name.empty()) {
    data.title = std::move(name);
  }
  if (const gfx::VectorIcon* icon =
          InternalPageIcon(InternalPageForUrl(data.visible_url))) {
    data.favicon = ui::ImageModel::FromVectorIcon(*icon, ui::kColorMenuIcon,
                                                  metrics::kFaviconSize);
    data.should_themify_favicon = false;
  }
  return data;
}

std::u16string TabDisplayTitle(const tabs::TabData& data) {
  const GURL& url = data.visible_url;
  const bool blank = IsBlankTabUrl(url);
  if (data.title.empty() || (blank && !data.should_render_loading_title)) {
    return blank || data.title.empty() ? L("tabs.untitled") : data.title;
  }
  return data.title;
}

TabCell::TabCell(TabCellStyle style, TabCellDelegate* delegate)
    : AnimationDelegateViews(this), style_(style), delegate_(delegate) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetNotifyEnterExitOnChild(true);
  set_context_menu_controller(this);
  SetWindowDraggable(this, false);
  hover_animation_.SetSlideDuration(
      base::Milliseconds(motion::kHoverDuration * 1000));
  hover_animation_.SetTweenType(gfx::Tween::EASE_OUT);
  drop_animation_.SetSlideDuration(
      base::Milliseconds(motion::kHoverDuration * 1000));
  drop_animation_.SetTweenType(gfx::Tween::EASE_OUT);

  icon_holder_ = AddChildView(std::make_unique<views::View>());
  icon_holder_->SetCanProcessEventsWithinSubtree(false);
  icon_ = icon_holder_->AddChildView(std::make_unique<TabIcon>());

  audio_button_ = AddChildView(std::make_unique<GlyphButton>(
      base::BindRepeating(
          [](TabCell* cell) { cell->delegate_->TabCellMuteToggled(cell); },
          base::Unretained(this)),
      icons::kSpeakerWave, kAudioGlyphSize, kAudioButtonSize));
  audio_button_->SetVisible(false);
  EnsureCloseButton();

  GetViewAccessibility().SetRole(ax::mojom::Role::kTab);
  SetFocusBehavior(FocusBehavior::ALWAYS);
  agent_subscription_ = agent::AgentActivities::Get()->AddObserver(
      base::BindRepeating(&TabCell::UpdateAgent, base::Unretained(this)));
}

TabCell::~TabCell() = default;

void TabCell::EnsureCloseButton() {
  if (close_button_ || is_tile()) {
    return;
  }
  close_button_ = AddChildView(std::make_unique<GlyphButton>(
      base::BindRepeating(
          [](TabCell* cell) {
            cell->delegate_->TabCellCloseRequested(cell, true);
          },
          base::Unretained(this)),
      icons::kXmarkBold, kCloseGlyphSize, kCloseButtonSize));
  close_button_->SetTooltipText(L("tabs.close"));
  close_button_->GetViewAccessibility().SetName(L("tabs.close"));
  close_button_->SetFillRadius(5);
  close_button_->SetVisible(false);
}

void TabCell::SetStyle(TabCellStyle style) {
  if (style_ == style) {
    return;
  }
  style_ = style;
  // 关闭按钮的提示、颜色在接着的 SetData 里设。
  EnsureCloseButton();
  UpdateButtons();
  InvalidateLayout();
  SchedulePaint();
}

void TabCell::SetData(tabs::TabHandle handle,
                      const tabs::TabData& data,
                      bool selected,
                      const Theme& theme) {
  handle_ = handle;
  tabs::TabInterface* tab = handle_.Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  PrefService* media_prefs =
      contents ? prefs::PrefsFor(
                     Profile::FromBrowserContext(contents->GetBrowserContext()))
               : nullptr;
  if (media_prefs_.prefs() != media_prefs) {
    media_prefs_.RemoveAll();
    if (media_prefs) {
      media_prefs_.Init(media_prefs);
      media_prefs_.Add(prefs::kMediaTabMeters, base::BindRepeating(
                                                   [](TabCell* cell) {
                                                     cell->UpdateMeter();
                                                     cell->UpdateButtons();
                                                     cell->InvalidateLayout();
                                                     cell->SchedulePaint();
                                                   },
                                                   base::Unretained(this)));
    }
  }
  const bool theme_changed = !(theme_ == theme);
  const bool selection_changed = selected_ != selected;
  data_ = data;
  selected_ = selected;
  theme_ = theme;

  icon_->SetData(data);
  icon_->SetActiveState(selected);
  // 侧栏的普通标签：按网站的规则提示放进哪个文件夹。
  std::u16string suggestion;
  std::optional<SkColor> tint;
  if (style_ == TabCellStyle::kRow) {
    if (const std::optional<tab_organizer::Suggestion> found =
            tab_organizer::SuggestFolder(tab)) {
      suggestion = found->name;
      tint = pin_folders::ColorValue(found->color, theme.is_dark);
    }
  }
  if (suggestion != suggest_name_ || tint != suggest_tint_) {
    suggest_name_ = std::move(suggestion);
    suggest_tint_ = tint;
    if (suggest_name_.empty()) {
      suggest_hovered_ = false;
    }
    InvalidateLayout();
  }
  // 休眠标签的图标保持显示但半透明变淡。
  icon_holder_->SetVisible(true);

  audio_button_->SetIcon(Muted() ? icons::kSpeakerSlash : icons::kSpeakerWave);
  audio_button_->SetColors(theme.on_surface_variant, theme.OnSurface(0.08f));
  audio_button_->SetTooltipText(Muted() ? L("tabs.unmute") : L("tabs.mute"));
  if (close_button_) {
    const bool pin = tab && tab->IsPinned();
    const bool archive = pin && contents->WasDiscarded();
    const auto title =
        pin ? (archive ? L("tabs.archive") : L("tabs.closeKeepPin"))
            : L("tabs.close");
    close_button_->SetTooltipText(title);
    close_button_->GetViewAccessibility().SetName(title);
    close_button_->SetIcon(pin && !archive ? icons::kMinus : icons::kXmarkBold);
    close_button_->SetColors(theme.on_surface_variant, theme.OnSurface(0.08f));
  }

  UpdateAgent();
  UpdateAccessibleName();
  GetViewAccessibility().SetIsSelected(selected);
  UpdateMeter();
  UpdateButtons();
  if (theme_changed || selection_changed) {
    SchedulePaint();
  }
  InvalidateLayout();
  SchedulePaint();
}

void TabCell::SetSplit(bool in_split, bool active_group) {
  if (in_split_ == in_split && split_active_ == active_group) {
    return;
  }
  in_split_ = in_split;
  split_active_ = active_group;
  InvalidateLayout();
  SchedulePaint();
}

void TabCell::SetTree(bool parent,
                      bool collapsed,
                      int count,
                      std::vector<ui::ImageModel> icons) {
  if (is_tile() || is_top()) {
    parent = false;
  }
  collapsed = collapsed && parent;
  const bool changed = tree_parent_ != parent || tree_collapsed_ != collapsed ||
                       tree_count_ != count ||
                       tree_icons_.size() != icons.size();
  ui::Compositor* compositor =
      GetWidget() && IsDrawn() ? GetWidget()->GetCompositor() : nullptr;
  // 开合：箭头转过去（折着是 -90°）。
  if (parent && tree_parent_ && collapsed != tree_collapsed_) {
    tree_fold_.AnimateTo(compositor, collapsed ? 1 : 0,
                         MotionCurve::Of(motion::kLift));
  } else if (!tree_fold_.is_animating()) {
    tree_fold_.SetValue(collapsed ? 1 : 0);
  }
  // 折着的一组里多了标签：数字跳一下。
  if (collapsed && tree_collapsed_ && count > tree_count_ &&
      !motion::ReduceMotion()) {
    tree_bump_.SetValue(0);
    tree_bump_.AnimateTo(compositor, 1,
                         MotionCurve::EaseOut(kTreeCountBumpDuration));
  }
  tree_parent_ = parent;
  tree_collapsed_ = collapsed;
  tree_count_ = count;
  tree_icons_ = std::move(icons);
  UpdateIconFade();
  if (changed) {
    UpdateAccessibleName();
    InvalidateLayout();
  }
  SchedulePaint();
}

void TabCell::UpdateIconFade() {
  const float hover = static_cast<float>(hover_animation_.GetCurrentValue());
  const float opacity = tree_parent_ ? 1 - hover : 1;
  if (opacity < 1 && !icon_holder_->layer()) {
    icon_holder_->SetPaintToLayer();
    icon_holder_->layer()->SetFillsBoundsOpaquely(false);
  }
  if (icon_holder_->layer()) {
    icon_holder_->layer()->SetOpacity(opacity);
  }
  // 父标签悬停时网站图标还缩到 0.6（点击的弹一下放着时不管）。
  const float scale =
      tree_parent_ ? 1 - (1 - kTreeHoverIconScale) * hover : 1.0f;
  if (icon_holder_->layer() && scale != icon_scale_ &&
      !bounce_motion_.IsAnimatingTransform()) {
    icon_scale_ = scale;
    LayerPose pose;
    pose.scale = scale;
    bounce_motion_.SetTransform(icon_holder_->layer(), pose,
                                gfx::PointF(icon_holder_->width() / 2.0f,
                                            icon_holder_->height() / 2.0f));
  }
}

gfx::Rect TabCell::TreeToggleBounds() const {
  gfx::Rect bounds = icon_holder_->bounds();
  bounds.Outset(kTreeToggleOutset);
  return bounds;
}

void TabCell::PaintTree(gfx::Canvas* canvas) {
  if (tree_parent_) {
    const float hover = static_cast<float>(hover_animation_.GetCurrentValue());
    if (hover > 0) {
      // 箭头从 0.6 放大淡入；开合时转过去（0° 向下，-90° 向右）。
      const gfx::PointF center =
          gfx::RectF(icon_holder_->bounds()).CenterPoint();
      const float scale =
          kTreeHoverIconScale + (1 - kTreeHoverIconScale) * hover;
      canvas->Save();
      canvas->sk_canvas()->translate(center.x(), center.y());
      canvas->sk_canvas()->rotate(-90 * static_cast<float>(tree_fold_.value()));
      canvas->sk_canvas()->scale(scale, scale);
      canvas->sk_canvas()->translate(-kTreeChevronSize / 2.0f,
                                     -kTreeChevronSize / 2.0f);
      gfx::PaintVectorIcon(
          canvas, icons::kChevronDown, kTreeChevronSize,
          SkColorSetA(theme_.on_surface, static_cast<U8CPU>(255 * hover)));
      canvas->Restore();
    }
  }
  if (!tree_count_bounds_.IsEmpty()) {
    // 多了标签：1 → 1.28 → 1。
    const bool bump = tree_bump_.is_animating();
    if (bump) {
      const float t = static_cast<float>(tree_bump_.value());
      const float scale = 1 + (kTreeCountBumpScale - 1) *
                                  std::sin(t * std::numbers::pi_v<float>);
      const gfx::PointF center = gfx::RectF(tree_count_bounds_).CenterPoint();
      canvas->Save();
      canvas->sk_canvas()->translate(center.x(), center.y());
      canvas->sk_canvas()->scale(scale, scale);
      canvas->sk_canvas()->translate(-center.x(), -center.y());
    }
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(theme_.OnSurface(0.1f));
    canvas->DrawRoundRect(gfx::RectF(tree_count_bounds_),
                          kTreeCountHeight / 2.0f, fill);
    canvas->DrawStringRectWithFlags(
        base::NumberToString16(tree_count_),
        UiFont(kTreeCountFontSize, gfx::Font::Weight::SEMIBOLD),
        theme_.on_surface_variant, tree_count_bounds_,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    if (bump) {
      canvas->Restore();
    }
  }
  const ui::ColorProvider* provider = GetColorProvider();
  if (tree_stack_bounds_.IsEmpty() || !provider) {
    return;
  }
  // 单独一层：后一个图标外面的一圈空是在这一层上挖掉的（同折叠的文件夹）。
  gfx::Rect layer = tree_stack_bounds_;
  layer.Outset(2);
  canvas->SaveLayerAlpha(255, layer);
  const size_t shown = std::min(tree_icons_.size(), kTreeStackMax);
  for (size_t i = 0; i < shown; ++i) {
    const gfx::Rect rect(
        tree_stack_bounds_.x() + static_cast<int>(i) * kTreeStackStep,
        tree_stack_bounds_.y(), kTreeStackIcon, kTreeStackIcon);
    if (i > 0) {
      cc::PaintFlags clear;
      clear.setAntiAlias(true);
      clear.setBlendMode(SkBlendMode::kClear);
      gfx::RectF gap(rect);
      gap.Outset(kTreeStackGap);
      canvas->DrawRoundRect(gap, 4 + kTreeStackGap, clear);
    }
    const gfx::ImageSkia image = tree_icons_[i].IsEmpty()
                                     ? gfx::ImageSkia()
                                     : tree_icons_[i].Rasterize(provider);
    if (image.isNull()) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(0.22f));
      canvas->DrawRoundRect(gfx::RectF(rect), 4, flags);
      continue;
    }
    canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), rect.x(),
                         rect.y(), rect.width(), rect.height(), true);
  }
  canvas->Restore();
}

void TabCell::SetTrayInsets(int left, int right) {
  if (tray_left_ == left && tray_right_ == right) {
    return;
  }
  tray_left_ = left;
  tray_right_ = right;
  InvalidateLayout();
  SchedulePaint();
}

gfx::Rect TabCell::ContentRect() const {
  gfx::Rect content = GetLocalBounds();
  content.Inset(gfx::Insets::TLBR(0, tray_left_, 0, tray_right_));
  return content;
}

gfx::Point TabCell::IconCenter() const {
  return icon_holder_->bounds().CenterPoint();
}

void TabCell::SetLifted(bool lifted) {
  if (lifted_ == lifted) {
    return;
  }
  lifted_ = lifted;
  // 拖起来以后鼠标的进出收不到了：悬停的样子先收掉，不然松手后还留在原来那一行上。
  if (lifted) {
    SetHovered(false);
  }
  motion_.AnimateOpacity(layer(), lifted ? motion::kTabLiftedOpacity : 1,
                         motion::Hover());
}

void TabCell::SetDropTarget(bool target) {
  if (drop_target_ == target) {
    return;
  }
  drop_target_ = target;
  if (target) {
    drop_animation_.Show();
  } else {
    drop_animation_.Hide();
  }
}

void TabCell::SetLeaving() {
  leaving_ = true;
  SetCanProcessEventsWithinSubtree(false);
  // 有焦点时交给下一个。
  SetFocusBehavior(FocusBehavior::NEVER);
  SetHovered(false);
}

void TabCell::Bounce() {
  if (motion::ReduceMotion()) {
    return;
  }
  if (!icon_holder_->layer()) {
    icon_holder_->SetPaintToLayer();
    icon_holder_->layer()->SetFillsBoundsOpaquely(false);
  }
  const gfx::PointF center(icon_holder_->width() / 2.0f,
                           icon_holder_->height() / 2.0f);
  bounce_motion_.AnimateTransformFrom(
      icon_holder_->layer(), LayerPose(), LayerPose(), motion::TabClickBounce(),
      center,
      base::BindOnce(
          [](base::WeakPtr<TabCell> cell) {
            // 父标签的图标要留着图层：悬停时靠它淡出。
            if (cell && cell->icon_holder_->layer() && !cell->tree_parent_) {
              cell->icon_holder_->DestroyLayer();
            }
          },
          weak_factory_.GetWeakPtr()));
}

bool TabCell::StepLoadingAnimation(base::TimeDelta elapsed) {
  icon_->StepLoadingAnimation(elapsed);
  return icon_->GetShowingLoadingAnimation();
}

gfx::Size TabCell::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  switch (style_) {
    case TabCellStyle::kRow:
      return gfx::Size(0, metrics::kTabRowHeight);
    case TabCellStyle::kTile:
      return gfx::Size(0, metrics::kPinnedTileHeight);
    case TabCellStyle::kTopRow:
      return gfx::Size(metrics::kTopTabMaxWidth, metrics::kTopTabHeight);
    case TabCellStyle::kTopTile:
      return gfx::Size(metrics::kTopPinnedTabWidth, metrics::kTopTabHeight);
  }
}

void TabCell::Layout(PassKey) {
  if (is_tile()) {
    LayoutTile();
  } else {
    LayoutRow();
  }
}

void TabCell::LayoutRow() {
  const int size = metrics::kFaviconSize;
  const int mid_y = height() / 2;
  const bool top = is_top();
  const int spacing = top ? kTopRowSpacing : kRowSpacing;
  gfx::Rect favicon(tray_left_ + kRowLeading, mid_y - size / 2, size, size);
  icon_holder_->SetBoundsRect(favicon);
  if (agent_ring_) {
    agent_ring_->SetBoundsRect(AgentRing::BoundsAround(favicon));
  }
  gfx::Rect icon_bounds(icon_holder_->size());
  icon_bounds.Inset(-icon_->GetInsets());
  icon_->SetBoundsRect(icon_bounds);

  const int trailing = top ? kTopRowTrailing : kRowTrailing;
  int right = width() - tray_right_ - trailing;
  auto place = [&](const gfx::Size& item) {
    gfx::Rect rect(right - item.width(), mid_y - item.height() / 2,
                   item.width(), item.height());
    right = rect.x() - spacing;
    return rect;
  };
  if (close_button_) {
    if (ClosePresent()) {
      close_button_->SetBoundsRect(
          place(gfx::Size(kCloseButtonSize, kCloseButtonSize)));
    } else {
      close_button_->SetBounds(
          width() - tray_right_ - trailing - kCloseButtonSize,
          mid_y - kCloseButtonSize / 2, kCloseButtonSize, kCloseButtonSize);
    }
  }
  if (audio_button_->GetVisible()) {
    audio_button_->SetBoundsRect(
        place(gfx::Size(kAudioButtonSize, kAudioButtonSize)));
  }
  LayoutMeter();
  capture_bounds_ = ShowsCapture()
                        ? place(gfx::Size(kCaptureDotSize, kCaptureDotSize))
                        : gfx::Rect();
  sleep_bounds_ = data_.is_tab_discarded
                      ? place(gfx::Size(kSleepGlyphSize, kSleepGlyphSize))
                      : gfx::Rect();
  // 「放进文件夹」的提示：文件夹图标和名字的小胶囊（名字太长时截短）。
  suggest_bounds_ = gfx::Rect();
  if (!top && !suggest_name_.empty()) {
    const int text =
        std::min(kSuggestMaxText,
                 gfx::GetStringWidth(suggest_name_,
                                     UiFont(11, gfx::Font::Weight::MEDIUM)));
    suggest_bounds_ =
        place(gfx::Size(kSuggestPadding + kSuggestIcon + kSuggestGap + text +
                            kSuggestPadding + 1,
                        kSuggestHeight));
  }
  // 树里折着的父标签：里面几个标签，行够宽时前面叠着前几个子标签的图标。
  tree_count_bounds_ = gfx::Rect();
  tree_stack_bounds_ = gfx::Rect();
  if (tree_collapsed_ && !top) {
    const int count_width =
        std::max(kTreeCountMinWidth,
                 gfx::GetStringWidth(
                     base::NumberToString16(tree_count_),
                     UiFont(kTreeCountFontSize, gfx::Font::Weight::SEMIBOLD)) +
                     2 * kTreeCountPadding);
    tree_count_bounds_ = place(gfx::Size(count_width, kTreeCountHeight));
    const int shown =
        static_cast<int>(std::min(tree_icons_.size(), kTreeStackMax));
    if (shown > 0 && width() >= kTreeStackMinWidth) {
      const int stack_width = kTreeStackIcon + (shown - 1) * kTreeStackStep;
      tree_stack_bounds_ =
          gfx::Rect(tree_count_bounds_.x() - kTreeStackCountGap - stack_width,
                    mid_y - kTreeStackIcon / 2, stack_width, kTreeStackIcon);
      right = tree_stack_bounds_.x() - spacing;
    }
  }
  // 侧栏的标题后面是弹性空白，它和左右两边各有一个间距；顶部的标题占满。
  const int start = favicon.right() + spacing;
  const int end = top ? right + spacing : right;
  title_bounds_ = gfx::Rect(start, 0, std::max(0, end - start), height());
}

void TabCell::LayoutTile() {
  const bool compact = style_ == TabCellStyle::kTopTile;
  const int size = compact ? kTopTileIconSize : kTileIconSize;
  gfx::Rect favicon((width() - size) / 2, (height() - size) / 2, size, size);
  icon_holder_->SetBoundsRect(favicon);
  if (agent_ring_) {
    agent_ring_->SetBoundsRect(AgentRing::BoundsAround(favicon));
  }
  gfx::Rect icon_bounds(icon_holder_->size());
  icon_bounds.Inset(-icon_->GetInsets());
  icon_->SetBoundsRect(icon_bounds);
  // 右上角只放一个：休眠、摄像头麦克风、声音，按这个顺序（顶部标签行里的小一些）。
  const int sleep = compact ? kSleepGlyphSize - 2 : kSleepGlyphSize;
  sleep_bounds_ = data_.is_tab_discarded
                      ? gfx::Rect(width() - 4 - sleep, 4, sleep, sleep)
                      : gfx::Rect();
  const int dot = compact ? 3 : 5;
  capture_bounds_ = !data_.is_tab_discarded && ShowsCapture()
                        ? gfx::Rect(width() - dot - kCaptureDotSize - 1, dot,
                                    kCaptureDotSize, kCaptureDotSize)
                        : gfx::Rect();
  const int pad = compact ? 2 : 4;
  audio_button_->SetBounds(width() - kAudioButtonSize - pad, pad / 2,
                           kAudioButtonSize, kAudioButtonSize);
  LayoutMeter();
  title_bounds_ = gfx::Rect();
}

void TabCell::OnPaint(gfx::Canvas* canvas) {
  // 底色：选中时由列表的高亮代替；侧栏的行平时透明，顶部的标签和固定标签平时有一层很淡
  // 的底。
  const float hover = static_cast<float>(hover_animation_.GetCurrentValue());
  float opacity = 0;
  if (!selected_) {
    switch (style_) {
      case TabCellStyle::kRow:
        opacity = 0.05f * hover;
        break;
      case TabCellStyle::kTopRow:
        opacity = 0.035f + 0.035f * hover;
        break;
      case TabCellStyle::kTile:
      case TabCellStyle::kTopTile:
        opacity = 0.06f + 0.04f * hover;
        break;
    }
  }
  const gfx::Rect content = ContentRect();
  if (opacity > 0) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme_.OnSurface(opacity));
    canvas->DrawRoundRect(gfx::RectF(content), radius::kRow, flags);
  }
  // 拖着的标签会和它合成文件夹：垫一层强调色、描一圈边（同文件夹的行）。
  if (const float drop = static_cast<float>(drop_animation_.GetCurrentValue());
      drop > 0) {
    gfx::RectF box(content);
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(
        SkColorSetA(theme_.accent, static_cast<U8CPU>(255 * 0.12f * drop)));
    canvas->DrawRoundRect(box, radius::kRow, fill);
    cc::PaintFlags outline;
    outline.setAntiAlias(true);
    outline.setStyle(cc::PaintFlags::kStroke_Style);
    outline.setStrokeWidth(1.5f);
    outline.setColor(
        SkColorSetA(theme_.accent, static_cast<U8CPU>(255 * 0.7f * drop)));
    box.Inset(0.75f);
    canvas->DrawRoundRect(box, radius::kRow - 0.75f, outline);
  }
  if (in_split_ && is_tile()) {
    const int inset = style_ == TabCellStyle::kTopTile ? 3 : 4;
    canvas->Save();
    canvas->Translate(gfx::Vector2d(width() - inset - kSplitBadgeSize,
                                    height() - inset - kSplitBadgeSize));
    gfx::PaintVectorIcon(
        canvas, icons::kSplit2x1, kSplitBadgeSize,
        split_active_ ? theme_.accent : theme_.on_surface_variant);
    canvas->Restore();
  }

  if (!title_bounds_.IsEmpty()) {
    // 顶部标签字号略小且未选中时前景色略淡。
    const bool top = is_top();
    const gfx::FontList& font =
        UiFont(top ? 12 : 13, selected_ ? gfx::Font::Weight::SEMIBOLD
                                        : gfx::Font::Weight::NORMAL);
    const std::u16string title = DisplayTitle(data_);
    canvas->DrawStringRectWithFlags(
        title, font, theme_.OnSurface(selected_ ? 1.0f : (top ? 0.78f : 0.82f)),
        title_bounds_, gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  PaintTree(canvas);

  if (!sleep_bounds_.IsEmpty()) {
    canvas->Save();
    canvas->Translate(sleep_bounds_.OffsetFromOrigin());
    gfx::PaintVectorIcon(canvas, icons::kMoonFill, kSleepGlyphSize,
                         theme_.on_surface_variant);
    canvas->Restore();
  }
  if (!suggest_bounds_.IsEmpty()) {
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(suggest_hovered_
                      ? SkColorSetA(theme_.accent, theme_.is_dark ? 56 : 41)
                      : theme_.OnSurface(0.06f + 0.03f * hover));
    canvas->DrawRoundRect(gfx::RectF(suggest_bounds_),
                          suggest_bounds_.height() / 2.0f, fill);
    const SkColor text_color =
        suggest_hovered_ ? theme_.accent : theme_.on_surface_variant;
    canvas->Save();
    canvas->Translate(
        gfx::Vector2d(suggest_bounds_.x() + kSuggestPadding,
                      suggest_bounds_.CenterPoint().y() - kSuggestIcon / 2));
    gfx::PaintVectorIcon(canvas, icons::kFolder, kSuggestIcon,
                         suggest_tint_.value_or(text_color));
    canvas->Restore();
    const gfx::FontList& font = UiFont(11, gfx::Font::Weight::MEDIUM);
    const int text_x =
        suggest_bounds_.x() + kSuggestPadding + kSuggestIcon + kSuggestGap;
    const int text_width =
        std::max(0, suggest_bounds_.right() - kSuggestPadding - text_x);
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(suggest_name_, font, text_width, gfx::ELIDE_TAIL), font,
        text_color,
        gfx::Rect(text_x, suggest_bounds_.y(), text_width + 1,
                  suggest_bounds_.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
  if (!capture_bounds_.IsEmpty()) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme_.danger());
    canvas->DrawCircle(gfx::PointF(capture_bounds_.CenterPoint()),
                       kCaptureDotSize / 2.0f, flags);
  }
  if (HasFocus()) {
    PaintFocusRing(canvas, theme_, gfx::RectF(content), radius::kRow);
  }
}

bool TabCell::OnKeyPressed(const ui::KeyEvent& event) {
  if (leaving_) {
    return false;
  }
  if (event.key_code() == ui::VKEY_RETURN ||
      event.key_code() == ui::VKEY_SPACE) {
    delegate_->TabCellClicked(this, /*reset=*/false, /*keep_current=*/false);
    return true;
  }
  return delegate_->TabListKeyPressed(this, event);
}

void TabCell::OnFocus() {
  views::View::OnFocus();
  ScrollViewToVisible();
  SchedulePaint();
}

void TabCell::OnBlur() {
  views::View::OnBlur();
  SchedulePaint();
}

bool TabCell::IsGroupFocusTraversable() const {
  return false;
}

views::View* TabCell::GetSelectedViewForGroup(int group) {
  return TabListView::FocusEntryFor(this);
}

bool TabCell::OnMousePressed(const ui::MouseEvent& event) {
  // 「放进文件夹」的提示：松开时放进去，不切过去、不拖。
  if (event.IsOnlyLeftMouseButton() &&
      suggest_bounds_.Contains(event.location())) {
    suggest_pressed_ = true;
    return true;
  }
  if (event.IsOnlyLeftMouseButton()) {
    press_location_ = event.location();
    dragging_ = false;
    return true;
  }
  // 中键关标签（同 Chrome）。
  return event.IsOnlyMiddleMouseButton();
}

bool TabCell::OnMouseDragged(const ui::MouseEvent& event) {
  if (suggest_pressed_) {
    return true;
  }
  if (!press_location_) {
    return false;
  }
  if (!dragging_) {
    const gfx::Vector2d moved = event.location() - *press_location_;
    if (std::abs(moved.x()) < kDragThreshold &&
        std::abs(moved.y()) < kDragThreshold) {
      return true;
    }
    dragging_ = true;
    drag_alone_ = event.IsAltDown();
    delegate_->TabCellDragStarted(this, *press_location_);
  }
  delegate_->TabCellDragged(this, event.location());
  return true;
}

void TabCell::OnMouseReleased(const ui::MouseEvent& event) {
  if (event.IsOnlyMiddleMouseButton()) {
    if (HitTestPoint(event.location())) {
      delegate_->TabCellCloseRequested(this);
    }
    return;
  }
  if (std::exchange(suggest_pressed_, false)) {
    if (suggest_bounds_.Contains(event.location())) {
      delegate_->TabCellSuggestionAccepted(this);
    }
    return;
  }
  if (!press_location_) {
    return;
  }
  press_location_.reset();
  if (dragging_) {
    dragging_ = false;
    delegate_->TabCellDragEnded(this, /*cancelled=*/false);
    return;
  }
  // 树里的父标签：点图标处的箭头开合（⌥：连同里面的各层）。
  if (tree_parent_ && TreeToggleBounds().Contains(event.location())) {
    delegate_->TabCellTreeToggled(this, event.IsAltDown());
    return;
  }
  // 松开鼠标时才切换选中，保证拖动标签过程中当前网页不切换。
  if (HitTestPoint(event.location())) {
    const bool modifier = (event.flags() & ui::EF_PLATFORM_ACCELERATOR) != 0;
    const bool icon_click = icon_holder_->bounds().Contains(event.location());
    if (!is_tile() && !icon_click && event.GetClickCount() == 2 &&
        handle_.Get() && handle_.Get()->IsPinned()) {
      pinned_tabs::Rename(handle_.Get());
      return;
    }
    const bool reset =
        is_tile() ? (event.GetClickCount() == 2 || modifier) : icon_click;
    delegate_->TabCellClicked(this, reset, modifier);
  }
}

void TabCell::OnMouseCaptureLost() {
  suggest_pressed_ = false;
  press_location_.reset();
  if (dragging_) {
    dragging_ = false;
    delegate_->TabCellDragEnded(this, /*cancelled=*/true);
  }
}

void TabCell::OnMouseEntered(const ui::MouseEvent& event) {
  SetHovered(true);
}

void TabCell::OnMouseMoved(const ui::MouseEvent& event) {
  const bool over = suggest_bounds_.Contains(event.location());
  if (over != suggest_hovered_) {
    suggest_hovered_ = over;
    SchedulePaint();
  }
}

void TabCell::OnMouseExited(const ui::MouseEvent& event) {
  suggest_hovered_ = false;
  SetHovered(false);
}

std::u16string TabCell::GetRenderedTooltipText(const gfx::Point& p) const {
  if (suggest_bounds_.Contains(p)) {
    return L("organize.suggest.help", {{"folder", suggest_name_}});
  }
  if (tree_parent_ && TreeToggleBounds().Contains(p)) {
    return tree_collapsed_ ? L("tabs.tree.expand") : L("tabs.tree.collapse");
  }
  if (const auto* tab = handle_.Get(); tab && tab->IsPinned() && !is_tile() &&
                                       icon_holder_->bounds().Contains(p)) {
    const auto url = pinned_tabs::PinnedUrl(handle_.Get());
    if (url.is_valid() && url != data_.visible_url) {
      return L("tabs.resetPin");
    }
  }
  // 开启标签预览时不再显示原生文字提示。
  if (!delegate_->TabCellShowsTooltip()) {
    return std::u16string();
  }
  const std::u16string title = DisplayTitle(data_);
  std::u16string help;
  if (is_tile() || data_.visible_url.is_empty() ||
      title == L("tabs.untitled")) {
    help = title;
  } else if (is_top() && !IsInternalURL(data_.visible_url)) {
    // 顶部的标签只显示网址（标题就在标签上）。
    help = base::UTF8ToUTF16(data_.visible_url.spec());
  } else {
    help = title + u"\n" + base::UTF8ToUTF16(data_.visible_url.spec());
  }
  // 智能体正在操作时，首行展示其当前状态。
  return agent_status_.empty() ? help : agent_status_ + u"\n" + help;
}

void TabCell::ShowContextMenuForViewImpl(views::View* source,
                                         const gfx::Point& point,
                                         ui::mojom::MenuSourceType source_type) {
  if (!leaving_) {
    delegate_->TabCellContextMenu(this, point, source_type);
  }
}

void TabCell::AnimationProgressed(const gfx::Animation* animation) {
  if (animation == &hover_animation_ && close_button_ && !selected_) {
    close_button_->SetAlpha(static_cast<float>(animation->GetCurrentValue()));
  }
  if (animation == &hover_animation_) {
    UpdateIconFade();
  }
  SchedulePaint();
}

void TabCell::AnimationEnded(const gfx::Animation* animation) {
  AnimationProgressed(animation);
  UpdateButtons();
}

void TabCell::SetHovered(bool hovered) {
  if (leaving_) {
    hovered = false;
  }
  if (hovered_ == hovered) {
    return;
  }
  hovered_ = hovered;
  if (hovered) {
    hover_animation_.Show();
  } else {
    hover_animation_.Hide();
  }
  UpdateButtons();
  InvalidateLayout();
  delegate_->TabCellHoverChanged(this, hovered);
}

bool TabCell::ClosePresent() const {
  return close_button_ && !is_tile() && !leaving_ && (hovered_ || selected_);
}

void TabCell::UpdateButtons() {
  audio_button_->SetVisible(ShowsAudio());
  // 在放声音时喇叭的位置是跟着声音跳的电平条；鼠标移上来换回喇叭（点它静音）。
  const bool meter = meter_ && meter_->GetVisible();
  audio_button_->SetAlpha(meter && !hovered_ ? 0 : 1);
  if (meter_) {
    meter_->layer()->SetOpacity(meter && !hovered_ ? 1 : 0);
  }
  if (!close_button_) {
    return;
  }
  if (is_tile()) {
    close_button_->SetVisible(false);
    return;
  }
  close_button_->SetShowsFill(hovered_);
  const bool present = ClosePresent();
  // 悬停淡出的过程中还留着，放完再藏起来。
  const bool fading = hover_animation_.is_animating() && !selected_;
  close_button_->SetVisible(present || fading);
  if (selected_) {
    close_button_->SetAlpha(1);
  } else if (!fading) {
    close_button_->SetAlpha(present ? 1 : 0);
  }
}

bool TabCell::ShowsCapture() const {
  if (!data_.alert_state) {
    return false;
  }
  switch (*data_.alert_state) {
    case tabs::TabAlert::kMediaRecording:
    case tabs::TabAlert::kTabCapturing:
    case tabs::TabAlert::kDesktopCapturing:
    case tabs::TabAlert::kAudioRecording:
    case tabs::TabAlert::kVideoRecording:
      return true;
    default:
      return false;
  }
}

bool TabCell::ShowsAudio() const {
  if (is_tile() && (data_.is_tab_discarded || ShowsCapture())) {
    return false;
  }
  return data_.alert_state == tabs::TabAlert::kAudioPlaying ||
         data_.alert_state == tabs::TabAlert::kAudioMuting;
}

void TabCell::UpdateMeter() {
  tabs::TabInterface* tab = handle_.Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  const bool live = contents && !leaving_ && ShowsAudio() && !Muted() &&
                    !data_.is_tab_discarded &&
                    prefs::PrefsFor(Profile::FromBrowserContext(
                                        contents->GetBrowserContext()))
                        ->GetBoolean(prefs::kMediaTabMeters);
  if (live && !meter_) {
    meter_ = AddChildView(std::make_unique<MediaWaveform>(
        MediaHub::Get(
            Profile::FromBrowserContext(contents->GetBrowserContext())),
        MediaWaveform::Style::kMeter));
    meter_->SetBar(2, 1.5f);
  }
  if (!meter_) {
    return;
  }
  meter_->SetVisible(live);
  if (live) {
    meter_->SetSource(
        content::MediaSession::GetRequestIdFromWebContents(contents),
        /*playing=*/true,
        selected_ ? theme_.primary : theme_.on_surface_variant);
  }
}

void TabCell::LayoutMeter() {
  if (meter_) {
    // 喇叭那一格里居中，比喇叭的图形略宽。
    gfx::Rect rect = audio_button_->bounds();
    rect.ClampToCenteredSize(gfx::Size(kAudioGlyphSize, kAudioGlyphSize - 2));
    meter_->SetBoundsRect(rect);
  }
}

bool TabCell::Muted() const {
  return data_.alert_state == tabs::TabAlert::kAudioMuting;
}

void TabCell::UpdateAgent() {
  tabs::TabInterface* tab = handle_.Get();
  const agent::AgentActivity* activity =
      tab && !leaving_ ? agent::AgentActivities::Get()->For(tab) : nullptr;
  std::u16string status;
  if (activity) {
    status = L("agent.status.accessibility",
               {{"client", activity->client},
                {"action", activity->Line(tab->GetContents())}});
  }
  const std::optional<bool> working =
      activity ? std::optional<bool>(activity->working()) : std::nullopt;
  if (working && !agent_ring_) {
    // 在图标下面（回弹时图标有自己的图层）。
    agent_ring_ = AddChildViewAt(std::make_unique<AgentRing>(), 0);
    InvalidateLayout();
  }
  if (agent_ring_) {
    agent_ring_->SetWorking(working);
  }
  if (status != agent_status_) {
    agent_status_ = std::move(status);
    UpdateAccessibleName();
  }
}

void TabCell::UpdateAccessibleName() {
  std::u16string title = DisplayTitle(data_);
  if (tree_collapsed_) {
    title += u", " + L("tabs.tree.collapsedCount", {{"count", tree_count_}});
  }
  GetViewAccessibility().SetName(
      agent_status_.empty() ? title : title + u", " + agent_status_);
}

std::string TabCell::AgentRingDescription() const {
  if (!agent_ring_ || !agent_ring_->working()) {
    return "none";
  }
  return *agent_ring_->working()
             ? (agent_ring_->spinning() ? "working(spin)" : "working")
             : "idle";
}

BEGIN_METADATA(TabCell)
END_METADATA

}  // namespace starry
