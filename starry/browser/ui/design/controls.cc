// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/design/controls.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/assistant/agent_parts.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/gfx/vector_icon_types.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace starry {

namespace {

SkColor WithOpacity(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(SkColorGetA(color) * opacity));
}

void PaintIconCentered(gfx::Canvas* canvas,
                       const gfx::VectorIcon& icon,
                       int icon_size,
                       SkColor color,
                       const gfx::Rect& bounds) {
  canvas->Save();
  canvas->Translate(gfx::Vector2d(bounds.x() + (bounds.width() - icon_size) / 2,
                                  bounds.y() + (bounds.height() - icon_size) / 2));
  gfx::PaintVectorIcon(canvas, icon, icon_size, color);
  canvas->Restore();
}

}  // namespace

// IconButton ----------------------------------------------------------------

IconButton::IconButton(PressedCallback callback,
                       const gfx::VectorIcon& icon,
                       const std::u16string& tooltip,
                       int size,
                       int icon_size)
    : views::Button(std::move(callback)),
      icon_(icon),
      size_(size),
      icon_size_(icon_size),
      badge_presence_(0,
                      base::BindRepeating(
                          [](IconButton* button) { button->SchedulePaint(); },
                          base::Unretained(this))) {
  SetTooltipText(tooltip);
  GetViewAccessibility().SetName(tooltip);
  SetAnimationDuration(base::Milliseconds(180));
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
}

IconButton::~IconButton() = default;

void IconButton::SetIcon(const gfx::VectorIcon& icon) {
  if (&*icon_ == &icon) {
    return;
  }
  icon_ = raw_ref(icon);
  SchedulePaint();
}

void IconButton::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void IconButton::SetVariant(ButtonVariant variant) {
  variant_ = variant;
  SchedulePaint();
}

void IconButton::SetActive(bool active) {
  if (active_ == active) {
    return;
  }
  active_ = active;
  SchedulePaint();
}

void IconButton::SetTint(std::optional<SkColor> tint) {
  tint_ = tint;
  SchedulePaint();
}

gfx::Size IconButton::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(size_, size_);
}

SkColor IconButton::FillColor(float hover) const {
  switch (variant_) {
    case ButtonVariant::kFilled:
      return WithOpacity(theme_.primary, 1 - 0.1f * hover);
    case ButtonVariant::kSecondary:
      return WithOpacity(theme_.primary,
                         active_ ? 0.22f : 0.16f + 0.06f * hover);
    case ButtonVariant::kTertiary:
      return WithOpacity(theme_.primary, 0.08f + 0.04f * hover);
    case ButtonVariant::kGhost:
      return theme_.OnSurface(active_ ? 0.06f + 0.02f * hover : 0.08f * hover);
    case ButtonVariant::kDanger:
      return WithOpacity(theme_.danger(), 1 - 0.12f * hover);
  }
}

SkColor IconButton::IconColor() const {
  if (variant_ == ButtonVariant::kFilled) {
    return theme_.on_primary;
  }
  if (variant_ == ButtonVariant::kDanger) {
    return SK_ColorWHITE;
  }
  if (!GetEnabled()) {
    return theme_.OnSurface(0.25f);
  }
  if (tint_) {
    return *tint_;
  }
  return active_ ? theme_.primary : theme_.OnSurface(0.75f);
}

void IconButton::PaintButtonContents(gfx::Canvas* canvas) {
  const float hover = GetEnabled()
                          ? static_cast<float>(hover_animation().GetCurrentValue())
                          : 0;
  const SkColor fill = FillColor(hover);
  if (SkColorGetA(fill) > 0) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(fill);
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kButton,
                          flags);
  }
  PaintIcon(canvas, IconColor());
  // 右上角状态角标：直径 6，圆心距右边、上边各 8。
  const float presence =
      std::clamp(static_cast<float>(badge_presence_.value()), 0.0f, 1.2f);
  if (badge_shown_ != IconBadge::kNone && presence > 0.01f) {
    const gfx::PointF center(width() - 8, 8);
    const float radius = 3 * presence;
    cc::PaintFlags flags;
    if (badge_shown_ == IconBadge::kAttention) {
      flags.setAntiAlias(true);
      flags.setColor(theme_.warning());
    } else if (badge_shown_ == IconBadge::kAccent) {
      flags.setAntiAlias(true);
      flags.setColor(theme_.accent);
    } else {
      flags = AgentGradient(gfx::RectF(center.x() - 3, center.y() - 3, 6, 6));
    }
    flags.setAlphaf(std::min(1.0f, presence));
    canvas->DrawCircle(center, radius, flags);
  }
}

