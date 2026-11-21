// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/split/split_overlay.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/contents_container_outline.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view_mini_toolbar.h"
#include "components/tabs/public/split_tab_data.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_background_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/text_utils.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_utils.h"

namespace starry {

namespace split_ui {

namespace {

// 把手：收起时一段 36×5 的胶囊，离格子顶边 5；感应的范围是顶上 96×16。展开时是高 26 的
// 玻璃小条，离顶边 6：左右 10，图标 14，间距 6，标题最长 180，三个点。
constexpr int kCapsuleWidth = 36;
constexpr int kCapsuleHeight = 5;
constexpr int kCapsuleTop = 5;
constexpr int kHandleHitWidth = 96;
constexpr int kHandleHitHeight = 16;
constexpr int kPillHeight = 26;
constexpr int kPillTop = 6;
constexpr int kPillPadding = 10;
constexpr int kPillIcon = 14;
constexpr int kPillSpacing = 6;
constexpr int kPillTitleMax = 180;
constexpr int kPillEllipsis = 10;
// 当前一格的描边。
constexpr int kRingWidth = 2;
// 拒绝的小条：高 38，左右 16。
constexpr int kRefusalHeight = 38;
// 预览的格子给阴影留的边。
constexpr int kSlotShadow = 20;
constexpr int kDragThreshold = 4;

SkColor Alpha(SkColor color, float opacity) {
  return SkColorSetA(color,
                     static_cast<U8CPU>(std::clamp(opacity, 0.0f, 1.0f) * 255));
}

std::u16string TitleOf(const tabs::TabData& data) {
  if (data.title.empty() || data.visible_url.IsAboutBlank()) {
    return L("tabs.untitled");
  }
  return data.title;
}

void DrawFavicon(gfx::Canvas* canvas,
                 const views::View* view,
                 const ui::ImageModel& favicon,
                 const gfx::Rect& rect) {
  if (favicon.IsEmpty() || !view->GetColorProvider()) {
    return;
  }
  const gfx::ImageSkia image = favicon.Rasterize(view->GetColorProvider());
  canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), rect.x(),
                       rect.y(), rect.width(), rect.height(), true);
}

// 玻璃面板着色底与 1px 细描边。
void PaintPanelTint(gfx::Canvas* canvas,
                    const gfx::RectF& bounds,
                    float radius,
                    const Theme& theme) {
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(Alpha(theme.surface_panel, theme.is_dark ? 0.66f : 0.76f));
  canvas->DrawRoundRect(bounds, radius, fill);
  cc::PaintFlags border;
  border.setAntiAlias(true);
  border.setStyle(cc::PaintFlags::kStroke_Style);
  border.setStrokeWidth(1);
  border.setColor(theme.OnSurface(0.1f));
  gfx::RectF stroke = bounds;
  stroke.Inset(0.5f);
  canvas->DrawRoundRect(stroke, std::max(0.0f, radius - 0.5f), border);
}

GlassStyle GlassFor(const Theme& theme) {
  GlassStyle style;
  style.dark = theme.is_dark;
  style.luma = theme.is_dark ? 0.1f : 0.9f;
  return style;
}

// 只画东西、不接鼠标的一块，内容由回调画。
class Painted : public views::View {
  METADATA_HEADER(Painted, views::View)

 public:
  using Painter =
      base::RepeatingCallback<void(gfx::Canvas*, const views::View*)>;
  explicit Painted(Painter painter) : painter_(std::move(painter)) {
    SetCanProcessEventsWithinSubtree(false);
  }
  void OnPaint(gfx::Canvas* canvas) override { painter_.Run(canvas, this); }

 private:
  Painter painter_;
};

BEGIN_METADATA(Painted)
END_METADATA

}  // namespace

// 当前一格的描边：2px 强调色细线，切换格子时平滑滑动。
class FocusRing : public views::View {
  METADATA_HEADER(FocusRing, views::View)

 public:
  FocusRing()
      : slide_(1,
               base::BindRepeating(&FocusRing::Step, base::Unretained(this))) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  // 放到 target；animate 时从现在的位置滑过去。nullopt 时淡出。
  void MoveTo(std::optional<gfx::Rect> target, bool animate) {
    if (!target) {
      if (shown_) {
        fade_.AnimateOpacity(layer(), 0, motion::Hover());
      }
      shown_ = false;
      return;
    }
    if (!shown_) {
      from_ = to_ = *target;
      slide_.SetValue(1);
      SetBoundsRect(*target);
      fade_.AnimateOpacity(layer(), 1, motion::Hover());
      shown_ = true;
      return;
    }
    if (*target == to_) {
      return;
    }
    if (!animate || motion::ReduceMotion()) {
      from_ = to_ = *target;
      if (slide_.is_animating()) {
        // 正在滑：终点换了（窗口在变大小），接着滑到新的终点。
        to_ = *target;
        return;
      }
      SetBoundsRect(*target);
      return;
    }
    from_ = bounds();
    to_ = *target;
    slide_.SetValue(0);
    slide_.AnimateTo(1, MotionCurve::Of(motion::kSelection));
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(kRingWidth);
    flags.setColor(Alpha(theme_.accent, theme_.is_dark ? 0.6f : 0.45f));
    gfx::RectF rect(GetLocalBounds());
    rect.Inset(kRingWidth / 2.0f);
    canvas->DrawRoundRect(rect, radius::kContent - kRingWidth / 2.0f, flags);
  }

 private:
  void Step() {
    SetBoundsRect(gfx::Tween::RectValueBetween(slide_.value(), from_, to_));
  }

  Theme theme_ = Theme::DarkBase();
  ValueMotion slide_;
  LayerMotion fade_;
  gfx::Rect from_;
  gfx::Rect to_;
  bool shown_ = false;
};

BEGIN_METADATA(FocusRing)
END_METADATA

// 两格之间的分隔线（就是空隙）。
class Divider : public views::View, public views::AnimationDelegateViews {
  METADATA_HEADER(Divider, views::View)

