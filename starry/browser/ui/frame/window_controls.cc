// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/frame/window_controls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/hit_test.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/display/screen.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view_targeter.h"
#include "ui/views/window/hit_test_utils.h"

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/devtools.h"
#endif

namespace starry {

namespace {

constexpr float kGlassRestingOpacity = 0.82f;
constexpr float kInactiveOpacity = 0.38f;
constexpr float kPressScale = 0.92f;
constexpr float kGlyphStrokeWidth = 1.4f;

// 悬浮玻璃胶囊模式隐藏态：边框微提示尺寸、移出延迟与光标轮询间隔（同 SidebarView）。
constexpr int kHintBarWidth = 38;
constexpr float kHintBarHeight = 4.5f;
constexpr int kHintSensorHeight = 12;
constexpr double kInteractiveReveal = 0.35;
constexpr int kRevealGraceMargin = 18;
constexpr base::TimeDelta kRetractDelay = base::Milliseconds(260);
constexpr base::TimeDelta kPointerPollInterval = base::Milliseconds(40);

// 悬浮玻璃胶囊几何尺寸。
constexpr int kGlassPad = 3;
constexpr int kGlassButtonW = 30;
constexpr int kGlassButtonH = 24;
constexpr int kGlassGap = 2;
constexpr float kGlassPillRadius = 12.f;

// 纤细顶檐模式。
constexpr int kRailButtonW = 34;
constexpr int kRailButtonH = 24;
constexpr int kRailGap = 4;

// 顶部标签行 / 工具栏 / 小窗口。
constexpr int kInlineButtonW = 36;
constexpr int kInlineButtonH = 28;
constexpr int kInlineGap = 4;

// 紧凑地址栏。
constexpr int kCompactButtonW = 30;
constexpr int kCompactButtonH = 24;
constexpr int kCompactGap = 3;

struct Metrics {
  int pad_x;
  int pad_y;
  int button_w;
  int button_h;
  int gap;
  float radius;
  int bar_lead_gap;
};

Metrics MetricsFor(WindowControlsView::Presentation p) {
  switch (p) {
    case WindowControlsView::Presentation::kFloatingGlass:
      return {kGlassPad, kGlassPad, kGlassButtonW, kGlassButtonH, kGlassGap,
              kGlassPillRadius, 0};
    case WindowControlsView::Presentation::kSlimRail:
      return {0, 0, kRailButtonW, kRailButtonH, kRailGap,
              static_cast<float>(radius::kButton), 8};
    case WindowControlsView::Presentation::kInlineBar:
      return {0, 0, kInlineButtonW, kInlineButtonH, kInlineGap,
              static_cast<float>(radius::kButton), 8};
    case WindowControlsView::Presentation::kCompactBar:
      return {0, 0, kCompactButtonW, kCompactButtonH, kCompactGap,
              static_cast<float>(radius::kButton), 10};
  }
}

SkColor WithAlpha(SkColor color, float factor) {
  return SkColorSetA(
      color,
      static_cast<U8CPU>(std::lround(SkColorGetA(color) *
                                     std::clamp(factor, 0.f, 1.f))));
}

SkColor Darken(SkColor color, float amount) {
  const float scale = std::clamp(1.f - amount, 0.f, 1.f);
  return SkColorSetARGB(
      SkColorGetA(color),
      static_cast<U8CPU>(std::lround(SkColorGetR(color) * scale)),
      static_cast<U8CPU>(std::lround(SkColorGetG(color) * scale)),
      static_cast<U8CPU>(std::lround(SkColorGetB(color) * scale)));
}

}  // namespace

bool WindowsControlsEnabled() {
#if BUILDFLAG(IS_WIN)
  return true;
#elif defined(STARRY_DEVTOOLS)
  return devtools::HasForcedWindowControls();
#else
  return false;
#endif
}

WindowsControlsStyle WindowsControlsStyleFromPrefs(const PrefService* prefs) {
  std::string value;
#if defined(STARRY_DEVTOOLS)
  value = devtools::ForcedWindowControlsStyle();
#endif
  if (value.empty() && prefs) {
    value = prefs->GetString(prefs::kWindowsControlsStyle);
  }
  if (value == prefs::kWindowsControlsStyleRail) {
    return WindowsControlsStyle::kRail;
  }
  return WindowsControlsStyle::kGlass;
}

bool WindowsControlsAutoHideFromPrefs(const PrefService* prefs) {
#if defined(STARRY_DEVTOOLS)
  if (devtools::ForceWindowControlsAutoHide()) {
    return true;
  }
#endif
  return prefs && prefs->GetBoolean(prefs::kWindowsControlsAutoHide);
}

// WindowRailView --------------------------------------------------------------

WindowRailView::WindowRailView() {
  SetWindowDraggable(this, true);
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
}

WindowRailView::~WindowRailView() = default;

void WindowRailView::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  SchedulePaint();
}

void WindowRailView::SetTitle(const std::u16string& title) {
  if (title_ == title) {
    return;
  }
  title_ = title;
  SchedulePaint();
}

void WindowRailView::SetWindowActive(bool active) {
  if (active_ == active) {
    return;
  }
  active_ = active;
  SchedulePaint();
}