void IconButton::SetBadge(IconBadge badge) {
  if (badge_ == badge) {
    return;
  }
  badge_ = badge;
  if (badge != IconBadge::kNone) {
    badge_shown_ = badge;
  }
  const double target = badge == IconBadge::kNone ? 0 : 1;
  if (motion::ReduceMotion() || !GetWidget()) {
    badge_presence_.SetValue(target);
    SchedulePaint();
  } else {
    badge_presence_.AnimateTo(target, MotionCurve::Of(motion::kAssistantBadge));
  }
}

void IconButton::PaintIcon(gfx::Canvas* canvas, SkColor color) {
  PaintIconCentered(canvas, *icon_, icon_size_, color, GetLocalBounds());
}

void IconButton::StateChanged(ButtonState old_state) {
  views::Button::StateChanged(old_state);
  const bool pressed = GetState() == STATE_PRESSED;
  if (pressed == pressed_scale_) {
    return;
  }
  pressed_scale_ = pressed;
  LayerPose pose;
  pose.scale = pressed ? motion::kPressScale : 1;
  press_motion_.AnimateTransform(
      layer(), pose, motion::Press(),
      gfx::PointF(width() / 2.0f, height() / 2.0f));
}

void IconButton::OnEnabledChanged() {
  views::Button::OnEnabledChanged();
  SchedulePaint();
}

BEGIN_METADATA(IconButton)
END_METADATA

// PillButton ----------------------------------------------------------------

namespace {
constexpr int kPillHeight = 32;
constexpr int kPillPadding = 16;
constexpr int kPillIconSize = 12;
constexpr int kPillIconSpacing = 8;
// 键盘选中的外圈：离按钮 3，宽 2。
constexpr int kFocusOutset = 3;
}  // namespace

PillButton::PillButton(PressedCallback callback,
                       const std::u16string& title,
                       ButtonVariant variant)
    : views::Button(std::move(callback)),
      title_(title),
      variant_(variant),
      height_(kPillHeight) {
  SetAnimationDuration(base::Milliseconds(180));
  GetViewAccessibility().SetName(title);
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  // 外圈画在按钮外面：边距里留出它的位置。
  SetBorder(nullptr);
}

PillButton::~PillButton() = default;

void PillButton::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void PillButton::SetTitle(const std::u16string& title) {
  if (title_ == title) {
    return;
  }
  title_ = title;
  GetViewAccessibility().SetName(title);
  PreferredSizeChanged();
  SchedulePaint();
}

void PillButton::SetVariant(ButtonVariant variant) {
  variant_ = variant;
  SchedulePaint();
}

void PillButton::SetIcon(const gfx::VectorIcon* icon) {
  icon_ = icon;
  PreferredSizeChanged();
  SchedulePaint();
}

void PillButton::SetHeight(int height) {
  height_ = height;
  PreferredSizeChanged();
  SchedulePaint();
}

// static
int PillButton::FocusOutset() {
  return kFocusOutset;
}

void PillButton::SetKeyboardSelected(bool selected) {
  if (keyboard_selected_ == selected) {
    return;
  }
  keyboard_selected_ = selected;
  SchedulePaint();
}

gfx::Size PillButton::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  const int text = gfx::GetStringWidth(
      title_, UiFont(13, gfx::Font::Weight::MEDIUM));
  const int icon = icon_ ? kPillIconSize + kPillIconSpacing : 0;
  return gfx::Size(text + icon + 2 * kPillPadding + 2 * kFocusOutset,
                   height_ + 2 * kFocusOutset);
}