 public:
  Divider(SplitOverlay* overlay, int index)
      : views::AnimationDelegateViews(this), overlay_(overlay), index_(index) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    hover_.SetSlideDuration(base::Milliseconds(180));
    arm_.SetSlideDuration(base::Milliseconds(200));
    SetWindowDraggable(this, false);
    GetViewAccessibility().SetRole(ax::mojom::Role::kSplitter);
    GetViewAccessibility().SetName(L("split.divider.help"));
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }
  void SetSideBySide(bool side_by_side) {
    if (side_by_side_ != side_by_side) {
      side_by_side_ = side_by_side;
      SchedulePaint();
    }
  }
  void SetArmed(bool armed) {
    if (armed == armed_) {
      return;
    }
    armed_ = armed;
    if (motion::ReduceMotion()) {
      arm_.Reset(armed ? 1 : 0);
    } else if (armed) {
      arm_.Show();
    } else {
      arm_.Hide();
    }
    SchedulePaint();
  }

  // views::View:
  void OnMouseEntered(const ui::MouseEvent& event) override { hover_.Show(); }
  void OnMouseExited(const ui::MouseEvent& event) override {
    if (!dragging_) {
      hover_.Hide();
    }
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton()) {
      return false;
    }
    if (event.GetClickCount() == 2) {
      overlay_->Equalize();
      return true;
    }
    press_ = ToOverlay(event.location());
    return true;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!press_) {
      return false;
    }
    // 用父视图的坐标：分隔线自己在跟着动。
    const gfx::Point point = ToOverlay(event.location());
    const int translation =
        side_by_side_ ? point.x() - press_->x() : point.y() - press_->y();
    if (!dragging_) {
      if (std::abs(translation) < 1) {
        return true;
      }
      dragging_ = true;
      overlay_->BeginResize(index_);
      SchedulePaint();
    }
    overlay_->Resize(translation);
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    press_.reset();
    if (dragging_) {
      dragging_ = false;
      overlay_->EndResize(/*commit=*/true);
      if (!IsMouseHovered()) {
        hover_.Hide();
      }
      SchedulePaint();
    }
  }
  void OnMouseCaptureLost() override {
    press_.reset();
    if (dragging_) {
      dragging_ = false;
      overlay_->EndResize(/*commit=*/false);
      hover_.Hide();
      SchedulePaint();
    }
  }
  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    return ui::Cursor(side_by_side_ ? ui::mojom::CursorType::kColumnResize
                                    : ui::mojom::CursorType::kRowResize);
  }
  std::u16string GetRenderedTooltipText(const gfx::Point& p) const override {
    return dragging_ ? std::u16string() : L("split.divider.help");
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const float shown = std::max(static_cast<float>(hover_.GetCurrentValue()),
                                 dragging_ ? 1.0f : 0.0f);
    if (shown <= 0) {
      return;
    }
    const float along = side_by_side_ ? height() : width();
    const float length =
        std::min(44.0f, along * 0.3f) * (dragging_ ? 1.2f : 1.0f);
    const float thickness = 4;
    const gfx::PointF center = gfx::RectF(GetLocalBounds()).CenterPoint();
    const gfx::RectF capsule =
        side_by_side_
            ? gfx::RectF(center.x() - thickness / 2, center.y() - length / 2,
                         thickness, length)
            : gfx::RectF(center.x() - length / 2, center.y() - thickness / 2,
                         length, thickness);
    const SkColor rest = theme_.OnSurface(dragging_ ? 0.5f : 0.3f);
    const SkColor color = color_utils::AlphaBlend(
        theme_.accent, rest, static_cast<float>(arm_.GetCurrentValue()));
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(Alpha(color, SkColorGetA(color) / 255.0f * shown));
    canvas->DrawRoundRect(capsule, thickness / 2, flags);
  }

  // views::AnimationDelegateViews:
  void AnimationProgressed(const gfx::Animation* animation) override {
    SchedulePaint();
  }

 private:
  gfx::Point ToOverlay(const gfx::Point& point) const {
    gfx::Point converted = point;
    views::View::ConvertPointToTarget(this, parent(), &converted);
    return converted;
  }

  const raw_ptr<SplitOverlay> overlay_;
  const int index_;
  Theme theme_ = Theme::DarkBase();
  bool side_by_side_ = true;
  bool dragging_ = false;
  bool armed_ = false;
  std::optional<gfx::Point> press_;
  gfx::SlideAnimation hover_{this};
  gfx::SlideAnimation arm_{this};
};

BEGIN_METADATA(Divider)
END_METADATA

// 把手菜单（点击和右键共用）。
class PaneMenu : public ui::SimpleMenuModel::Delegate {
 public:
  enum Command { kRemove = 1, kShowOnly, kEqualize, kClose };

  PaneMenu(BrowserWindowInterface* browser, tabs::TabInterface* tab)
      : browser_(browser), tab_(tab->GetHandle()), model_(this) {
    const auto icon = [](const gfx::VectorIcon& vector) {
      return ui::ImageModel::FromVectorIcon(vector, ui::kColorMenuIcon, 14);
    };
    model_.AddItemWithIcon(kRemove, L("split.remove"),
                           icon(icons::kSplitRemove));
    model_.AddItemWithIcon(kShowOnly, L("split.showOnlyThisTab"),
                           icon(icons::kRectangle));
    model_.AddItemWithIcon(kEqualize, L("split.equalize"),
                           icon(icons::kEqualSquare));
    model_.AddSeparator(ui::NORMAL_SEPARATOR);
    model_.AddItemWithIcon(kClose, L("tabs.close"), icon(icons::kXmark));
  }

  ui::SimpleMenuModel* model() { return &model_; }

  void ExecuteCommand(int command_id, int event_flags) override {
    tabs::TabInterface* tab = tab_.Get();
    if (!tab) {
      return;
    }
    TabStripModel* model = browser_->GetTabStripModel();
    switch (command_id) {
      case kRemove:
        split::RemoveFromSplit(browser_, tab);
        break;
      case kShowOnly:
        split::ShowOnly(browser_, tab);
        break;
      case kEqualize:
        if (const std::optional<split_tabs::SplitTabId> split =
                tab->GetSplit()) {
          split::Equalize(browser_, *split);
        }
        break;
      case kClose: {
        const int index = model->GetIndexOfTab(tab);
        if (index != TabStripModel::kNoTab) {
          model->CloseWebContentsAt(
              index, TabCloseTypes::CLOSE_USER_GESTURE |
                         TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
        }
        break;
      }
    }
  }

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
  const tabs::TabHandle tab_;
  ui::SimpleMenuModel model_;
};

// 每一格顶上正中的把手。
class PaneHandle : public views::View, public views::ViewTargeterDelegate {
  METADATA_HEADER(PaneHandle, views::View)

 public:
  PaneHandle(SplitOverlay* overlay, int index)
      : overlay_(overlay), index_(index) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetWindowDraggable(this, false);
    capsule_ = AddChildView(std::make_unique<Painted>(base::BindRepeating(
        [](PaneHandle* handle, gfx::Canvas* canvas, const views::View* view) {
          // 深色的芯加一圈浅色的边：浅色、深色的网页上都看得见。
          const gfx::RectF rect(view->GetLocalBounds());
          cc::PaintFlags fill;
          fill.setAntiAlias(true);
          fill.setColor(SkColorSetA(SK_ColorBLACK, 0x4D));
          canvas->DrawRoundRect(rect, rect.height() / 2, fill);
          cc::PaintFlags border;
          border.setAntiAlias(true);
          border.setStyle(cc::PaintFlags::kStroke_Style);
          border.setStrokeWidth(0.5f);
          border.setColor(SkColorSetA(SK_ColorWHITE, 0x99));
          gfx::RectF stroke = rect;
          stroke.Inset(0.25f);
          canvas->DrawRoundRect(stroke, stroke.height() / 2, border);
        },
        base::Unretained(this))));
    capsule_->SetPaintToLayer();
    capsule_->layer()->SetFillsBoundsOpaquely(false);

    pill_ = AddChildView(std::make_unique<GlassView>());
    pill_->SetCanProcessEventsWithinSubtree(false);
    pill_content_ = pill_->AddChildView(std::make_unique<Painted>(
        base::BindRepeating(&PaneHandle::PaintPill, base::Unretained(this))));
    pill_->layer()->SetOpacity(0);
    pill_->SetVisible(false);
    GetViewAccessibility().SetRole(ax::mojom::Role::kButton);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    pill_->SetStyle(GlassFor(theme));
    pill_content_->SchedulePaint();
  }

  void SetData(const tabs::TabData& data) {
    data_ = data;
    title_ = TitleOf(data);
    GetViewAccessibility().SetName(
        L("split.handle.accessibilityLabel", {{"title", title_}}));
    pill_content_->SchedulePaint();
  }