void WindowRailView::OnPaint(gfx::Canvas* canvas) {
  if (title_.empty() || width() < 140) {
    return;
  }
  constexpr int kHorizontalPadding = 16;
  const gfx::Rect text_bounds(kHorizontalPadding, 0,
                              std::max(0, width() - 2 * kHorizontalPadding),
                              height());
  canvas->DrawStringRectWithFlags(
      title_, UiFont(12, gfx::Font::Weight::MEDIUM),
      theme_.OnSurface(active_ ? 0.62f : 0.34f), text_bounds,
      gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
}

BEGIN_METADATA(WindowRailView)
END_METADATA

// WindowControlsView::CaptionButton -------------------------------------------

class WindowControlsView::CaptionButton : public views::Button {
  METADATA_HEADER(CaptionButton, views::Button)

 public:
  enum class Kind { kMinimize, kMaximize, kClose };

  CaptionButton(Kind kind,
                int index,
                PressedCallback callback,
                base::RepeatingCallback<void(int, bool)> hover_callback)
      : views::Button(std::move(callback)),
        kind_(kind),
        index_(index),
        hover_callback_(std::move(hover_callback)),
        hover_(0,
               base::BindRepeating(&CaptionButton::SchedulePaint,
                                   base::Unretained(this))),
        press_(0,
               base::BindRepeating(&CaptionButton::SchedulePaint,
                                   base::Unretained(this))),
        glyph_(0,
               base::BindRepeating(&CaptionButton::SchedulePaint,
                                   base::Unretained(this))) {
    SetFocusBehavior(FocusBehavior::ACCESSIBLE_ONLY);
    SetAnimateOnStateChange(false);
    UpdateAccessibleLabel();
  }

  CaptionButton(const CaptionButton&) = delete;
  CaptionButton& operator=(const CaptionButton&) = delete;
  ~CaptionButton() override = default;

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void SetMaximized(bool maximized) {
    if (maximized_ == maximized) {
      return;
    }
    maximized_ = maximized;
    if (kind_ == Kind::kMaximize) {
      UpdateAccessibleLabel();
      SchedulePaint();
    }
  }

  void SetVisualRect(const gfx::Rect& rect, float corner_radius) {
    visual_rect_ = rect;
    corner_radius_ = corner_radius;
    SchedulePaint();
  }

  void SetGlassBlend(float glass_blend) {
    glass_blend_ = std::clamp(glass_blend, 0.f, 1.f);
    SchedulePaint();
  }

  void SetWindowActiveBlend(float active_blend) {
    active_blend_ = std::clamp(active_blend, 0.f, 1.f);
    SchedulePaint();
  }

  float hover_value() const { return static_cast<float>(hover_.value()); }
  float press_value() const { return static_cast<float>(press_.value()); }

  // views::Button:
  void StateChanged(ButtonState old_state) override {
    const ButtonState state = GetState();
    const bool hovered = state == STATE_HOVERED || state == STATE_PRESSED;
    const bool pressed = state == STATE_PRESSED;
    if (hovered != is_hovered_) {
      is_hovered_ = hovered;
      if (hover_callback_) {
        hover_callback_.Run(index_, hovered);
      }
      if (motion::ReduceMotion()) {
        hover_.SetValue(hovered ? 1 : 0);
        glyph_.SetValue(hovered ? 1 : 0);
      } else {
        hover_.AnimateTo(hovered ? 1 : 0, motion::Hover());
        glyph_.AnimateTo(hovered ? 1 : 0, MotionCurve::Of(motion::kLift));
      }
    }
    if (pressed != is_pressed_) {
      is_pressed_ = pressed;
      if (motion::ReduceMotion()) {
        press_.SetValue(pressed ? 1 : 0);
      } else {
        press_.AnimateTo(pressed ? 1 : 0, motion::Press());
      }
    }
    SchedulePaint();
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const gfx::RectF rect(visual_rect_.IsEmpty() ? GetLocalBounds()
                                                 : visual_rect_);
    if (rect.IsEmpty()) {
      return;
    }
    const float h = static_cast<float>(std::clamp(hover_.value(), 0.0, 1.0));
    const float p = static_cast<float>(std::clamp(press_.value(), 0.0, 1.0));
    const float g = static_cast<float>(std::clamp(glyph_.value(), -0.2, 1.3));
    const bool reduce_motion = motion::ReduceMotion();

    gfx::ScopedCanvas scoped(canvas);
    const gfx::PointF center = rect.CenterPoint();
    if (!reduce_motion && p > 0.001f) {
      const float scale = 1.f - (1.f - kPressScale) * p;
      canvas->sk_canvas()->translate(center.x(), center.y());
      canvas->sk_canvas()->scale(scale, scale);
      canvas->sk_canvas()->translate(-center.x(), -center.y());
    }

    // 非悬浮玻璃模式（纤细顶檐 / 横栏 / 紧凑栏）：按钮自绘 6px 软角底色；
    // 悬浮玻璃胶囊模式由 ControlsSurface 绘制磁吸滑动高亮胶囊。
    const float inline_weight = 1.f - glass_blend_;
    if (inline_weight > 0.001f && (h > 0.001f || p > 0.001f)) {
      cc::PaintFlags bg;
      bg.setAntiAlias(true);
      if (kind_ == Kind::kClose) {
        const SkColor base_red =
            p > 0.001f ? Darken(theme_.danger(), 0.14f * p) : theme_.danger();
        bg.setColor(WithAlpha(base_red, std::max(h, p) * inline_weight));
      } else {
        const float alpha = (0.08f * h + 0.06f * p) * inline_weight;
        bg.setColor(theme_.OnSurface(alpha));
      }
      canvas->DrawRoundRect(rect, corner_radius_, bg);
    }

    // 图标前景色：失焦柔和降淡，悬停立刻唤醒；关闭悬停平滑反白。
    const float wake = std::max(active_blend_, h);
    const float base_alpha = (0.82f + 0.18f * h) * wake;
    SkColor fg = theme_.OnSurface(base_alpha);
    if (kind_ == Kind::kClose && (h > 0.001f || p > 0.001f)) {
      fg = Theme::LerpColor(fg, SK_ColorWHITE, std::max(h, p));
    }

    cc::PaintFlags stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(cc::PaintFlags::kStroke_Style);
    stroke.setStrokeWidth(kGlyphStrokeWidth);
    stroke.setStrokeCap(cc::PaintFlags::kRound_Cap);
    stroke.setStrokeJoin(cc::PaintFlags::kRound_Join);
    stroke.setColor(fg);

    canvas->sk_canvas()->translate(center.x(), center.y());
    switch (kind_) {
      case Kind::kMinimize: {
        const float dy = reduce_motion ? 0.f : 1.5f * g;
        canvas->DrawLine(gfx::PointF(-4.8f, 0.5f + dy),
                         gfx::PointF(4.8f, 0.5f + dy), stroke);
        break;
      }
      case Kind::kMaximize: {
        if (!maximized_) {
          const float scale = reduce_motion ? 1.f : (1.f + 0.09f * g);
          canvas->Scale(scale, scale);
          const gfx::RectF box(-4.4f, -4.4f, 8.8f, 8.8f);
          canvas->DrawRoundRect(box, 2.0f, stroke);
        } else {
          // 还原双圆角框：悬停时前后框轻微舒展。
          const float spread = reduce_motion ? 0.f : 0.45f * g;
          const gfx::RectF front(-4.8f - spread * 0.5f, -2.6f + spread * 0.5f,
                                 7.4f, 7.4f);
          canvas->DrawRoundRect(front, 1.6f, stroke);
          const float bx = -2.4f + spread;
          const float by = -4.8f - spread;
          const float bw = 7.2f;
          const float bh = 7.2f;
          const float r = 1.6f;
          SkPathBuilder back;
          back.moveTo(bx, by + 2.2f);
          back.lineTo(bx, by + r);
          back.quadTo(bx, by, bx + r, by);
          back.lineTo(bx + bw - r, by);
          back.quadTo(bx + bw, by, bx + bw, by + r);
          back.lineTo(bx + bw, by + bh - r);
          back.quadTo(bx + bw, by + bh, bx + bw - r, by + bh);
          back.lineTo(bx + bw - 2.2f, by + bh);
          canvas->DrawPath(back.detach(), stroke);
        }
        break;
      }
      case Kind::kClose: {
        const float scale = reduce_motion ? 1.f : (1.f + 0.08f * g);
        canvas->Scale(scale, scale);
        // 悬浮玻璃胶囊态：× 悬停轻旋 90°。
        if (!reduce_motion && glass_blend_ > 0.2f && h > 0.001f) {
          canvas->sk_canvas()->rotate(90.f * h * glass_blend_);
        }
        constexpr float kHalf = 4.0f;
        canvas->DrawLine(gfx::PointF(-kHalf, -kHalf), gfx::PointF(kHalf, kHalf),
                         stroke);
        canvas->DrawLine(gfx::PointF(kHalf, -kHalf), gfx::PointF(-kHalf, kHalf),
                         stroke);
        break;
      }
    }
  }

 private:
  void UpdateAccessibleLabel() {
    std::u16string text;
    switch (kind_) {
      case Kind::kMinimize:
        text = L("window.minimize");
        break;
      case Kind::kMaximize:
        text = L(maximized_ ? "window.restore" : "window.maximize");
        break;
      case Kind::kClose:
        text = L("common.close");
        break;
    }
    SetTooltipText(text);
    GetViewAccessibility().SetName(text);
  }

  Kind kind_;
  int index_;
  base::RepeatingCallback<void(int, bool)> hover_callback_;
  Theme theme_ = Theme::DarkBase();
  bool maximized_ = false;
  bool is_hovered_ = false;
  bool is_pressed_ = false;
  gfx::Rect visual_rect_;
  float corner_radius_ = static_cast<float>(radius::kButton);
  float glass_blend_ = 1.f;
  float active_blend_ = 1.f;
  ValueMotion hover_;
  ValueMotion press_;
  ValueMotion glyph_;
};

BEGIN_METADATA(WindowControlsView, CaptionButton)
END_METADATA

// WindowControlsView::ControlsSurface -----------------------------------------

class WindowControlsView::ControlsSurface : public views::View {
  METADATA_HEADER(ControlsSurface, views::View)

 public:
  explicit ControlsSurface(WindowControlsView* owner) : owner_(owner) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }
  ControlsSurface(const ControlsSurface&) = delete;
  ControlsSurface& operator=(const ControlsSurface&) = delete;
  ~ControlsSurface() override = default;

  void OnPaint(gfx::Canvas* canvas) override {
    const float glass =
        static_cast<float>(std::clamp(owner_->glass_presence_.value(), 0.0, 1.0));
    const float pill_alpha =
        static_cast<float>(std::clamp(owner_->pill_opacity_.value(), 0.0, 1.0));
    if (glass <= 0.001f || pill_alpha <= 0.001f) {
      return;
    }
    // 悬浮玻璃胶囊模式：磁吸滑动高亮胶囊（在三枚按钮之间用 kMenuHighlight 弹簧滑动，
    // 滑到关闭位时底色平滑过渡为 danger 红）。
    const gfx::Rect visual = owner_->VisualRectInLocal();
    const float x = static_cast<float>(owner_->pill_x_.value());
    const float y = static_cast<float>(visual.y() + kGlassPad);
    gfx::RectF pill(x, y, static_cast<float>(kGlassButtonW),
                    static_cast<float>(kGlassButtonH));

    // 当前按下的按钮如果是关闭或普通键，高亮底同步微缩。
    float press = 0.f;
    if (owner_->hovered_index_ == 0) {
      press = owner_->minimize_->press_value();
    } else if (owner_->hovered_index_ == 1) {
      press = owner_->maximize_->press_value();
    } else if (owner_->hovered_index_ == 2) {
      press = owner_->close_->press_value();
    }
    const bool reduce_motion = motion::ReduceMotion();
    gfx::ScopedCanvas scoped(canvas);
    if (!reduce_motion && press > 0.001f) {
      const gfx::PointF center = pill.CenterPoint();
      const float scale = 1.f - (1.f - kPressScale) * press;
      canvas->sk_canvas()->translate(center.x(), center.y());
      canvas->sk_canvas()->scale(scale, scale);
      canvas->sk_canvas()->translate(-center.x(), -center.y());
    }

    const float danger_t =
        static_cast<float>(std::clamp(owner_->pill_danger_.value(), 0.0, 1.0));
    const SkColor normal_fill =
        owner_->theme_.OnSurface(owner_->theme_.is_dark ? 0.14f : 0.09f);
    const SkColor danger_fill =
        press > 0.001f ? Darken(owner_->theme_.danger(), 0.14f * press)
                       : owner_->theme_.danger();
    const SkColor color =
        WithAlpha(Theme::LerpColor(normal_fill, danger_fill, danger_t),
                  pill_alpha * glass);

    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(color);
    canvas->DrawRoundRect(pill, kGlassPillRadius, flags);
  }

 private:
  raw_ptr<WindowControlsView> owner_;
};

