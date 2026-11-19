// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/assistant_capsules.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/layer_loop.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/spring.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_delegate.h"
#include "ui/compositor/layer_textured.h"
#include "ui/compositor/paint_recorder.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/view_targeter.h"

namespace starry {

namespace {

constexpr int kHeight = 34;
constexpr int kGap = 6;
constexpr int kStatusX = 10;
constexpr int kStatusSide = 15;
constexpr int kTitleX = 30;
constexpr int kTitleMax = 160;
constexpr int kDetailMax = 130;
constexpr int kDismissSide = 18;
// 冒出来：缩放 0.9、往下 10，弹簧 0.34/0.78，不透明度 0.14 秒；缩回去：0.94、往下 6，
// 0.16 秒；挪位置：弹簧 0.42/0.86。
constexpr Spring kEnter{0.34, 0.78};
constexpr Spring kSlide{0.42, 0.86};
constexpr double kEnterFade = 0.14;
constexpr double kLeave = 0.16;

const gfx::FontList& TitleFont() {
  return UiFont(13, gfx::Font::Weight::MEDIUM);
}

const gfx::FontList& DetailFont() {
  return UiFont(13);
}

SkColor TintBase(const Theme& theme) {
  return theme.is_dark ? theme.surface_panel : theme.surface_bright;
}

// 画转圈的那一段圆弧。
class ArcPainter : public ui::LayerDelegate {
 public:
  void SetLayer(ui::Layer* layer) { layer_ = layer; }
  void SetColor(SkColor color) { color_ = color; }

  // ui::LayerDelegate:
  void OnPaintLayer(const ui::PaintContext& context) override {
    ui::PaintRecorder recorder(context, layer_->size());
    gfx::Canvas* canvas = recorder.canvas();
    gfx::RectF rect{gfx::SizeF(layer_->size())};
    rect.Inset(2.5f);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1.6f);
    flags.setStrokeCap(cc::PaintFlags::kRound_Cap);
    flags.setColor(color_);
    SkPathBuilder path;
    path.addArc(gfx::RectFToSkRect(rect), -90, 270);
    canvas->DrawPath(path.detach(), flags);
  }
  void OnDeviceScaleFactorChanged(float old_scale, float new_scale) override {}

 private:
  raw_ptr<ui::Layer> layer_ = nullptr;
  SkColor color_ = SK_ColorGRAY;
};

// 转圈：一段圆弧绕着中心转（0.9 秒一圈）。
class CapsuleSpinner : public views::View {
  METADATA_HEADER(CapsuleSpinner, views::View)

 public:
  CapsuleSpinner() {
    SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    SetCanProcessEventsWithinSubtree(false);
    pivot_ = ui::Layer::Create(ui::LAYER_NOT_DRAWN);
    arc_ = std::make_unique<ui::LayerTextured>();
    painter_.SetLayer(arc_.get());
    arc_->set_delegate(&painter_);
    arc_->SetFillsBoundsOpaquely(false);
    pivot_->Add(arc_.get());
    layer()->Add(pivot_.get());
    // 合成器上一直转，不占主线程（LayerLoop）。
    spin_.Spin(pivot_.get(), base::Milliseconds(900));
  }

  ~CapsuleSpinner() override {
    spin_.Stop();
    arc_->set_delegate(nullptr);
  }

  void SetColor(SkColor color) {
    painter_.SetColor(color);
    arc_->SchedulePaint(gfx::Rect(arc_->size()));
  }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    const gfx::Size size = this->size();
    pivot_->SetBounds(gfx::Rect(size.width() / 2, size.height() / 2, 1, 1));
    arc_->SetBounds(gfx::Rect(-size.width() / 2, -size.height() / 2,
                              size.width(), size.height()));
    arc_->SchedulePaint(gfx::Rect(size));
  }

 private:
  ArcPainter painter_;
  std::unique_ptr<ui::Layer> pivot_;
  std::unique_ptr<ui::Layer> arc_;
  LayerLoop spin_;
};

BEGIN_METADATA(CapsuleSpinner)
END_METADATA

}  // namespace

// 胶囊玻璃上面画的东西，接鼠标。
class CapsuleBody : public views::View {
  METADATA_HEADER(CapsuleBody, views::View)