  // 放到 pane（父视图的坐标）的顶上正中。
  void Place(const gfx::Rect& pane) {
    pane_ = pane;
    const int text = std::min(
        kPillTitleMax,
        gfx::GetStringWidth(title_, UiFont(12, gfx::Font::Weight::MEDIUM)));
    pill_width_ = kPillPadding + kPillIcon + kPillSpacing + text +
                  kPillSpacing + kPillEllipsis + kPillPadding;
    const int width = std::max(kHandleHitWidth, pill_width_);
    SetBoundsRect(gfx::Rect(pane.x() + (pane.width() - width) / 2, pane.y(),
                            width, kPillTop + kPillHeight));
    capsule_->SetBoundsRect(gfx::Rect((width - kCapsuleWidth) / 2, kCapsuleTop,
                                      kCapsuleWidth, kCapsuleHeight));
    pill_->SetGlassBounds(PillRect());
    pill_content_->SetBoundsRect(pill_->glass_rect());
  }

  // 展开的小条在本视图里的位置。
  gfx::Rect PillRect() const {
    return gfx::Rect((width() - pill_width_) / 2, kPillTop, pill_width_,
                     kPillHeight);
  }
  // 现在接鼠标的范围（本视图的坐标）。
  gfx::Rect HitRect() const {
    if (expanded_) {
      return PillRect();
    }
    return gfx::Rect((width() - kHandleHitWidth) / 2, 0, kHandleHitWidth,
                     kHandleHitHeight);
  }

  void SetMenuOpen(bool open) {
    menu_open_ = open;
    if (!open && !IsMouseHovered() && !dragging_) {
      SetExpanded(false);
    }
  }

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override {
    return HitRect().Intersects(rect);
  }