BEGIN_METADATA(WindowControlsView, ControlsSurface)
END_METADATA

// WindowControlsView::BorderHintView ------------------------------------------

// 悬浮玻璃胶囊模式隐藏态时留在窗口右上角边框的极简视觉提示：一条 38×4.5 的微光胶囊条 +
// 三枚极小圆点，鼠标移入右上角边缘立即触发 Liquid Glass 胶囊弹出。
class WindowControlsView::BorderHintView : public views::View {
  METADATA_HEADER(BorderHintView, views::View)

 public:
  explicit BorderHintView(WindowControlsView* owner) : owner_(owner) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }
  BorderHintView(const BorderHintView&) = delete;
  BorderHintView& operator=(const BorderHintView&) = delete;
  ~BorderHintView() override = default;

  void OnPaint(gfx::Canvas* canvas) override {
    if (width() <= 0 || height() <= 0) {
      return;
    }
    const float active = static_cast<float>(
        std::clamp(owner_->active_presence_.value(), 0.0, 1.0));
    const float x = (width() - kHintBarWidth) * 0.5f;
    const float y = std::max(1.5f, (height() - kHintBarHeight) * 0.5f);
    const gfx::RectF bar(x, y, static_cast<float>(kHintBarWidth),
                         kHintBarHeight);

    cc::PaintFlags pill;
    pill.setAntiAlias(true);
    pill.setColor(owner_->theme_.OnSurface(0.18f * (0.45f + 0.55f * active)));
    canvas->DrawRoundRect(bar, kHintBarHeight * 0.5f, pill);

    // 三枚极克制微点，提示最小化 / 最大化 / 关闭。
    cc::PaintFlags dot;
    dot.setAntiAlias(true);
    const float cy = bar.CenterPoint().y();
    const float cx = bar.CenterPoint().x();
    constexpr float kDotRadius = 1.15f;
    constexpr float kDotSpacing = 8.0f;
    for (int i = -1; i <= 1; ++i) {
      const float alpha = (i == 1 ? 0.56f : 0.44f) * (0.5f + 0.5f * active);
      dot.setColor(i == 1 ? WithAlpha(owner_->theme_.danger(), alpha)
                          : owner_->theme_.OnSurface(alpha));
      canvas->DrawCircle(gfx::PointF(cx + i * kDotSpacing, cy), kDotRadius,
                         dot);
    }
  }

 private:
  raw_ptr<WindowControlsView> owner_;
};