 public:
  explicit CapsuleBody(AssistantCapsuleView* capsule) : capsule_(capsule) {}

  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override {
    return event.IsOnlyLeftMouseButton();
  }
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;

 private:
  const raw_ptr<AssistantCapsuleView> capsule_;
};

BEGIN_METADATA(CapsuleBody)
END_METADATA

// 一个胶囊：玻璃（GlassView）和上面画的东西。overflow 的是「还有 N 个」。
// 视图比玻璃大一圈（阴影），只有玻璃本身接鼠标，免得挡住旁边的胶囊。
class AssistantCapsuleView : public GlassView,
                             public views::ViewTargeterDelegate {
  METADATA_HEADER(AssistantCapsuleView, GlassView)

 public:
  AssistantCapsuleView(AssistantCapsules* owner, bool overflow)
      : owner_(owner), overflow_(overflow) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetCornerRadius(kHeight / 2.0f, true);
    body_ = AddChildView(std::make_unique<CapsuleBody>(this));
    if (!overflow) {
      spinner_ = body_->AddChildView(std::make_unique<CapsuleSpinner>());
      spinner_->SetBoundsRect(gfx::Rect(kStatusX, (kHeight - kStatusSide) / 2,
                                        kStatusSide, kStatusSide));
    }
  }

  const std::string& id() const { return info_.id; }
  bool placed() const { return placed_; }
  void set_placed(bool placed) { placed_ = placed; }
  bool leaving() const { return leaving_; }
  void set_leaving() {
    leaving_ = true;
    SetCanProcessEventsWithinSubtree(false);
  }
  LayerMotion& motion() { return motion_; }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    if (spinner_) {
      spinner_->SetColor(theme.on_surface_variant);
    }
    body_->SchedulePaint();
  }

  void SetInfo(const AssistantController::Capsule& info) {
    info_ = info;
    if (spinner_) {
      spinner_->SetVisible(info.busy && !info.waiting);
    }
    SetTooltip();
    body_->SchedulePaint();
  }

  void SetCount(int count) {
    count_ = count;
    body_->SchedulePaint();
  }

  // 宽度：30 + 标题（最多 160）+ 状态（最多 130）+ 13；
  // 「还有 N 个」是字宽 + 27。
  int PreferredCapsuleWidth() const {
    if (overflow_) {
      return gfx::GetStringWidth(MoreText(), DetailFont()) + 3 + 24;
    }
    const int title =
        std::min(gfx::GetStringWidth(info_.title, TitleFont()) + 3, kTitleMax);
    const int detail =
        info_.detail.empty()
            ? 0
            : std::min(gfx::GetStringWidth(info_.detail, DetailFont()) + 12,
                       kDetailMax);
    return kTitleX + title + detail + 13;
  }

  void SetCapsuleBounds(const gfx::Rect& rect) {
    SetGlassBounds(rect);
    body_->SetBoundsRect(glass_rect());
  }

  gfx::Rect capsule_bounds() const { return glass_bounds(); }

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override {
    return glass_rect().Intersects(rect);
  }

  // 缩放的中心：胶囊的下边中点（图层坐标）。
  gfx::PointF Origin() const {
    const gfx::Rect rect = glass_rect();
    return gfx::PointF(rect.CenterPoint().x(), rect.bottom());
  }

  std::u16string MoreText() const {
    return L("assistant.capsule.more",
             {{"count", base::NumberToString16(count_)}});
  }

  gfx::Rect DismissRect() const {
    const int width = body_->width();
    return gfx::Rect(width - 23, (kHeight - kDismissSide) / 2, kDismissSide,
                     kDismissSide);
  }

  void SetTooltip() {
    body_->SetTooltipText(overflow_ ? std::u16string() : info_.title);
  }

 public:
  void SetHovered(bool hovered) {
    if (hovered_ != hovered) {
      hovered_ = hovered;
      body_->SchedulePaint();
    }
  }

  void Clicked(const gfx::Point& point) {
    if (overflow_) {
      owner_->callbacks_.more.Run();
      return;
    }
    if (hovered_ && DismissRect().Contains(point)) {
      owner_->callbacks_.dismiss.Run(info_.id);
      return;
    }
    owner_->callbacks_.open.Run(info_.id);
  }

  void PaintBody(gfx::Canvas* canvas) {
    const gfx::RectF bounds(body_->GetLocalBounds());
    const float radius = bounds.height() / 2;
    // 一层主题的面板色（同岛只有输入框时：46%），一圈细边。
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(TintBase(theme_), 0.46f * 255));
    canvas->DrawRoundRect(bounds, radius, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius - 0.5f, border);
    const int height = body_->height();
    constexpr int kFlags = gfx::Canvas::NO_SUBPIXEL_RENDERING;
    if (overflow_) {
      canvas->DrawStringRectWithFlags(
          MoreText(), DetailFont(), theme_.on_surface_variant,
          gfx::Rect(12, 0, body_->width() - 12, height), kFlags);
      return;
    }
    // 状态：等用户是黄色的感叹号，做完了是勾（在做时是转圈，CapsuleSpinner）。
    if (!info_.busy || info_.waiting) {
      const gfx::PointF center(kStatusX + kStatusSide / 2.0f, height / 2.0f);
      cc::PaintFlags dot;
      dot.setAntiAlias(true);
      dot.setColor(info_.waiting ? theme_.warning() : theme_.accent);
      canvas->DrawCircle(center, 6.5f, dot);
      cc::PaintFlags mark;
      mark.setAntiAlias(true);
      mark.setStyle(cc::PaintFlags::kStroke_Style);
      mark.setStrokeWidth(1.6f);
      mark.setStrokeCap(cc::PaintFlags::kRound_Cap);
      mark.setStrokeJoin(cc::PaintFlags::kRound_Join);
      mark.setColor(TintBase(theme_) | 0xFF000000);
      SkPathBuilder path;
      if (info_.waiting) {
        path.moveTo(center.x(), center.y() - 3.2f);
        path.lineTo(center.x(), center.y() + 0.6f);
        canvas->DrawPath(path.detach(), mark);
        cc::PaintFlags point = mark;
        point.setStyle(cc::PaintFlags::kFill_Style);
        canvas->DrawCircle(gfx::PointF(center.x(), center.y() + 3.0f), 0.95f,
                           point);
      } else {
        path.moveTo(center.x() - 3.0f, center.y() + 0.2f);
        path.lineTo(center.x() - 0.8f, center.y() + 2.4f);
        path.lineTo(center.x() + 3.2f, center.y() - 2.2f);
        canvas->DrawPath(path.detach(), mark);
      }
    }
    const int room = body_->width() - kTitleX - 13;
    const int title_width =
        std::min({gfx::GetStringWidth(info_.title, TitleFont()) + 3, kTitleMax,
                  std::max(0, room)});
    canvas->DrawStringRectWithFlags(info_.title, TitleFont(), theme_.on_surface,
                                    gfx::Rect(kTitleX, 0, title_width, height),
                                    kFlags);
    if (!info_.detail.empty()) {
      const int x = kTitleX + title_width + 4;
      const int width =
          std::min(gfx::GetStringWidth(info_.detail, DetailFont()) + 8,
                   std::min(125, room - 60));
      const SkColor color = info_.waiting ? theme_.warning()
                            : info_.done  ? theme_.accent
                                          : theme_.on_surface_variant;
      if (width > 0) {
        canvas->DrawStringRectWithFlags(info_.detail, DetailFont(), color,
                                        gfx::Rect(x, 0, width, height), kFlags);
      }
    }
    // 悬停时右边的 ×（没有底色）。
    if (hovered_) {
      const gfx::Rect dismiss = DismissRect();
      cc::PaintFlags clear;
      clear.setAntiAlias(true);
      clear.setColor(SkColorSetA(TintBase(theme_), 0xF0));
      canvas->DrawCircle(gfx::PointF(dismiss.CenterPoint()),
                         kDismissSide / 2.0f, clear);
      canvas->Save();
      canvas->Translate(gfx::Vector2d(dismiss.x() + 5, dismiss.y() + 5));
      gfx::PaintVectorIcon(canvas, icons::kXmark, 8, theme_.on_surface_variant);
      canvas->Restore();
    }
  }

 private:
  const raw_ptr<AssistantCapsules> owner_;
  const bool overflow_;
  raw_ptr<CapsuleBody> body_ = nullptr;
  raw_ptr<CapsuleSpinner> spinner_ = nullptr;
  Theme theme_ = Theme::DarkBase();
  AssistantController::Capsule info_;
  int count_ = 0;
  bool hovered_ = false;
  bool placed_ = false;
  bool leaving_ = false;
  LayerMotion motion_;
};