  // views::View:
  void OnMouseEntered(const ui::MouseEvent& event) override {
    SetExpanded(true);
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    if (!menu_open_ && !dragging_) {
      SetExpanded(false);
    }
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (event.IsRightMouseButton()) {
      gfx::Point point = event.location();
      views::View::ConvertPointToTarget(this, parent(), &point);
      overlay_->ShowPaneMenu(index_, point);
      return true;
    }
    if (!event.IsOnlyLeftMouseButton()) {
      return false;
    }
    press_ = event.location();
    return true;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!press_) {
      return false;
    }
    if (!dragging_) {
      const gfx::Vector2d moved = event.location() - *press_;
      if (std::abs(moved.x()) < kDragThreshold &&
          std::abs(moved.y()) < kDragThreshold) {
        return true;
      }
      dragging_ = true;
    }
    gfx::Point point = event.location();
    views::View::ConvertPointToTarget(this, parent(), &point);
    overlay_->PaneDragged(index_, point);
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (!press_) {
      return;
    }
    press_.reset();
    gfx::Point point = event.location();
    views::View::ConvertPointToTarget(this, parent(), &point);
    if (dragging_) {
      dragging_ = false;
      overlay_->PaneDragEnded(index_, point, /*cancelled=*/false);
      if (!IsMouseHovered()) {
        SetExpanded(false);
      }
      return;
    }
    if (event.IsOnlyLeftMouseButton()) {
      overlay_->ShowPaneMenu(index_, std::nullopt);
    }
  }
  void OnMouseCaptureLost() override {
    press_.reset();
    if (dragging_) {
      dragging_ = false;
      overlay_->PaneDragEnded(index_, gfx::Point(), /*cancelled=*/true);
      SetExpanded(false);
    }
  }
  std::u16string GetRenderedTooltipText(const gfx::Point& p) const override {
    return L("split.handle.help");
  }

 private:
  void SetExpanded(bool expanded) {
    if (expanded_ == expanded) {
      return;
    }
    expanded_ = expanded;
    ui::Layer* layer = pill_->layer();
    const gfx::Rect glass = pill_->glass_rect();
    const gfx::PointF origin(glass.CenterPoint().x(), glass.y());
    if (expanded) {
      pill_->SetVisible(true);
      if (!motion::ReduceMotion()) {
        LayerPose from;
        from.scale = 0.5f;
        pill_motion_.AnimateTransformFrom(layer, from, LayerPose(),
                                          MotionCurve::Of(motion::kSplitHandle),
                                          origin);
      }
      pill_motion_.AnimateOpacity(layer, 1, motion::Hover());
      capsule_motion_.AnimateOpacity(capsule_->layer(), 0, motion::Hover());
    } else {
      if (!motion::ReduceMotion()) {
        LayerPose to;
        to.scale = 0.5f;
        pill_motion_.AnimateTransform(
            layer, to, MotionCurve::Of(motion::kSplitHandle), origin);
      }
      pill_motion_.AnimateOpacity(layer, 0, motion::Hover(),
                                  base::BindOnce(
                                      [](base::WeakPtr<PaneHandle> handle) {
                                        if (handle && !handle->expanded_) {
                                          handle->pill_->SetVisible(false);
                                        }
                                      },
                                      weak_factory_.GetWeakPtr()));
      capsule_motion_.AnimateOpacity(capsule_->layer(), 1, motion::Hover());
    }
  }

  void PaintPill(gfx::Canvas* canvas, const views::View* view) {
    const gfx::Rect bounds = view->GetLocalBounds();
    PaintPanelTint(canvas, gfx::RectF(bounds), bounds.height() / 2.0f, theme_);
    int x = kPillPadding;
    DrawFavicon(
        canvas, view, data_.favicon,
        gfx::Rect(x, (bounds.height() - kPillIcon) / 2, kPillIcon, kPillIcon));
    x += kPillIcon + kPillSpacing;
    const int text_right =
        bounds.width() - kPillPadding - kPillEllipsis - kPillSpacing;
    canvas->DrawStringRectWithFlags(
        title_, UiFont(12, gfx::Font::Weight::MEDIUM), theme_.on_surface,
        gfx::Rect(x, 0, std::max(0, text_right - x), bounds.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    canvas->Save();
    canvas->Translate(
        gfx::Vector2d(bounds.width() - kPillPadding - kPillEllipsis,
                      (bounds.height() - kPillEllipsis) / 2));
    gfx::PaintVectorIcon(canvas, icons::kEllipsis, kPillEllipsis,
                         theme_.on_surface_variant);
    canvas->Restore();
  }

  const raw_ptr<SplitOverlay> overlay_;
  const int index_;
  Theme theme_ = Theme::DarkBase();
  tabs::TabData data_;
  std::u16string title_;
  gfx::Rect pane_;
  int pill_width_ = kHandleHitWidth;
  raw_ptr<Painted> capsule_ = nullptr;
  raw_ptr<GlassView> pill_ = nullptr;
  raw_ptr<Painted> pill_content_ = nullptr;
  LayerMotion pill_motion_;
  LayerMotion capsule_motion_;
  bool expanded_ = false;
  bool menu_open_ = false;
  bool dragging_ = false;
  std::optional<gfx::Point> press_;
  base::WeakPtrFactory<PaneHandle> weak_factory_{this};
};

BEGIN_METADATA(PaneHandle)
END_METADATA

// 分隔线拖过折叠阈值时，覆盖在将被移出的格子上的提示。
class CollapseHint : public views::View {
  METADATA_HEADER(CollapseHint, views::View)

 public:
  CollapseHint() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetCanProcessEventsWithinSubtree(false);
    SetVisible(false);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void Show(const gfx::Rect& pane) {
    SetBoundsRect(pane);
    SetVisible(true);
    fade_.AnimateOpacity(layer(), 1,
                         MotionCurve::Of(motion::kSplitCollapseArm));
  }
  void Hide() {
    if (!GetVisible()) {
      return;
    }
    fade_.AnimateOpacity(
        layer(), 0, MotionCurve::Of(motion::kSplitCollapseArm),
        base::BindOnce(
            [](base::WeakPtr<CollapseHint> hint) {
              if (hint && hint->layer()->GetTargetOpacity() == 0) {
                hint->SetVisible(false);
              }
            },
            weak_factory_.GetWeakPtr()));
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::Rect bounds = GetLocalBounds();
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(Alpha(theme_.surface, 0.94f));
    canvas->DrawRoundRect(gfx::RectF(bounds), radius::kContent, fill);

    const gfx::FontList& title_font = UiFont(13, gfx::Font::Weight::SEMIBOLD);
    const gfx::FontList& detail_font = UiFont(11);
    const int icon = 26;
    const int total =
        icon + 8 + title_font.GetHeight() + 8 + detail_font.GetHeight();
    int y = (bounds.height() - total) / 2;
    canvas->Save();
    canvas->Translate(gfx::Vector2d((bounds.width() - icon) / 2, y));
    gfx::PaintVectorIcon(canvas, icons::kSplitRemove, icon, theme_.accent);
    canvas->Restore();
    y += icon + 8;
    const gfx::Rect line(12, y, std::max(0, bounds.width() - 24),
                         title_font.GetHeight());
    canvas->DrawStringRectWithFlags(
        L("split.collapseHint.title"), title_font, theme_.on_surface, line,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    y += title_font.GetHeight() + 8;
    canvas->DrawStringRectWithFlags(
        L("split.collapseHint.detail"), detail_font, theme_.on_surface_variant,
        gfx::Rect(12, y, std::max(0, bounds.width() - 24),
                  detail_font.GetHeight()),
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  Theme theme_ = Theme::DarkBase();
  LayerMotion fade_;
  base::WeakPtrFactory<CollapseHint> weak_factory_{this};
};

BEGIN_METADATA(CollapseHint)
END_METADATA

// 落点预览中的单个槽位：拖入项以强调色高亮并标明位置，其余槽位淡显以区分来源；
// 使用较高不透明度避免底层网页文字透出干扰标题阅读。
class DropSlot : public views::View {
  METADATA_HEADER(DropSlot, views::View)

 public:
  DropSlot() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void Set(const tabs::TabData& data,
           bool dragged,
           std::optional<std::u16string> caption,
           const Theme& theme) {
    data_ = data;
    title_ = TitleOf(data);
    dragged_ = dragged;
    caption_ = std::move(caption);
    theme_ = theme;
    SchedulePaint();
  }

  // 格子在父视图里的位置（不含阴影的边）。
  void Place(const gfx::Rect& frame) {
    gfx::Rect outset = frame;
    outset.Outset(kSlotShadow);
    SetBoundsRect(outset);
  }
  gfx::Rect frame() const {
    gfx::Rect rect = bounds();
    rect.Inset(kSlotShadow);
    return rect;
  }
  LayerMotion& motion() { return motion_; }

  void OnPaint(gfx::Canvas* canvas) override {
    gfx::RectF card(GetLocalBounds());
    card.Inset(kSlotShadow);
    const float radius = radius::kContent;
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(Alpha(theme_.surface_panel, dragged_ ? 0.96f : 0.9f));
    if (dragged_) {
      fill.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
          gfx::Vector2d(0, 6), 32,
          SkColorSetA(SK_ColorBLACK, theme_.is_dark ? 0x59 : 0x24))}));
    }
    canvas->DrawRoundRect(card, radius, fill);
    if (dragged_) {
      cc::PaintFlags tint;
      tint.setAntiAlias(true);
      tint.setColor(Alpha(theme_.accent, theme_.is_dark ? 0.16f : 0.1f));
      canvas->DrawRoundRect(card, radius, tint);
    }
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    const float stroke_width = dragged_ ? 2 : 1;
    border.setStrokeWidth(stroke_width);
    border.setColor(dragged_ ? Alpha(theme_.accent, 0.85f)
                             : theme_.OnSurface(0.12f));
    gfx::RectF stroke = card;
    stroke.Inset(stroke_width / 2);
    canvas->DrawRoundRect(stroke, radius - stroke_width / 2, border);

    // 中间：图标、标题（最多两行）、放在哪一边。
    const int icon = dragged_ ? 28 : 20;
    const gfx::FontList& font =
        UiFont(dragged_ ? 13 : 12, dragged_ ? gfx::Font::Weight::SEMIBOLD
                                            : gfx::Font::Weight::MEDIUM);
    const gfx::FontList& caption_font = UiFont(11, gfx::Font::Weight::MEDIUM);
    const int text_width = std::min(220, static_cast<int>(card.width()) - 32);
    int lines_height = 0;
    {
      int w = text_width;
      int h = 0;
      gfx::Canvas::SizeStringInt(
          title_, font, &w, &h, 0,
          gfx::Canvas::MULTI_LINE | gfx::Canvas::TEXT_ALIGN_CENTER);
      lines_height = std::min(h, font.GetHeight() * 2);
    }
    const int caption_height = caption_ ? 20 : 0;
    const int total =
        icon + 8 + lines_height + (caption_ ? 8 + caption_height : 0);
    const gfx::Rect inner = gfx::ToEnclosingRect(card);
    int y = inner.y() + (inner.height() - total) / 2;
    DrawFavicon(canvas, this, data_.favicon,
                gfx::Rect(inner.CenterPoint().x() - icon / 2, y, icon, icon));
    y += icon + 8;
    canvas->DrawStringRectWithFlags(
        title_, font, dragged_ ? theme_.on_surface : theme_.on_surface_variant,
        gfx::Rect(inner.CenterPoint().x() - text_width / 2, y,
                  std::max(0, text_width), lines_height),
        gfx::Canvas::MULTI_LINE | gfx::Canvas::TEXT_ALIGN_CENTER |
            gfx::Canvas::NO_SUBPIXEL_RENDERING);
    y += lines_height + 8;
    if (caption_) {
      const int width = gfx::GetStringWidth(*caption_, caption_font) + 16;
      const gfx::RectF pill(inner.CenterPoint().x() - width / 2.0f, y, width,
                            caption_height);
      cc::PaintFlags pill_fill;
      pill_fill.setAntiAlias(true);
      pill_fill.setColor(Alpha(theme_.accent, 0.14f));
      canvas->DrawRoundRect(pill, caption_height / 2.0f, pill_fill);
      canvas->DrawStringRectWithFlags(
          *caption_, caption_font, theme_.accent, gfx::ToEnclosingRect(pill),
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }
  }

 private:
  tabs::TabData data_;
  std::u16string title_;
  bool dragged_ = false;
  std::optional<std::u16string> caption_;
  Theme theme_ = Theme::DarkBase();
  LayerMotion motion_;
};

BEGIN_METADATA(DropSlot)
END_METADATA

// 拖标签到内容区时的落点预览浮层。
class DropPreview : public views::View {
  METADATA_HEADER(DropPreview, views::View)

 public:
  DropPreview()
      : slide_(
            1,
            base::BindRepeating(&DropPreview::Step, base::Unretained(this))) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetCanProcessEventsWithinSubtree(false);
    SetVisible(false);
    refusal_ = AddChildView(std::make_unique<GlassView>());
    refusal_content_ =
        refusal_->AddChildView(std::make_unique<Painted>(base::BindRepeating(
            &DropPreview::PaintRefusal, base::Unretained(this))));
    refusal_->SetVisible(false);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    refusal_->SetStyle(GlassFor(theme));
    SchedulePaint();
  }

  // card：内容区（父视图的坐标）。
  void Show(const gfx::Rect& card, const split::Drop& drop) {
    const bool was_shown = shown_;
    SetBoundsRect(card);
    if (!shown_) {
      SetVisible(true);
      fade_.AnimateOpacity(layer(), 1, motion::Hover());
      shown_ = true;
    }
    const gfx::Rect local(card.size());
    std::map<tabs::TabHandle, gfx::Rect> targets;
    if (drop.preview) {
      const auto frames = split::Frames(drop.preview->layout, local).panes;
      for (size_t i = 0; i < frames.size(); ++i) {
        targets[drop.preview->tabs[i]] = frames[i];
      }
    }
    // 不在预览里的格子淡出拿掉。
    for (auto it = slots_.begin(); it != slots_.end();) {
      if (targets.contains(it->first)) {
        ++it;
        continue;
      }
      RemoveSlot(it->second);
      it = slots_.erase(it);
    }
    from_.clear();
    to_.clear();
    bool moving = false;
    for (const auto& [handle, frame] : targets) {
      tabs::TabInterface* tab = handle.Get();
      if (!tab) {
        continue;
      }
      const bool dragged = handle == drop.dragged;
      DropSlot* slot = nullptr;
      if (auto it = slots_.find(handle); it != slots_.end()) {
        slot = it->second;
        from_[handle] = slot->frame();
        moving |= slot->frame() != frame;
      } else {
        slot = AddChildView(std::make_unique<DropSlot>());
        slots_[handle] = slot;
        slot->Place(frame);
        from_[handle] = frame;
        slot->motion().AnimateOpacityFrom(slot->layer(), 0, 1, motion::Hover());
      }
      to_[handle] = frame;
      slot->Set(StarryTabData(tab), dragged,
                dragged ? std::optional<std::u16string>(
                              split::PlacementTitle(drop.edge))
                        : std::nullopt,
                theme_);
    }
    if (moving && was_shown && !motion::ReduceMotion()) {
      slide_.SetValue(0);
      slide_.AnimateTo(1, MotionCurve::Of(motion::kSplitDropPreview));
    } else {
      slide_.SetValue(1);
      Step();
    }

    // 满了、别的窗口：说明原因，不接。
    refusal_kind_ = drop.refusal;
    if (drop.refusal) {
      const gfx::FontList& font = UiFont(13, gfx::Font::Weight::MEDIUM);
      const int width =
          16 + 13 + 8 + gfx::GetStringWidth(RefusalText(), font) + 16;
      const gfx::Rect pill((local.width() - width) / 2,
                           (local.height() - kRefusalHeight) / 2, width,
                           kRefusalHeight);
      refusal_->SetCornerRadius(kRefusalHeight / 2.0f, false);
      refusal_->SetGlassBounds(pill);
      refusal_content_->SetBoundsRect(refusal_->glass_rect());
      refusal_content_->SchedulePaint();
      if (!refusal_->GetVisible()) {
        refusal_->SetVisible(true);
        LayerPose from;
        from.scale = motion::ReduceMotion() ? 1 : 0.9f;
        const gfx::Rect glass = refusal_->glass_rect();
        refusal_motion_.AnimateTransformFrom(
            refusal_->layer(), from, LayerPose(),
            MotionCurve::Of(motion::kSplitDropPreview),
            gfx::PointF(glass.CenterPoint()));
        refusal_motion_.AnimateOpacityFrom(refusal_->layer(), 0, 1,
                                           motion::Hover());
      }
    } else if (refusal_->GetVisible()) {
      refusal_->SetVisible(false);
    }
    SchedulePaint();
  }

  void Hide() {
    if (!shown_) {
      return;
    }
    shown_ = false;
    fade_.AnimateOpacity(layer(), 0, motion::Hover(),
                         base::BindOnce(
                             [](base::WeakPtr<DropPreview> preview) {
                               if (preview && !preview->shown_) {
                                 preview->Clear();
                               }
                             },
                             weak_factory_.GetWeakPtr()));
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(Alpha(theme_.surface, theme_.is_dark ? 0.62f : 0.55f));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kContent, fill);
  }

 private:
  std::u16string RefusalText() const {
    if (refusal_kind_ == split::Refusal::kOtherWindow) {
      return L("split.refusal.otherWindow");
    }
    return L("split.refusal.full", {{"count", split::kMaxPanes}});
  }

  void PaintRefusal(gfx::Canvas* canvas, const views::View* view) {
    const gfx::Rect bounds = view->GetLocalBounds();
    PaintPanelTint(canvas, gfx::RectF(bounds), bounds.height() / 2.0f, theme_);
    const int icon = 13;
    canvas->Save();
    canvas->Translate(gfx::Vector2d(16, (bounds.height() - icon) / 2));
    gfx::PaintVectorIcon(canvas,
                         refusal_kind_ == split::Refusal::kOtherWindow
                             ? icons::kMacWindow
                             : icons::kSplit3x1,
                         icon, theme_.warning());
    canvas->Restore();
    const int x = 16 + icon + 8;
    canvas->DrawStringRectWithFlags(
        RefusalText(), UiFont(13, gfx::Font::Weight::MEDIUM), theme_.on_surface,
        gfx::Rect(x, 0, std::max(0, bounds.width() - x - 16), bounds.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void Step() {
    for (auto& [handle, slot] : slots_) {
      auto from = from_.find(handle);
      auto to = to_.find(handle);
      if (from == from_.end() || to == to_.end()) {
        continue;
      }
      slot->Place(gfx::Tween::RectValueBetween(slide_.value(), from->second,
                                               to->second));
    }
  }

  void RemoveSlot(DropSlot* slot) {
    slot->motion().AnimateOpacity(
        slot->layer(), 0, motion::Hover(),
        base::BindOnce(
            [](base::WeakPtr<DropPreview> preview, DropSlot* slot) {
              if (!preview) {
                return;
              }
              for (views::View* child : preview->children()) {
                if (child == slot) {
                  preview->RemoveChildViewT(slot);
                  return;
                }
              }
            },
            weak_factory_.GetWeakPtr(), base::Unretained(slot)));
  }

  void Clear() {
    for (auto& [handle, slot] : slots_) {
      RemoveChildViewT(slot.get());
    }
    slots_.clear();
    from_.clear();
    to_.clear();
    refusal_->SetVisible(false);
    SetVisible(false);
  }

  Theme theme_ = Theme::DarkBase();
  bool shown_ = false;
  LayerMotion fade_;
  ValueMotion slide_;
  std::map<tabs::TabHandle, raw_ptr<DropSlot>> slots_;
  std::map<tabs::TabHandle, gfx::Rect> from_;
  std::map<tabs::TabHandle, gfx::Rect> to_;
  raw_ptr<GlassView> refusal_ = nullptr;
  raw_ptr<Painted> refusal_content_ = nullptr;
  LayerMotion refusal_motion_;
  std::optional<split::Refusal> refusal_kind_;
  base::WeakPtrFactory<DropPreview> weak_factory_{this};
};

BEGIN_METADATA(DropPreview)
END_METADATA

}  // namespace split_ui

namespace {

// 拖过头时的阻力：越拖越慢，最多再多出 limit。
double RubberBand(double overshoot, double limit = 40) {
  if (overshoot <= 0) {
    return 0;
  }
  return limit * (1 - 1 / (overshoot * 0.55 / limit + 1));
}

}  // namespace

SplitOverlay::ResizeState::ResizeState(split_tabs::SplitTabId id) : split(id) {}
SplitOverlay::ResizeState::ResizeState(const ResizeState&) = default;
SplitOverlay::ResizeState& SplitOverlay::ResizeState::operator=(
    const ResizeState&) = default;
SplitOverlay::ResizeState::~ResizeState() = default;

SplitOverlay::SplitOverlay(StarryBrowserView* browser_view)
    : browser_view_(browser_view) {
  // Aura 用命中视图的图层判断鼠标事件是否要交给下面的网页；把手自己不画内容，
  // 给覆盖层建不绘制图层以免悬停和点击穿透到网页。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  ring_ = AddChildView(std::make_unique<split_ui::FocusRing>());
  for (size_t i = 0; i < dividers_.size(); ++i) {
    dividers_[i] = AddChildView(std::make_unique<split_ui::Divider>(this, i));
    dividers_[i]->SetVisible(false);
  }
  for (int i = 0; i < split::kMaxPanes; ++i) {
    handles_[i] = AddChildView(std::make_unique<split_ui::PaneHandle>(this, i));
    handles_[i]->SetVisible(false);
  }
  hint_ = AddChildView(std::make_unique<split_ui::CollapseHint>());
  preview_ = AddChildView(std::make_unique<split_ui::DropPreview>());
  browser_view_->browser()->GetTabStripModel()->AddObserver(this);
}

// 观察者在 TabStripModelObserver 的析构里自己去掉。
SplitOverlay::~SplitOverlay() = default;

void SplitOverlay::SetTheme(const Theme& theme) {
  theme_ = theme;
  ring_->SetTheme(theme);
  for (split_ui::Divider* divider : dividers_) {
    divider->SetTheme(theme);
  }
  for (split_ui::PaneHandle* handle : handles_) {
    handle->SetTheme(theme);
  }
  hint_->SetTheme(theme);
  preview_->SetTheme(theme);
}

void SplitOverlay::OnThemeChanged() {
  views::View::OnThemeChanged();
  UpdateHandles();
}

void SplitOverlay::ObserveContainers() {
  MultiContentsView* contents = browser_view_->multi_contents_view();
  if (!contents) {
    return;
  }
  std::vector<views::View*> sources = {contents};
  for (ContentsContainerView* container :
       contents->contents_container_views()) {
    sources.push_back(container);
  }
  for (views::View* source : sources) {
    if (!container_observations_.IsObservingSource(source)) {
      container_observations_.AddObservation(source);
    }
  }
}

gfx::Rect SplitOverlay::CardBounds() const {
  MultiContentsView* contents = browser_view_->multi_contents_view();
  if (!contents || !contents->parent()) {
    return gfx::Rect();
  }
  return views::View::ConvertRectToTarget(contents->parent(), this,
                                          contents->bounds());
}

void SplitOverlay::UpdateGeometry() {
  MultiContentsView* contents = browser_view_->multi_contents_view();
  if (!contents || !GetWidget()) {
    return;
  }
  ObserveContainers();
  const gfx::Rect card = CardBounds();
  panes_.reset();
  if (contents->IsInSplitView()) {
    layout_ = contents->GetSplitLayout();
    std::vector<Pane> panes;
    bool valid = true;
    for (ContentsContainerView* container :
         contents->contents_container_views()) {
      if (!container->GetVisible()) {
        continue;
      }
      if (!container->parent()) {
        valid = false;
        break;
      }
      Pane pane;
      pane.bounds = views::View::ConvertRectToTarget(container->parent(), this,
                                                     container->bounds());
      content::WebContents* web_contents =
          container->contents_view()->web_contents();
      tabs::TabInterface* tab =
          web_contents ? tabs::TabInterface::MaybeGetFromContents(web_contents)
                       : nullptr;
      if (!tab) {
        valid = false;
        break;
      }
      pane.tab = tab->GetHandle();
      panes.push_back(pane);
    }
    if (valid && panes.size() >= 2) {
      panes_ = panes;
    }
  }

  // 窗口底上的卡片和阴影：分屏时每一格一张。紧凑地址栏时网页上边不圆（细栏自己画圆角），
  // 卡片的圆角看下边。
  WindowBackgroundView* background = browser_view_->background_view();
  const float radius = contents->GetBackgroundRadii().lower_left();
  std::vector<gfx::Rect> cards;
  if (radius > 0) {
    if (panes_) {
      for (const Pane& pane : *panes_) {
        cards.push_back(
            views::View::ConvertRectToTarget(this, background, pane.bounds));
      }
    } else {
      cards.push_back(views::View::ConvertRectToTarget(this, background, card));
    }
  }
  background->SetCards(std::move(cards), static_cast<int>(radius));

  // 当前一格的描边。
  tabs::TabInterface* active =
      browser_view_->browser()->GetActiveTabInterface();
  std::optional<gfx::Rect> ring;
  std::optional<tabs::TabHandle> ring_tab;
  if (panes_ && active) {
    for (const Pane& pane : *panes_) {
      if (pane.tab == active->GetHandle()) {
        ring = pane.bounds;
        ring_tab = pane.tab;
      }
    }
  }
  ring_->MoveTo(ring, ring_tab_ && ring_tab && *ring_tab_ != *ring_tab);
  ring_tab_ = ring_tab;

  divider_geometry_.clear();
  if (panes_ && active && active->GetSplit()) {
    auto* model = browser_view_->browser()->GetTabStripModel();
    divider_geometry_ =
        split::Frames(split::GetLayout(model, *active->GetSplit()), card)
            .dividers;
  }
  for (size_t i = 0; i < dividers_.size(); ++i) {
    auto* divider = dividers_[i].get();
    const bool visible = i < divider_geometry_.size();
    if (visible) {
      divider->SetSideBySide(divider_geometry_[i].horizontal);
      divider->SetBoundsRect(divider_geometry_[i].bounds);
    }
    divider->SetVisible(visible);
  }
  UpdateHandles();
  if (resize_ && !resize_->collapsing.empty() && panes_) {
    gfx::Rect collapsing;
    for (int index : resize_->collapsing) {
      if (static_cast<size_t>(index) < panes_->size()) {
        collapsing.Union((*panes_)[index].bounds);
      }
    }
    hint_->Show(collapsing);
  }
  if (drop_) {
    preview_->Show(card, *drop_);
  }
}

void SplitOverlay::UpdateHandles() {
  for (size_t i = 0; i < handles_.size(); ++i) {
    split_ui::PaneHandle* handle = handles_[i];
    tabs::TabInterface* tab =
        panes_ && i < panes_->size() ? (*panes_)[i].tab.Get() : nullptr;
    if (!tab) {
      handle->SetVisible(false);
      continue;
    }
    handle->SetData(StarryTabData(tab));
    // 紧凑地址栏的细栏在格子顶上：把手放在网页的顶上。
    gfx::Rect page = (*panes_)[i].bounds;
    page.Inset(gfx::Insets::TLBR(browser_view_->compact_inset(), 0, 0, 0));
    handle->Place(page);
    handle->SetVisible(true);
  }
}

std::optional<SplitOverlay::Pane> SplitOverlay::PaneAt(
    const gfx::Point& point) const {
  if (panes_) {
    for (const Pane& pane : *panes_) {
      if (pane.bounds.Contains(point)) {
        return pane;
      }
    }
    return std::nullopt;
  }
  const gfx::Rect card = CardBounds();
  tabs::TabInterface* active =
      browser_view_->browser()->GetActiveTabInterface();
  if (!active || !card.Contains(point)) {
    return std::nullopt;
  }
  return Pane{card, active->GetHandle()};
}

bool SplitOverlay::DragTabTo(tabs::TabInterface* tab, const gfx::Point& point) {
  const gfx::Rect card = CardBounds();
  if (!tab || !card.Contains(point)) {
    if (drop_) {
      drop_.reset();
      preview_->Hide();
    }
    return false;
  }
  const std::optional<Pane> pane = PaneAt(point);
  if (!pane) {
    // 两格之间的空隙：保持上一次的预览，移到旁边的格子时不闪。
    return true;
  }
  const split::Drop drop =
      split::Evaluate(browser_view_->browser()->GetTabStripModel(), tab,
                      pane->tab.Get(), split::NearestEdge(point, pane->bounds));
  if (!drop.preview && !drop.refusal) {
    // 拖到自己身上：什么都不显示。
    if (drop_) {
      drop_.reset();
      preview_->Hide();
    }
    return true;
  }
  if (drop_ != drop) {
    drop_ = drop;
    preview_->Show(card, drop);
  }
  return true;
}

bool SplitOverlay::EndTabDrag(bool commit) {
  std::optional<split::Drop> drop = std::move(drop_);
  drop_.reset();
  preview_->Hide();
  if (!commit || !drop || !drop->preview) {
    return false;
  }
  // 拖动的鼠标事件尚未结束，延后到下一轮消息循环再应用分屏。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<SplitOverlay> overlay, split::Drop drop) {
                       if (overlay) {
                         split::Apply(overlay->browser_view_->browser(), drop);
                       }
                     },
                     weak_factory_.GetWeakPtr(), *drop));
  return true;
}

