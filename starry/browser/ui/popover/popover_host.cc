// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/popover/popover_host.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/frame_motion.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/event_observer.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/views/event_monitor.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 离窗口边、挂着的东西多远。
constexpr int kWindowInset = 8;
constexpr int kAnchorGap = 6;
constexpr int kInsideInset = 10;
// 长出来时内容在进度的这一段里淡入（收起时反过来先淡出），缩回去的最后这一段
// 整张卡片淡出。
constexpr double kGrowContentFrom = 0.45;
constexpr double kGrowContentSpan = 0.45;
constexpr double kShrinkFadeSpan = 0.3;

// 卡片的底：比菜单实一点的面板色、1 的细描边（同下载列表）。
class PopoverBackground : public views::View {
  METADATA_HEADER(PopoverBackground, views::View)

 public:
  PopoverBackground() { SetCanProcessEventsWithinSubtree(false); }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void SetRadius(float radius) {
    if (radius_ != radius) {
      radius_ = radius;
      SchedulePaint();
    }
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(theme_.surface_panel,
                              theme_.is_dark ? 0.66f * 255 : 0.76f * 255));
    canvas->DrawRoundRect(bounds, radius_, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, std::max(0.0f, radius_ - 0.5f), border);
  }

 private:
  Theme theme_ = Theme::DarkBase();
  float radius_ = radius::kCard;
};

BEGIN_METADATA(PopoverBackground)
END_METADATA

}  // namespace

PopoverContent::PopoverContent() = default;
PopoverContent::~PopoverContent() = default;

void PopoverContent::SetTheme(const Theme& theme) {}

views::View* PopoverContent::InitialFocus() {
  return nullptr;
}

void PopoverContent::WillClose() {}

bool PopoverContent::HandlesEscape() const {
  return false;
}

BEGIN_METADATA(PopoverContent)
END_METADATA

// 卡片：玻璃、底、内容。
class PopoverCard : public GlassView {
  METADATA_HEADER(PopoverCard, GlassView)

 public:
  PopoverCard(PopoverHost* host,
              std::unique_ptr<PopoverContent> content,
              const Theme& theme)
      : host_(host) {
    SetCornerRadius(radius::kCard, true);
    background_ = AddChildView(std::make_unique<PopoverBackground>());
    content_ = AddChildView(std::move(content));
    SetTheme(theme);
  }

  void SetTheme(const Theme& theme) {
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    background_->SetTheme(theme);
    content_->SetTheme(theme);
  }

  PopoverContent* content() { return content_; }
  LayerMotion& motion() { return motion_; }

  void Place(const gfx::Rect& card) {
    SetGlassBounds(card);
    const gfx::Rect rect = glass_rect();
    background_->SetBoundsRect(rect);
    content_->SetBoundsRect(rect);
  }

  // 内容按最终尺寸布局，动画只改变裁剪区域和不透明度。
  void Morph(const gfx::Rect& glass,
             const gfx::Rect& target,
             float radius,
             float content_opacity) {
    if (!content_->layer()) {
      content_->SetPaintToLayer();
      content_->layer()->SetFillsBoundsOpaquely(false);
    }
    SetCornerRadius(radius, true);
    SetGlassBounds(glass);
    const gfx::Rect rect = glass_rect();
    background_->SetRadius(radius);
    background_->SetBoundsRect(rect);
    gfx::Rect inner(target.size());
    inner.set_origin(rect.origin() + (target.origin() - glass.origin()));
    content_->SetBoundsRect(inner);
    gfx::Rect clip = rect;
    clip.Offset(-inner.OffsetFromOrigin());
    content_->layer()->SetClipRect(clip);
    content_->layer()->SetOpacity(content_opacity);
  }

  void EndMorph(const gfx::Rect& target) {
    SetCornerRadius(radius::kCard, true);
    background_->SetRadius(radius::kCard);
    if (content_->layer()) {
      content_->layer()->SetClipRect(gfx::Rect());
      content_->layer()->SetOpacity(1);
    }
    Place(target);
  }