BEGIN_METADATA(AssistantCapsuleView)
END_METADATA

void CapsuleBody::OnPaint(gfx::Canvas* canvas) {
  capsule_->PaintBody(canvas);
}

void CapsuleBody::OnMouseReleased(const ui::MouseEvent& event) {
  if (HitTestPoint(event.location())) {
    capsule_->Clicked(event.location());
  }
}

void CapsuleBody::OnMouseEntered(const ui::MouseEvent& event) {
  capsule_->SetHovered(true);
}

void CapsuleBody::OnMouseExited(const ui::MouseEvent& event) {
  capsule_->SetHovered(false);
}

AssistantCapsules::Callbacks::Callbacks() = default;
AssistantCapsules::Callbacks::Callbacks(Callbacks&&) = default;
AssistantCapsules::Callbacks& AssistantCapsules::Callbacks::operator=(
    Callbacks&&) = default;
AssistantCapsules::Callbacks::~Callbacks() = default;

AssistantCapsules::AssistantCapsules(Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  overflow_ = AddChildView(
      std::make_unique<AssistantCapsuleView>(this, /*overflow=*/true));
  overflow_->SetVisible(false);
}

AssistantCapsules::~AssistantCapsules() = default;

void AssistantCapsules::SetTheme(const Theme& theme) {
  theme_ = theme;
  overflow_->SetTheme(theme);
  for (AssistantCapsuleView* capsule : capsules_) {
    capsule->SetTheme(theme);
  }
}