gfx::Rect SplitOverlay::PaneBounds(int index) const {
  return panes_ && index >= 0 && static_cast<size_t>(index) < panes_->size()
             ? (*panes_)[index].bounds
             : gfx::Rect();
}

gfx::Rect SplitOverlay::DividerBounds(int index) const {
  return index >= 0 && static_cast<size_t>(index) < dividers_.size() &&
                 dividers_[index]->GetVisible()
             ? dividers_[index]->bounds()
             : gfx::Rect();
}

gfx::Rect SplitOverlay::HandleBounds(int index) const {
  if (index < 0 || index >= split::kMaxPanes ||
      !handles_[index]->GetVisible()) {
    return gfx::Rect();
  }
  split_ui::PaneHandle* handle = handles_[index];
  return handle->HitRect() + handle->bounds().OffsetFromOrigin();
}

void SplitOverlay::ShowPaneMenu(int index, std::optional<gfx::Point> at) {
  if (!panes_ || index < 0 || static_cast<size_t>(index) >= panes_->size()) {
    return;
  }
  tabs::TabInterface* tab = (*panes_)[index].tab.Get();
  if (!tab) {
    return;
  }
  split_ui::PaneHandle* handle = handles_[index];
  menu_ = std::make_unique<split_ui::PaneMenu>(browser_view_->browser(), tab);
  MenuOptions options;
  gfx::Point point;
  if (at) {
    point = views::View::ConvertPointToTarget(this, browser_view_, *at);
  } else {
    const gfx::Rect pill =
        handle->PillRect() + handle->bounds().OffsetFromOrigin();
    point = views::View::ConvertPointToTarget(this, browser_view_,
                                              pill.bottom_center());
    options.anchor = views::View::ConvertRectToTarget(
        this, browser_view_->menu_host(), pill);
  }
  handle->SetMenuOpen(true);
  browser_view_->ShowMenu(
      menu_->model(), point,
      base::BindOnce(
          [](base::WeakPtr<SplitOverlay> overlay, int index) {
            if (overlay) {
              overlay->handles_[index]->SetMenuOpen(false);
            }
          },
          weak_factory_.GetWeakPtr(), index),
      std::move(options));
}