BEGIN_METADATA(WindowControlsView, BorderHintView)
END_METADATA

// WindowControlsView ----------------------------------------------------------

WindowControlsView::WindowControlsView()
    : glass_presence_(1,
                      base::BindRepeating(
                          &WindowControlsView::UpdateGlassAppearance,
                          base::Unretained(this))),
      reveal_presence_(1,
                       base::BindRepeating(
                           &WindowControlsView::UpdateRevealAppearance,
                           base::Unretained(this))),
      capsule_opacity_(kGlassRestingOpacity,
                       base::BindRepeating(
                           &WindowControlsView::ApplyVisualOpacity,
                           base::Unretained(this))),
      active_presence_(1,
                       base::BindRepeating(
                           &WindowControlsView::ApplyVisualOpacity,
                           base::Unretained(this))),
      pill_x_(0,
              base::BindRepeating(
                  [](WindowControlsView* view) { view->surface_->SchedulePaint(); },
                  base::Unretained(this))),
      pill_opacity_(0,
                    base::BindRepeating(
                        [](WindowControlsView* view) {
                          view->surface_->SchedulePaint();
                        },
                        base::Unretained(this))),
      pill_danger_(0,
                   base::BindRepeating(
                       [](WindowControlsView* view) {
                         view->surface_->SchedulePaint();
                       },
                       base::Unretained(this))) {
  // 自身图层不绘制、不裁剪：GlassView 的外部阴影与折射边距可以自然延展。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  layer()->SetMasksToBounds(false);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));

  hint_ = AddChildView(std::make_unique<BorderHintView>(this));
  hint_->SetVisible(false);

  glass_ = AddChildView(std::make_unique<GlassView>());
  glass_->SetCornerRadius(-1, /*continuous=*/false);
  glass_->SetCanProcessEventsWithinSubtree(false);

  surface_ = AddChildView(std::make_unique<ControlsSurface>(this));

  const auto on_hover = base::BindRepeating(
      &WindowControlsView::OnButtonHoverChanged, base::Unretained(this));
  minimize_ = surface_->AddChildView(std::make_unique<CaptionButton>(
      CaptionButton::Kind::kMinimize, 0,
      base::BindRepeating(&WindowControlsView::OnButtonPressed,
                          base::Unretained(this), 0),
      on_hover));
  maximize_ = surface_->AddChildView(std::make_unique<CaptionButton>(
      CaptionButton::Kind::kMaximize, 1,
      base::BindRepeating(&WindowControlsView::OnButtonPressed,
                          base::Unretained(this), 1),
      on_hover));
  close_ = surface_->AddChildView(std::make_unique<CaptionButton>(
      CaptionButton::Kind::kClose, 2,
      base::BindRepeating(&WindowControlsView::OnButtonPressed,
                          base::Unretained(this), 2),
      on_hover));
  views::SetHitTestComponent(minimize_, HTMINBUTTON);
  views::SetHitTestComponent(maximize_, HTMAXBUTTON);
  views::SetHitTestComponent(close_, HTCLOSE);

  UpdateGlassAppearance();
  UpdateRevealAppearance();
}