void PillButton::PaintButtonContents(gfx::Canvas* canvas) {
  const float hover = static_cast<float>(hover_animation().GetCurrentValue());
  gfx::RectF pill(GetLocalBounds());
  pill.Inset(kFocusOutset);
  const float radius = pill.height() / 2;
  SkColor fill = SK_ColorTRANSPARENT;
  SkColor text = theme_.primary;
  switch (variant_) {
    case ButtonVariant::kFilled:
      fill = WithOpacity(theme_.primary, 1 - 0.1f * hover);
      text = theme_.on_primary;
      break;
    case ButtonVariant::kSecondary:
      fill = WithOpacity(theme_.primary, 0.16f + 0.06f * hover);
      break;
    case ButtonVariant::kTertiary:
      fill = WithOpacity(theme_.primary, 0.08f + 0.04f * hover);
      break;
    case ButtonVariant::kGhost:
      fill = theme_.OnSurface(0.08f * hover);
      text = theme_.on_surface;
      break;
    case ButtonVariant::kDanger:
      fill = WithOpacity(theme_.danger(), 1 - 0.12f * hover);
      text = SK_ColorWHITE;
      break;
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(fill);
  canvas->DrawRoundRect(pill, radius, flags);
  if (keyboard_selected_) {
    cc::PaintFlags ring;
    ring.setAntiAlias(true);
    ring.setStyle(cc::PaintFlags::kStroke_Style);
    ring.setStrokeWidth(2);
    ring.setColor(WithOpacity(theme_.accent, 0.75f));
    gfx::RectF outer = pill;
    outer.Outset(kFocusOutset - 1);
    canvas->DrawRoundRect(outer, outer.height() / 2, ring);
  }
  const gfx::FontList& font = UiFont(13, gfx::Font::Weight::MEDIUM);
  if (!icon_) {
    canvas->DrawStringRectWithFlags(
        title_, font, text, gfx::ToEnclosingRect(pill),
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    return;
  }
  // 图标和文字作为一个整体水平居中。
  const gfx::Rect bounds = gfx::ToEnclosingRect(pill);
  const int text_width = gfx::GetStringWidth(title_, font);
  const int content = kPillIconSize + kPillIconSpacing + text_width;
  const int left = bounds.x() + std::max(0, (bounds.width() - content) / 2);
  PaintIconCentered(
      canvas, *icon_, kPillIconSize, text,
      gfx::Rect(left, bounds.y(), kPillIconSize, bounds.height()));
  const int text_left = left + kPillIconSize + kPillIconSpacing;
  canvas->DrawStringRectWithFlags(
      title_, font, text,
      gfx::Rect(text_left, bounds.y(), std::max(0, bounds.right() - text_left),
                bounds.height()),
      gfx::Canvas::TEXT_ALIGN_LEFT | gfx::Canvas::NO_SUBPIXEL_RENDERING);
}

void PillButton::StateChanged(ButtonState old_state) {
  views::Button::StateChanged(old_state);
  const bool pressed = GetState() == STATE_PRESSED;
  if (pressed == pressed_scale_) {
    return;
  }
  pressed_scale_ = pressed;
  LayerPose pose;
  pose.scale = pressed ? motion::kPressScale : 1;
  press_motion_.AnimateTransform(
      layer(), pose, motion::Press(),
      gfx::PointF(width() / 2.0f, height() / 2.0f));
}

BEGIN_METADATA(PillButton)
END_METADATA

// GlyphButton ---------------------------------------------------------------

GlyphButton::GlyphButton(PressedCallback callback,
                         const gfx::VectorIcon& icon,
                         int glyph_size,
                         int button_size)
    : views::Button(std::move(callback)),
      icon_(icon),
      glyph_size_(glyph_size),
      button_size_(button_size) {}

GlyphButton::~GlyphButton() = default;

void GlyphButton::SetIcon(const gfx::VectorIcon& icon) {
  if (&*icon_ == &icon) {
    return;
  }
  icon_ = raw_ref(icon);
  SchedulePaint();
}

void GlyphButton::SetColors(SkColor tint, SkColor fill) {
  tint_ = tint;
  fill_ = fill;
  SchedulePaint();
}

void GlyphButton::SetShowsFill(bool shows_fill) {
  if (shows_fill_ == shows_fill) {
    return;
  }
  shows_fill_ = shows_fill;
  SchedulePaint();
}

void GlyphButton::SetFillRadius(float radius) {
  fill_radius_ = radius;
  SchedulePaint();
}

void GlyphButton::SetAlpha(float alpha) {
  if (alpha_ == alpha) {
    return;
  }
  alpha_ = alpha;
  SchedulePaint();
}

gfx::Size GlyphButton::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(button_size_, button_size_);
}

void GlyphButton::PaintButtonContents(gfx::Canvas* canvas) {
  if (alpha_ <= 0) {
    return;
  }
  if (shows_fill_) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(WithOpacity(fill_, alpha_));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), fill_radius_, flags);
  }
  PaintIconCentered(canvas, *icon_, glyph_size_, WithOpacity(tint_, alpha_),
                    GetLocalBounds());
}