std::string SplitOverlay::DebugDescription() const {
  if (!panes_) {
    return "split=no";
  }
  TabStripModel* model = browser_view_->browser()->GetTabStripModel();
  tabs::TabInterface* first = (*panes_)[0].tab.Get();
  double ratio = 0;
  if (first && first->GetSplit() && model->ContainsSplit(*first->GetSplit())) {
    ratio =
        model->GetSplitData(*first->GetSplit())->visual_data()->split_ratio();
  }
  std::string text = base::StrCat(
      {"split=yes layout=",
       layout_ == split_tabs::SplitTabLayout::kSideBySide ? "side" : "stacked",
       " ratio=", base::NumberToString(ratio)});
  tabs::TabInterface* active =
      browser_view_->browser()->GetActiveTabInterface();
  for (size_t i = 0; i < panes_->size(); ++i) {
    tabs::TabInterface* tab = (*panes_)[i].tab.Get();
    base::StrAppend(
        &text,
        {" pane", base::NumberToString(i), "=", (*panes_)[i].bounds.ToString(),
         "[", tab ? base::UTF16ToUTF8(tab->GetContents()->GetTitle()) : "", "]",
         tab && tab == active ? "*" : "", " handle", base::NumberToString(i),
         "=", HandleBounds(static_cast<int>(i)).ToString()});
  }
  for (size_t i = 0; i < divider_geometry_.size(); ++i) {
    base::StrAppend(&text, {" divider", base::NumberToString(i), "=",
                            DividerBounds(i).ToString()});
  }
  return text;
}