WindowControlsView::~WindowControlsView() = default;

void WindowControlsView::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  minimize_->SetTheme(theme_);
  maximize_->SetTheme(theme_);
  close_->SetTheme(theme_);
  if (!glass_dark_overridden_) {
    glass_dark_ = theme_.is_dark;
  }
  hint_->SchedulePaint();
  UpdateGlassAppearance();
  surface_->SchedulePaint();
}

void WindowControlsView::SetGlassDark(bool dark) {
  glass_dark_overridden_ = true;
  if (glass_dark_ == dark) {
    return;
  }
  glass_dark_ = dark;
  UpdateGlassAppearance();
}

void WindowControlsView::SetPresentation(Presentation presentation,
                                         bool animated) {
  if (presentation_ == presentation) {
    return;
  }
  presentation_ = presentation;
  const double target_glass =
      presentation_ == Presentation::kFloatingGlass ? 1.0 : 0.0;
  if (animated && GetWidget() && GetVisible() && !motion::ReduceMotion()) {
    glass_presence_.AnimateTo(target_glass, motion::ChromeLayout());
  } else {
    glass_presence_.SetValue(target_glass);
  }
  if (!IsAutoHideActive()) {
    revealed_ = false;
    retract_timer_.Stop();
    pointer_poll_.Stop();
    reveal_presence_.SetValue(1.0);
  } else if (!revealed_) {
    reveal_presence_.SetValue(0.0);
  }
  UpdateGlassAppearance();
  UpdateRevealAppearance();
  InvalidateLayout();
}

void WindowControlsView::SetAutoHide(bool auto_hide) {
  if (auto_hide_ == auto_hide) {
    return;
  }
  auto_hide_ = auto_hide;
  retract_timer_.Stop();
  pointer_poll_.Stop();
  revealed_ = false;
  const double target = IsAutoHideActive() ? 0.0 : 1.0;
  if (GetWidget() && GetVisible() && !motion::ReduceMotion()) {
    reveal_presence_.AnimateTo(
        target, target > 0.5 ? MotionCurve::Of(motion::kLift)
                             : MotionCurve::Of(motion::kTabClose));
  } else {
    reveal_presence_.SetValue(target);
  }
  UpdateRevealAppearance();
  InvalidateLayout();
}

void WindowControlsView::SetMaximized(bool maximized) {
  if (maximized_ == maximized) {
    return;
  }
  maximized_ = maximized;
  minimize_->SetMaximized(maximized_);
  maximize_->SetMaximized(maximized_);
  close_->SetMaximized(maximized_);
}

void WindowControlsView::SetHitPadding(const gfx::Insets& padding) {
  if (hit_padding_ == padding) {
    return;
  }
  hit_padding_ = padding;
  InvalidateLayout();
}