  // views::View:
  void OnMouseEntered(const ui::MouseEvent& event) override {
    host_->OnCardHover(true);
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    host_->OnCardHover(false);
  }

 private:
  raw_ptr<PopoverHost> host_;
  raw_ptr<PopoverBackground> background_ = nullptr;
  raw_ptr<PopoverContent> content_ = nullptr;
  LayerMotion motion_;
};

BEGIN_METADATA(PopoverCard)
END_METADATA

// views::View 也有 OnEvent，所以窗口的事件由这个小对象转给 OnWindowEvent。
class PopoverHost::WindowWatcher : public ui::EventObserver {
 public:
  explicit WindowWatcher(PopoverHost* host) : host_(host) {}
  void OnEvent(const ui::Event& event) override { host_->OnWindowEvent(event); }

 private:
  const raw_ptr<PopoverHost> host_;
};

PopoverHost::PopoverHost()
    : watcher_(std::make_unique<WindowWatcher>(this)),
      grow_motion_(std::make_unique<FrameMotion>(
          0,
          base::BindRepeating(&PopoverHost::StepGrow,
                              base::Unretained(this)))) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  SetVisible(false);
}

PopoverHost::~PopoverHost() {
  event_monitor_.reset();
}

PopoverContent* PopoverHost::content() {
  return card_ && !closing_ ? card_->content() : nullptr;
}

void PopoverHost::SetTheme(const Theme& theme) {
  theme_ = theme;
  if (card_) {
    card_->SetTheme(theme);
  }
}

void PopoverHost::SetClosedCallback(base::OnceClosure closed) {
  closed_ = std::move(closed);
}

void PopoverHost::Show(std::unique_ptr<PopoverContent> content,
                       const gfx::Rect& anchor,
                       Placement placement,
                       std::optional<Grow> grow) {
  if (card_) {
    // 换内容：旧的先存好再拿掉，不播动画。
    if (!closing_) {
      card_->content()->WillClose();
    }
    RemoveCard();
    if (closed_) {
      std::move(closed_).Run();
    }
  }
  anchor_ = anchor;
  placement_ = placement;
  hovering_ = false;
  SetVisible(true);
  card_ = AddChildView(
      std::make_unique<PopoverCard>(this, std::move(content), theme_));
  ui::Compositor* compositor =
      GetWidget() ? GetWidget()->GetCompositor() : nullptr;
  grow_ = compositor && !motion::ReduceMotion() ? grow : std::nullopt;
  morphing_ = grow_.has_value();
  grow_motion_->SetValue(0);
  PlaceCard();

  if (grow_) {
    // 从那一块长出来：玻璃先叠在那一块上淡入，再长到卡片的位置。
    card_->motion().AnimateOpacityFrom(
        card_->layer(), 0, 1, MotionCurve::EaseOut(motion::kMenuFadeIn));
    grow_motion_->AnimateTo(compositor, 1,
                            MotionCurve::Of(motion::kTileCardOpen));
  } else {
    // 从挂着的那个角由 0.95 放大、淡入（同菜单、下载列表）。
    LayerPose from;
    if (!motion::ReduceMotion()) {
      from.scale = motion::kMenuOpenScale;
    }
    const gfx::Rect glass = card_->glass_rect();
    const gfx::PointF origin = placement == Placement::kInsideTopRight ||
                                       placement == Placement::kBelowRight
                                   ? gfx::PointF(glass.right(), glass.y())
                                   : gfx::PointF(glass.x(), glass.y());
    card_->motion().AnimateTransformFrom(card_->layer(), from, LayerPose(),
                                         MotionCurve::Of(motion::kMenuOpen),
                                         origin);
    card_->motion().AnimateOpacityFrom(
        card_->layer(), 0, 1, MotionCurve::EaseOut(motion::kMenuFadeIn));
  }
  if (GetWidget()) {
    event_monitor_ = views::EventMonitor::CreateWindowMonitor(
        watcher_.get(), GetWidget()->GetNativeWindow(),
        {ui::EventType::kMousePressed, ui::EventType::kKeyPressed});
    if (views::View* focus = card_->content()->InitialFocus()) {
      if (views::FocusManager* manager = GetFocusManager()) {
        previous_focus_.SetView(manager->GetFocusedView());
      }
      focus->RequestFocus();
    }
  }
}