void SplitOverlay::BeginResize(int index) {
  if (!panes_ || index < 0 ||
      static_cast<size_t>(index) >= divider_geometry_.size()) {
    return;
  }
  auto* first = (*panes_)[0].tab.Get();
  auto* model = browser_view_->browser()->GetTabStripModel();
  if (!first || !first->GetSplit() ||
      !model->ContainsSplit(*first->GetSplit())) {
    return;
  }
  ResizeState state(*first->GetSplit());
  state.start_layout = split::GetLayout(model, state.split);
  state.divider_index = index;
  state.divider = divider_geometry_[index];
  state.side_by_side = state.divider.horizontal;
  auto* node = state.start_layout.At(state.divider.path);
  const int length = state.side_by_side ? state.divider.container.width()
                                        : state.divider.container.height();
  state.available = std::max(
      1, length - split::kGap * static_cast<int>(node->children.size() - 1));
  state.leading = node->fractions[state.divider.index] * state.available;
  state.pair = state.leading +
               node->fractions[state.divider.index + 1] * state.available;
  resize_ = state;
}

void SplitOverlay::Resize(int translation) {
  if (!resize_ || resize_->pair <= 0) {
    return;
  }
  const double minimum =
      resize_->side_by_side ? split::kPaneMinWidth : split::kPaneMinHeight;
  const double pair = resize_->pair;
  const double floor = std::min(minimum, pair * 0.3);
  double size = resize_->leading + translation;
  split::Layout layout = resize_->start_layout;
  auto* node = layout.At(resize_->divider.path);
  const int index = resize_->divider.index;
  std::vector<int> collapsing;
  if (size < floor) {
    const double overshoot = floor - size;
    size = floor - RubberBand(overshoot);
    if (overshoot >= split::kCollapseDistance) {
      collapsing = node->children[index].Panes();
    }
  } else if (size > pair - floor) {
    const double overshoot = size - (pair - floor);
    size = pair - floor + RubberBand(overshoot);
    if (overshoot >= split::kCollapseDistance) {
      collapsing = node->children[index + 1].Panes();
    }
  }
  size = std::clamp(size, pair * 0.001, pair * 0.999);
  node->fractions[index] = size / resize_->available;
  node->fractions[index + 1] = (pair - size) / resize_->available;
  SetCollapsing(collapsing);
  split::SetLayout(browser_view_->browser(), resize_->split, layout, false);
}