void WindowControlsView::SetActivationChangedCallback(
    base::RepeatingCallback<void(bool)> callback) {
  activation_changed_ = std::move(callback);
}

bool WindowControlsView::IsAutoHideActive() const {
  return auto_hide_ && presentation_ == Presentation::kFloatingGlass;
}

gfx::Rect WindowControlsView::SensorRectInLocal() const {
  const int sensor_h = std::max(kHintSensorHeight, hit_padding_.top());
  return gfx::Rect(0, 0, width(), std::min(height(), sensor_h));
}

void WindowControlsView::RevealCapsule() {
  if (!IsAutoHideActive()) {
    return;
  }
  retract_timer_.Stop();
  if (!revealed_) {
    revealed_ = true;
    if (GetWidget() && GetVisible() && !motion::ReduceMotion()) {
      reveal_presence_.AnimateTo(1.0, MotionCurve::Of(motion::kLift));
    } else {
      reveal_presence_.SetValue(1.0);
    }
    UpdateRevealAppearance();
  }
  if (!pointer_poll_.IsRunning()) {
    pointer_poll_.Start(FROM_HERE, kPointerPollInterval, this,
                        &WindowControlsView::PollPointer);
  }
}

void WindowControlsView::ScheduleRetract() {
  if (!IsAutoHideActive() || !revealed_) {
    return;
  }
  if (!retract_timer_.IsRunning()) {
    retract_timer_.Start(FROM_HERE, kRetractDelay, this,
                         &WindowControlsView::RetractCapsule);
  }
}

void WindowControlsView::RetractCapsule() {
  if (!IsAutoHideActive()) {
    return;
  }
  if (IsPointerInRevealZone()) {
    return;
  }
  revealed_ = false;
  pointer_poll_.Stop();
  retract_timer_.Stop();
  if (GetWidget() && GetVisible() && !motion::ReduceMotion()) {
    reveal_presence_.AnimateTo(0.0, MotionCurve::Of(motion::kTabClose));
  } else {
    reveal_presence_.SetValue(0.0);
  }
  UpdateRevealAppearance();
}

void WindowControlsView::PollPointer() {
  if (!IsAutoHideActive() || !revealed_) {
    pointer_poll_.Stop();
    return;
  }
  if (IsPointerInRevealZone()) {
    retract_timer_.Stop();
  } else {
    ScheduleRetract();
  }
}

bool WindowControlsView::IsPointerInRevealZone() const {
  if (any_hovered_) {
    return true;
  }
  if (!GetWidget() || !GetVisible() || GetWidget()->IsMinimized()) {
    return false;
  }
  gfx::Point point = display::Screen::Get()->GetCursorScreenPoint();
  views::View::ConvertPointFromScreen(this, &point);
  gfx::Rect zone = GetLocalBounds();
  zone.Inset(-kRevealGraceMargin);
  return zone.Contains(point);
}

gfx::Size WindowControlsView::VisualSize() const {
  const Metrics m = MetricsFor(presentation_);
  return gfx::Size(m.pad_x * 2 + m.button_w * 3 + m.gap * 2,
                   m.pad_y * 2 + m.button_h);
}

int WindowControlsView::ReservedBarWidth() const {
  if (presentation_ == Presentation::kFloatingGlass) {
    return 0;
  }
  const Metrics m = MetricsFor(presentation_);
  return VisualSize().width() + m.bar_lead_gap;
}

gfx::Rect WindowControlsView::VisualRectInLocal() const {
  const gfx::Size visual = VisualSize();
  // 视觉区域靠左下（top/right 留给最大化或隐藏态边缘感应的扩展热区）。
  return gfx::Rect(hit_padding_.left(), hit_padding_.top(), visual.width(),
                   visual.height());
}

void WindowControlsView::Layout(PassKey) {
  const gfx::Rect visual = VisualRectInLocal();
  surface_->SetBoundsRect(GetLocalBounds());
  // 边框微提示居中贴在窗口右上角顶边（8px 留白条里）。
  const int hint_w = kHintBarWidth + 8;
  const int hint_h = std::max(8, std::min(10, hit_padding_.top()));
  hint_->SetBounds(visual.right() - hint_w - 4, 0, hint_w, hint_h);
  if (glass_->GetVisible()) {
    glass_->SetGlassBounds(visual);
  }
  UpdateButtonMetrics();
  UpdateRevealAppearance();
}