void PopoverHost::Close() {
  if (!card_ || closing_) {
    return;
  }
  card_->content()->WillClose();
  closing_ = true;
  auto_close_timer_.Stop();
  event_monitor_.reset();
  // 焦点还在卡片里时还给原来的地方。
  if (views::FocusManager* manager = GetFocusManager()) {
    views::View* focused = manager->GetFocusedView();
    if (focused && card_->Contains(focused)) {
      if (views::View* previous = previous_focus_.view();
          previous && previous->GetWidget()) {
        previous->RequestFocus();
      } else {
        manager->ClearFocus();
      }
    }
  }
  previous_focus_.SetView(nullptr);
  card_->SetCanProcessEventsWithinSubtree(false);
  ui::Compositor* compositor =
      GetWidget() ? GetWidget()->GetCompositor() : nullptr;
  if (grow_ && compositor && !motion::ReduceMotion()) {
    // 缩回长出来的那一块（不回弹），最后一段淡出。
    morphing_ = true;
    grow_motion_->AnimateTo(compositor, 0,
                            MotionCurve::Of(motion::kTileCardClose));
  } else {
    card_->motion().AnimateOpacity(card_->layer(), 0,
                                   MotionCurve::EaseOut(motion::kMenuFadeOut),
                                   base::BindOnce(
                                       [](base::WeakPtr<PopoverHost> host) {
                                         if (host && host->closing_) {
                                           host->RemoveCard();
                                         }
                                       },
                                       weak_factory_.GetWeakPtr()));
  }
  if (closed_) {
    std::move(closed_).Run();
  }
}

void PopoverHost::RemoveCard() {
  grow_motion_->SetValue(0);
  grow_.reset();
  morphing_ = false;
  if (card_) {
    RemoveChildViewT(card_.ExtractAsDangling());
  }
  card_ = nullptr;
  closing_ = false;
  event_monitor_.reset();
  auto_close_timer_.Stop();
  SetVisible(false);
}

void PopoverHost::ScheduleAutoClose(base::TimeDelta delay) {
  auto_close_delay_ = delay;
  if (hovering_) {
    auto_close_timer_.Stop();
    return;
  }
  auto_close_timer_.Start(FROM_HERE, delay,
                          base::BindOnce(&PopoverHost::Close,
                                         weak_factory_.GetWeakPtr()));
}

void PopoverHost::CancelAutoClose() {
  auto_close_delay_ = base::TimeDelta();
  auto_close_timer_.Stop();
}

void PopoverHost::OnCardHover(bool hovering) {
  hovering_ = hovering;
  if (auto_close_delay_.is_zero() || !is_showing()) {
    return;
  }
  if (hovering) {
    auto_close_timer_.Stop();
  } else {
    // 鼠标离开后延迟 1.5 秒再自动收起。
    auto_close_timer_.Start(FROM_HERE, base::Seconds(1.5),
                            base::BindOnce(&PopoverHost::Close,
                                           weak_factory_.GetWeakPtr()));
  }
}

void PopoverHost::ContentSizeChanged() {
  if (card_ && !closing_) {
    PlaceCard();
  }
}

gfx::Rect PopoverHost::CardBoundsForTesting() const {
  return card_ ? card_->glass_rect() : gfx::Rect();
}

void PopoverHost::Layout(PassKey) {
  if (card_ && !closing_) {
    PlaceCard();
  }
}