void SplitOverlay::EndResize(bool commit) {
  if (!resize_) {
    return;
  }
  const ResizeState state = *resize_;
  resize_.reset();
  SetCollapsing({});
  auto* browser = browser_view_->browser();
  auto* model = browser->GetTabStripModel();
  if (!model->ContainsSplit(state.split)) {
    return;
  }
  if (!commit || !state.collapsing.empty()) {
    split::SetLayout(browser, state.split, state.start_layout, true);
  }
  if (commit && !state.collapsing.empty()) {
    std::vector<tabs::TabHandle> leaving;
    const auto tabs = model->GetSplitData(state.split)->ListTabs();
    for (int index : state.collapsing) {
      leaving.push_back(tabs[index]->GetHandle());
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<SplitOverlay> overlay,
                          std::vector<tabs::TabHandle> leaving) {
                         if (!overlay) {
                           return;
                         }
                         for (auto handle : leaving) {
                           if (handle.Get()) {
                             split::RemoveFromSplit(
                                 overlay->browser_view_->browser(),
                                 handle.Get());
                           }
                         }
                       },
                       weak_factory_.GetWeakPtr(), leaving));
  } else if (commit) {
    split::SetLayout(browser, state.split, split::GetLayout(model, state.split),
                     true);
  }
}

void SplitOverlay::Equalize() {
  if (!panes_) {
    return;
  }
  tabs::TabInterface* first = (*panes_)[0].tab.Get();
  if (first && first->GetSplit()) {
    split::Equalize(browser_view_->browser(), *first->GetSplit());
  }
}

void SplitOverlay::SetCollapsing(const std::vector<int>& indices) {
  if (resize_) {
    resize_->collapsing = indices;
  }
  for (size_t i = 0; i < dividers_.size(); ++i) {
    dividers_[i]->SetArmed(resize_ &&
                           resize_->divider_index == static_cast<int>(i) &&
                           !indices.empty());
  }
  if (!indices.empty() && panes_) {
    gfx::Rect bounds;
    for (int index : indices) {
      bounds.Union((*panes_)[index].bounds);
    }
    hint_->Show(bounds);
  } else {
    hint_->Hide();
  }
}

void SplitOverlay::PaneDragged(int index, const gfx::Point& point) {
  if (!panes_ || index < 0 || static_cast<size_t>(index) >= panes_->size()) {
    return;
  }
  DragTabTo((*panes_)[index].tab.Get(), point);
}

void SplitOverlay::PaneDragEnded(int index,
                                 const gfx::Point& point,
                                 bool cancelled) {
  tabs::TabInterface* tab =
      panes_ && index >= 0 && static_cast<size_t>(index) < panes_->size()
          ? (*panes_)[index].tab.Get()
          : nullptr;
  if (cancelled || !tab) {
    EndTabDrag(false);
    return;
  }
  if (CardBounds().Contains(point)) {
    EndTabDrag(true);
    return;
  }
  EndTabDrag(false);
  // 拖出了内容区（侧栏、窗口边上）：移出分屏。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<SplitOverlay> overlay, tabs::TabHandle handle) {
            if (overlay && handle.Get()) {
              split::RemoveFromSplit(overlay->browser_view_->browser(),
                                     handle.Get());
            }
          },
          weak_factory_.GetWeakPtr(), tab->GetHandle()));
}

bool SplitOverlay::DoesIntersectRect(const views::View* target,
                                     const gfx::Rect& rect) const {
  for (split_ui::Divider* divider : dividers_) {
    if (divider->GetVisible() && divider->bounds().Intersects(rect)) {
      return true;
    }
  }
  for (split_ui::PaneHandle* handle : handles_) {
    if (handle->GetVisible() &&
        (handle->HitRect() + handle->bounds().OffsetFromOrigin())
            .Intersects(rect)) {
      return true;
    }
  }
  return false;
}

void SplitOverlay::OnViewBoundsChanged(views::View* observed_view) {
  UpdateGeometry();
}

void SplitOverlay::OnViewVisibilityChanged(views::View* observed_view,
                                           views::View* starting_view,
                                           bool visible) {
  UpdateGeometry();
}

void SplitOverlay::OnViewIsDeleting(views::View* observed_view) {
  container_observations_.RemoveObservation(observed_view);
}

void SplitOverlay::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  UpdateGeometry();
  // Chrome 的窗口可能在这之后才换内容区里的网页。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SplitOverlay::UpdateGeometry,
                                weak_factory_.GetWeakPtr()));
}

void SplitOverlay::OnTabChangedAt(tabs::TabInterface* tab,
                                  TabChangeType change_type) {
  if (!panes_) {
    return;
  }
  for (const Pane& pane : *panes_) {
    if (pane.tab == tab->GetHandle()) {
      UpdateHandles();
      return;
    }
  }
}

void SplitOverlay::OnSplitTabChanged(const SplitTabChange& change) {
  UpdateGeometry();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SplitOverlay::UpdateGeometry,
                                weak_factory_.GetWeakPtr()));
}

BEGIN_METADATA(SplitOverlay)
END_METADATA

// 上游的钩子（starry_hooks.h）。

bool OverrideSplitPaneDecorations(ContentsContainerView* container,
                                  bool is_in_split) {
  StarryBrowserView* view = nullptr;
  for (views::View* ancestor = container->parent(); ancestor && !view;
       ancestor = ancestor->parent()) {
    view = views::AsViewClass<StarryBrowserView>(ancestor);
  }
  if (!view || !view->has_starry_chrome()) {
    return false;
  }
  // Starry 的格子：不要 Chrome
  // 的内边距、描边和角上的小工具条，圆角同内容卡片。紧凑地址
  // 栏时顶上留出细栏的位置。
  view->ApplyPaneInsets(container);
  container->mini_toolbar()->SetVisible(false);
  if (const views::View* outline = container->contents_outline_view()) {
    const_cast<views::View*>(outline)->SetVisible(false);
  }
  if (is_in_split) {
    MultiContentsView* contents = view->multi_contents_view();
    container->SetRoundedCorners(contents ? contents->GetBackgroundRadii()
                                          : gfx::RoundedCornersF());
  }
  return true;
}

}  // namespace starry