void WindowControlsView::UpdateButtonMetrics() {
  const gfx::Rect visual = VisualRectInLocal();
  const Metrics m = MetricsFor(presentation_);
  const int y = visual.y() + m.pad_y;
  const int x0 = visual.x() + m.pad_x;
  const int x1 = x0 + m.button_w + m.gap;
  const int x2 = x1 + m.button_w + m.gap;

  // 最大化时关闭按钮向上、向右扩展热区到窗口最右上角像素（Fitts's Law），
  // 但视觉圆角矩形仍精准画在 visual_rect 里，美感与盲点体验兼得。
  const int top_pad = hit_padding_.top();
  const int right_pad = hit_padding_.right();
  const int bottom_pad = hit_padding_.bottom();

  minimize_->SetBounds(x0, y - top_pad, m.button_w,
                       m.button_h + top_pad + bottom_pad);
  minimize_->SetVisualRect(gfx::Rect(0, top_pad, m.button_w, m.button_h),
                           m.radius);

  maximize_->SetBounds(x1, y - top_pad, m.button_w,
                       m.button_h + top_pad + bottom_pad);
  maximize_->SetVisualRect(gfx::Rect(0, top_pad, m.button_w, m.button_h),
                           m.radius);

  close_->SetBounds(x2, y - top_pad, m.button_w + right_pad,
                    m.button_h + top_pad + bottom_pad);
  close_->SetVisualRect(gfx::Rect(0, top_pad, m.button_w, m.button_h),
                        m.radius);

  if (hovered_index_ >= 0 && hovered_index_ < 3 && !pill_x_.is_animating()) {
    const std::array<int, 3> xs = {x0, x1, x2};
    pill_x_.SetValue(xs[static_cast<size_t>(hovered_index_)]);
  }
}

void WindowControlsView::AddedToWidget() {
  views::Widget* widget = GetWidget();
  if (!widget) {
    return;
  }
  widget_observation_.Observe(widget);
  active_ = widget->ShouldPaintAsActive();
  active_presence_.SetValue(active_ ? 1.0 : kInactiveOpacity);
  SyncMaximizedFromWidget();
  ApplyVisualOpacity();
}

void WindowControlsView::RemovedFromWidget() {
  retract_timer_.Stop();
  pointer_poll_.Stop();
  widget_observation_.Reset();
}

void WindowControlsView::OnMouseEntered(const ui::MouseEvent& event) {
  if (IsAutoHideActive()) {
    RevealCapsule();
  }
}

void WindowControlsView::OnMouseMoved(const ui::MouseEvent& event) {
  if (IsAutoHideActive()) {
    RevealCapsule();
  }
}

void WindowControlsView::OnMouseExited(const ui::MouseEvent& event) {
  if (IsAutoHideActive()) {
    ScheduleRetract();
  }
}

bool WindowControlsView::DoesIntersectRect(const views::View* target,
                                           const gfx::Rect& rect) const {
  if (!GetVisible()) {
    return false;
  }
  if (IsAutoHideActive() && reveal_presence_.value() < kInteractiveReveal) {
    return SensorRectInLocal().Intersects(rect);
  }
  return GetLocalBounds().Intersects(rect);
}

void WindowControlsView::OnWidgetActivationChanged(views::Widget* widget,
                                                   bool active) {
  active_ = active;
  const double target = active_ ? 1.0 : kInactiveOpacity;
  if (GetWidget() && GetVisible() && !motion::ReduceMotion()) {
    active_presence_.AnimateTo(target, motion::Hover());
  } else {
    active_presence_.SetValue(target);
  }
  hint_->SchedulePaint();
  if (activation_changed_) {
    activation_changed_.Run(active_);
  }
}

void WindowControlsView::OnWidgetBoundsChanged(views::Widget* widget,
                                               const gfx::Rect& new_bounds) {
  SyncMaximizedFromWidget();
}

void WindowControlsView::SyncMaximizedFromWidget() {
  if (views::Widget* widget = GetWidget()) {
    SetMaximized(widget->IsMaximized());
  }
}

void WindowControlsView::UpdateRevealAppearance() {
  const bool auto_hide = IsAutoHideActive();
  const float r = auto_hide ? static_cast<float>(reveal_presence_.value()) : 1.f;
  const float r_clamped = std::clamp(r, 0.f, 1.f);

  const float hint_alpha =
      auto_hide ? std::clamp(1.f - r_clamped * 2.5f, 0.f, 1.f) : 0.f;
  hint_->SetVisible(hint_alpha > 0.001f);
  if (hint_->GetVisible()) {
    hint_->layer()->SetOpacity(hint_alpha);
    hint_->SchedulePaint();
  }

  const bool show_capsule = !auto_hide || r_clamped > 0.001f;
  surface_->SetVisible(show_capsule);
  surface_->SetCanProcessEventsWithinSubtree(
      !auto_hide || reveal_presence_.value() >= kInteractiveReveal);

  // 弹出弹簧变换：从右上角边框轻盈落出并放大到位（减弱动态效果时仅淡入淡出）。
  if (show_capsule && auto_hide && !motion::ReduceMotion() &&
      std::abs(r - 1.f) > 0.001f) {
    const gfx::Rect visual = VisualRectInLocal();
    const float pivot_x = static_cast<float>(visual.right());
    const float pivot_y = static_cast<float>(visual.y());
    const float scale = 0.80f + 0.20f * r;
    const float dy = -10.f * (1.f - r);
    gfx::Transform transform;
    transform.Translate(pivot_x, pivot_y + dy);
    transform.Scale(scale, scale);
    transform.Translate(-pivot_x, -pivot_y);
    surface_->layer()->SetTransform(transform);
    if (glass_->GetVisible()) {
      const gfx::Rect gb = glass_->bounds();
      const float g_px = pivot_x - gb.x();
      const float g_py = pivot_y - gb.y();
      gfx::Transform glass_tx;
      glass_tx.Translate(g_px, g_py + dy);
      glass_tx.Scale(scale, scale);
      glass_tx.Translate(-g_px, -g_py);
      glass_->layer()->SetTransform(glass_tx);
    }
  } else {
    surface_->layer()->SetTransform(gfx::Transform());
    if (glass_->layer()) {
      glass_->layer()->SetTransform(gfx::Transform());
    }
  }

  UpdateGlassAppearance();
}