void PopoverHost::PlaceCard() {
  gfx::Rect visible = GetLocalBounds();
  visible.Inset(kWindowInset);
  const gfx::Size size = card_->content()->ContentSize();
  const int width = std::min(size.width(), visible.width());
  const int height = std::min(size.height(), visible.height());
  int x = anchor_.x();
  int y = anchor_.y();
  switch (placement_) {
    case Placement::kBelow:
      y = anchor_.bottom() + kAnchorGap;
      break;
    case Placement::kBelowRight:
      x = anchor_.right() - width;
      y = anchor_.bottom() + kAnchorGap;
      break;
    case Placement::kInsideTopRight:
      x = anchor_.right() - kInsideInset - width;
      y = anchor_.y() + kInsideInset;
      break;
    case Placement::kInsideTopLeft:
      x = anchor_.x() + kInsideInset;
      y = anchor_.y() + kInsideInset;
      break;
  }
  x = std::clamp(x, visible.x(), std::max(visible.x(), visible.right() - width));
  y = std::clamp(y, visible.y(),
                 std::max(visible.y(), visible.bottom() - height));
  target_ = gfx::Rect(x, y, width, height);
  // 正在长出来：终点换了，这一帧按新的终点算。
  if (morphing_) {
    StepGrow();
    return;
  }
  card_->Place(target_);
}

void PopoverHost::StepGrow() {
  if (!card_ || !grow_) {
    return;
  }
  const double progress = grow_motion_->value();
  const bool done = !grow_motion_->is_animating();
  if (done && closing_ && progress <= 0) {
    // 缩回去了。不在合成器的这一步里删图层：回到消息循环再删。
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<PopoverHost> host) {
                         if (host && host->closing_) {
                           host->RemoveCard();
                         }
                       },
                       weak_factory_.GetWeakPtr()));
    return;
  }
  if (done && !closing_ && progress >= 1) {
    morphing_ = false;
    card_->EndMorph(target_);
    return;
  }
  const double t = std::clamp(progress, 0.0, 1.0);
  const gfx::Rect glass =
      gfx::Tween::RectValueBetween(progress, grow_->from, target_);
  const float radius =
      static_cast<float>(grow_->radius + (radius::kCard - grow_->radius) * t);
  const double reveal =
      std::clamp((progress - kGrowContentFrom) / kGrowContentSpan, 0.0, 1.0);
  card_->Morph(glass, target_, radius,
               static_cast<float>(gfx::Tween::CalculateValue(
                   gfx::Tween::EASE_IN_OUT, reveal)));
  if (closing_) {
    card_->motion().SetOpacity(
        card_->layer(),
        static_cast<float>(std::clamp(progress / kShrinkFadeSpan, 0.0, 1.0)));
  }
}

bool PopoverHost::DoesIntersectRect(const views::View* target,
                                    const gfx::Rect& rect) const {
  return card_ && !closing_ && card_->glass_bounds().Intersects(rect);
}

void PopoverHost::OnWindowEvent(const ui::Event& event) {
  if (!is_showing()) {
    return;
  }
  // 卡片里打开的菜单（书签的文件夹）开着时，按键、点击都是菜单的。
  if (auto* view = views::AsViewClass<StarryBrowserView>(parent());
      view && view->menu_host() && view->menu_host()->is_showing()) {
    return;
  }
  if (event.IsKeyEvent()) {
    if (event.AsKeyEvent()->key_code() == ui::VKEY_ESCAPE &&
        !card_->content()->HandlesEscape()) {
      Close();
    }
    return;
  }
  if (!event.IsMouseEvent() || !event_monitor_) {
    return;
  }
  gfx::Point point = event_monitor_->GetLastMouseLocation();
  views::View::ConvertPointFromScreen(this, &point);
  if (card_->glass_bounds().Contains(point)) {
    return;
  }
  Close();
}

BEGIN_METADATA(PopoverHost)
END_METADATA

}  // namespace starry