void AssistantCapsules::Update(
    const std::vector<AssistantController::Capsule>& list) {
  std::vector<raw_ptr<AssistantCapsuleView>> next;
  for (const AssistantController::Capsule& info : list) {
    auto it = std::ranges::find_if(capsules_, [&](AssistantCapsuleView* view) {
      return view->id() == info.id;
    });
    AssistantCapsuleView* capsule = nullptr;
    if (it != capsules_.end()) {
      capsule = *it;
    } else {
      capsule = AddChildView(
          std::make_unique<AssistantCapsuleView>(this, /*overflow=*/false));
      capsule->SetTheme(theme_);
    }
    capsule->SetInfo(info);
    next.push_back(capsule);
  }
  for (AssistantCapsuleView* capsule : capsules_) {
    if (std::ranges::find(next, capsule) == next.end()) {
      Leave(capsule);
    }
  }
  capsules_ = std::move(next);
}

void AssistantCapsules::Leave(AssistantCapsuleView* capsule) {
  capsule->set_leaving();
  if (motion::ReduceMotion() || !capsule->placed()) {
    RemoveChildViewT(capsule);
    return;
  }
  LayerPose pose;
  pose.offset = gfx::Vector2dF(0, 6);
  pose.scale = 0.94f;
  capsule->motion().AnimateTransform(
      capsule->layer(), pose, MotionCurve::EaseOut(kLeave), capsule->Origin());
  // 放完再拿掉（不在它自己的动画回调里删它）。
  capsule->motion().AnimateOpacity(
      capsule->layer(), 0, MotionCurve::EaseOut(kLeave),
      base::BindOnce(
          [](base::WeakPtr<AssistantCapsules> owner,
             AssistantCapsuleView* capsule) {
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(&AssistantCapsules::Remove, owner,
                                          base::Unretained(capsule)));
          },
          weak_factory_.GetWeakPtr(), base::Unretained(capsule)));
}

void AssistantCapsules::Remove(AssistantCapsuleView* capsule) {
  if (capsule != overflow_ && Contains(capsule) &&
      std::ranges::find(capsules_, capsule) == capsules_.end()) {
    RemoveChildViewT(capsule);
  }
}