BEGIN_METADATA(GlyphButton)
END_METADATA

SwitchButton::SwitchButton(base::RepeatingCallback<void(bool on)> changed)
    : views::Button(
          base::BindRepeating(&SwitchButton::Toggle, base::Unretained(this))),
      changed_(std::move(changed)),
      knob_(0,
            base::BindRepeating(&views::View::SchedulePaint,
                                base::Unretained(this))) {
  GetViewAccessibility().SetRole(ax::mojom::Role::kSwitch);
  GetViewAccessibility().SetCheckedState(ax::mojom::CheckedState::kFalse);
}

SwitchButton::~SwitchButton() = default;

void SwitchButton::SetOn(bool on, bool animate) {
  if (on_ == on && !knob_.is_animating()) {
    knob_.SetValue(on ? 1 : 0);
    return;
  }
  on_ = on;
  GetViewAccessibility().SetCheckedState(on ? ax::mojom::CheckedState::kTrue
                                            : ax::mojom::CheckedState::kFalse);
  if (animate && !motion::ReduceMotion() && GetWidget()) {
    knob_.AnimateTo(on ? 1 : 0, motion::Hover());
  } else {
    knob_.SetValue(on ? 1 : 0);
  }
  SchedulePaint();
}

void SwitchButton::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void SwitchButton::Toggle() {
  SetOn(!on_, /*animate=*/true);
  if (changed_) {
    changed_.Run(on_);
  }
}

gfx::Size SwitchButton::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(34, 20);
}

void SwitchButton::PaintButtonContents(gfx::Canvas* canvas) {
  // 同设置页的 .switch：关着是浅灰的轨道、白色圆钮；开着是强调色的轨道，圆钮换成
  // surface_bright（强调色很浅时也看得出来）。
  const float t = static_cast<float>(std::clamp(knob_.value(), 0.0, 1.0));
  const gfx::RectF track(GetLocalBounds());
  const float radius = track.height() / 2;
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(color_utils::AlphaBlend(theme_.accent, theme_.OnSurface(0.18f),
                                         static_cast<SkAlpha>(255 * t)));
  canvas->DrawRoundRect(track, radius, flags);
  constexpr float kInset = 2;
  const float knob = track.height() - 2 * kInset;
  const float x = kInset + (track.width() - knob - 2 * kInset) * t;
  const gfx::PointF center(x + knob / 2, track.height() / 2);
  flags.setColor(SkColorSetA(SK_ColorBLACK, 40));
  canvas->DrawCircle(center + gfx::Vector2dF(0, 0.6f), knob / 2 + 0.3f, flags);
  flags.setColor(color_utils::AlphaBlend(theme_.surface_bright, SK_ColorWHITE,
                                         static_cast<SkAlpha>(255 * t)));
  canvas->DrawCircle(center, knob / 2, flags);
}

BEGIN_METADATA(SwitchButton)
END_METADATA

}  // namespace starry