void WindowControlsView::UpdateGlassAppearance() {
  const float g =
      static_cast<float>(std::clamp(glass_presence_.value(), 0.0, 1.0));
  const float r = IsAutoHideActive()
                      ? static_cast<float>(
                            std::clamp(reveal_presence_.value(), 0.0, 1.0))
                      : 1.f;
  // 隐藏态完全收起时关闭 GlassView，零实时背景滤镜 GPU 开销。
  const bool show_glass = g > 0.001f && r > 0.001f;
  glass_->SetVisible(show_glass);
  if (show_glass) {
    GlassStyle style;
    style.dark = glass_dark_;
    style.luma = glass_dark_ ? 0.1f : 0.9f;
    glass_->SetStyle(style);
    glass_->SetGlassBounds(VisualRectInLocal());
  }
  minimize_->SetGlassBlend(g);
  maximize_->SetGlassBlend(g);
  close_->SetGlassBlend(g);
  ApplyVisualOpacity();
  surface_->SchedulePaint();
}

void WindowControlsView::ApplyVisualOpacity() {
  const float g =
      static_cast<float>(std::clamp(glass_presence_.value(), 0.0, 1.0));
  const float r = IsAutoHideActive()
                      ? static_cast<float>(
                            std::clamp(reveal_presence_.value() * 1.35, 0.0, 1.0))
                      : 1.f;
  const float active = static_cast<float>(
      std::clamp(active_presence_.value(), 0.0, 1.0));
  const float cap_alpha =
      static_cast<float>(std::clamp(capsule_opacity_.value(), 0.0, 1.0));

  // 悬浮玻璃胶囊常态轻透（0.82），悬停亮至 1.0；窗口失焦时与活跃度联动。
  if (glass_->GetVisible()) {
    const float wake = any_hovered_ ? 1.f : active;
    glass_->layer()->SetOpacity(g * r * cap_alpha * (0.55f + 0.45f * wake));
  }
  // 悬浮玻璃胶囊态下整体内容随胶囊轻透；横栏态下内容完全不透明，只按活跃度柔降符号。
  const float surface_alpha = (1.f - g * (1.f - cap_alpha)) * r;
  surface_->layer()->SetOpacity(surface_alpha);
  minimize_->SetWindowActiveBlend(active);
  maximize_->SetWindowActiveBlend(active);
  close_->SetWindowActiveBlend(active);
}

void WindowControlsView::UpdateCapsuleHover(bool hovered) {
  if (any_hovered_ == hovered) {
    return;
  }
  any_hovered_ = hovered;
  if (hovered && IsAutoHideActive()) {
    RevealCapsule();
  } else if (!hovered && IsAutoHideActive()) {
    ScheduleRetract();
  }
  const double target = hovered ? 1.0 : kGlassRestingOpacity;
  if (GetWidget() && GetVisible() && !motion::ReduceMotion()) {
    capsule_opacity_.AnimateTo(target, motion::Hover());
  } else {
    capsule_opacity_.SetValue(target);
  }
}

void WindowControlsView::OnButtonHoverChanged(int index, bool hovered) {
  const gfx::Rect visual = VisualRectInLocal();
  const std::array<int, 3> x_slots = {
      visual.x() + kGlassPad,
      visual.x() + kGlassPad + kGlassButtonW + kGlassGap,
      visual.x() + kGlassPad + (kGlassButtonW + kGlassGap) * 2,
  };
  if (hovered && index >= 0 && index < 3) {
    const bool was_hidden = hovered_index_ < 0 || pill_opacity_.value() < 0.05;
    hovered_index_ = index;
    UpdateCapsuleHover(true);
    const int slot_x = x_slots[static_cast<size_t>(index)];
    if (was_hidden || motion::ReduceMotion()) {
      pill_x_.SetValue(slot_x);
    } else {
      pill_x_.AnimateTo(slot_x, MotionCurve::Of(motion::kMenuHighlight));
    }
    const double danger_target = index == 2 ? 1.0 : 0.0;
    if (motion::ReduceMotion()) {
      pill_opacity_.SetValue(1.0);
      pill_danger_.SetValue(danger_target);
    } else {
      pill_opacity_.AnimateTo(1.0, motion::Hover());
      pill_danger_.AnimateTo(danger_target, motion::Hover());
    }
    return;
  }
  if (hovered_index_ == index) {
    hovered_index_ = -1;
    UpdateCapsuleHover(false);
    if (motion::ReduceMotion()) {
      pill_opacity_.SetValue(0.0);
    } else {
      pill_opacity_.AnimateTo(0.0, motion::Hover());
    }
  }
}

void WindowControlsView::OnButtonPressed(int index) {
  views::Widget* widget = GetWidget();
  if (!widget) {
    return;
  }
  switch (index) {
    case 0:
      widget->Minimize();
      break;
    case 1:
      if (widget->IsMaximized()) {
        widget->Restore();
      } else {
        widget->Maximize();
      }
      break;
    case 2:
      widget->CloseWithReason(views::Widget::ClosedReason::kCloseButtonClicked);
      break;
    default:
      break;
  }
}

BEGIN_METADATA(WindowControlsView)
END_METADATA

}  // namespace starry