void AssistantCapsules::Place(AssistantCapsuleView* capsule,
                              const gfx::Rect& rect) {
  const gfx::Rect old = capsule->capsule_bounds();
  capsule->SetCapsuleBounds(rect);
  if (motion::ReduceMotion()) {
    capsule->set_placed(true);
    return;
  }
  if (!capsule->placed()) {
    // 刚出来：从下面冒出来。
    capsule->set_placed(true);
    LayerPose from;
    from.offset = gfx::Vector2dF(0, 10);
    from.scale = 0.9f;
    capsule->motion().AnimateTransformFrom(capsule->layer(), from, LayerPose(),
                                           MotionCurve::Of(kEnter),
                                           capsule->Origin());
    capsule->motion().AnimateOpacityFrom(capsule->layer(), 0, 1,
                                         MotionCurve::EaseOut(kEnterFade));
    return;
  }
  if (old.origin() != rect.origin()) {
    capsule->motion().ShiftAndAnimate(
        capsule->layer(),
        gfx::Vector2dF(old.x() - rect.x(), old.y() - rect.y()), LayerPose(),
        MotionCurve::Of(kSlide), capsule->Origin());
  }
}

void AssistantCapsules::LayoutFloating(int center_x, int bottom, int room) {
  // 一个一个往里放，放不下的收进「还有 N 个」。
  std::vector<int> widths;
  for (AssistantCapsuleView* capsule : capsules_) {
    widths.push_back(capsule->PreferredCapsuleWidth());
  }
  size_t shown = 0;
  int used = 0;
  int more_width = 0;
  for (size_t i = 0; i < capsules_.size(); ++i) {
    const bool last = i + 1 == capsules_.size();
    overflow_->SetCount(static_cast<int>(capsules_.size() - i - 1));
    more_width = last ? 0 : overflow_->PreferredCapsuleWidth() + kGap;
    const int next = used + (shown ? kGap : 0) + widths[i];
    if (next + more_width > room) {
      break;
    }
    used = next;
    ++shown;
  }
  const int hidden = static_cast<int>(capsules_.size() - shown);
  overflow_->SetCount(hidden);
  const int overflow_width = hidden ? overflow_->PreferredCapsuleWidth() : 0;
  const int total = used + (hidden ? (shown ? kGap : 0) + overflow_width : 0);
  int x = center_x - total / 2;
  const int y = bottom - kHeight;
  for (size_t i = 0; i < capsules_.size(); ++i) {
    AssistantCapsuleView* capsule = capsules_[i];
    capsule->SetVisible(i < shown);
    if (i < shown) {
      Place(capsule, gfx::Rect(x, y, widths[i], kHeight));
      x += widths[i] + kGap;
    }
  }
  overflow_->SetVisible(hidden > 0);
  if (hidden) {
    overflow_->SetCapsuleBounds(gfx::Rect(x, y, overflow_width, kHeight));
  }
}

void AssistantCapsules::LayoutDocked(const gfx::Rect& row) {
  overflow_->SetVisible(false);
  const int count = static_cast<int>(capsules_.size());
  if (!count) {
    return;
  }
  const int each = static_cast<int>(std::floor(
      (row.width() - (count - 1) * kGap) / static_cast<double>(count)));
  for (int i = 0; i < count; ++i) {
    capsules_[i]->SetVisible(true);
    Place(capsules_[i],
          gfx::Rect(row.x() + i * (each + kGap), row.y(), each, kHeight));
  }
}

bool AssistantCapsules::HitsCapsule(const gfx::Rect& rect) const {
  for (const views::View* child : children()) {
    const auto* capsule = static_cast<const AssistantCapsuleView*>(child);
    if (capsule->GetVisible() && !capsule->leaving() &&
        capsule->capsule_bounds().Intersects(rect)) {
      return true;
    }
  }
  return false;
}

bool AssistantCapsules::DoesIntersectRect(const views::View* target,
                                          const gfx::Rect& rect) const {
  return HitsCapsule(rect);
}

BEGIN_METADATA(AssistantCapsules)
END_METADATA

}  // namespace starry
