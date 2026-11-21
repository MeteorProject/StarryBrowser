// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/update/update_toast.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/archive/archive_toast.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "starry/browser/updater/platform_updater.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view_targeter.h"

namespace starry {

namespace {

constexpr int kHeight = 40;
constexpr int kPadding = 12;
constexpr int kIconSize = 16;
constexpr int kIconGap = 9;
constexpr int kTextGap = 12;
constexpr int kPillHeight = 26;
constexpr int kPillPadding = 11;
constexpr int kPillGap = 4;
constexpr int kTrailing = 7;
constexpr int kInset = 12;
constexpr int kStackGap = 8;

SkColor WithAlpha(SkColor color, float alpha) {
  return SkColorSetA(color, static_cast<U8CPU>(SkColorGetA(color) *
                                               std::clamp(alpha, 0.0f, 1.0f)));
}

const gfx::FontList& TextFont() {
  return UiFont(12, gfx::Font::Weight::MEDIUM);
}

const gfx::FontList& PillFont() {
  return UiFont(12, gfx::Font::Weight::SEMIBOLD);
}

int PillWidth(const std::u16string& text) {
  return gfx::GetStringWidth(text, PillFont()) + 2 * kPillPadding;
}

bool IsLinuxPackage(updater::InstallKind kind) {
  return kind == updater::InstallKind::kLinuxDeb ||
         kind == updater::InstallKind::kLinuxRpm;
}

std::u16string ToastLabel(const updater::UpdaterService::StateSnapshot& s) {
  if (s.status == updater::UpdateStatus::kReady) {
    return IsLinuxPackage(s.install_kind)
               ? L("updates.toast.downloaded",
                   {{"version", s.available_version}})
               : L("updates.toast.ready", {{"version", s.available_version}});
  }
  return L("updates.toast.available", {{"version", s.available_version}});
}

std::u16string PrimaryButtonLabel(
    const updater::UpdaterService::StateSnapshot& s) {
  if (s.status == updater::UpdateStatus::kReady) {
    return IsLinuxPackage(s.install_kind) ? L("updates.action.install")
                                          : L("updates.action.relaunch");
  }
  return L("updates.toast.view");
}

}  // namespace

class UpdateToast : public GlassView {
  METADATA_HEADER(UpdateToast, GlassView)

 public:
  enum class Part { kNone, kBody, kPrimary, kDismiss };

  UpdateToast(UpdateToastHost* host,
              std::u16string text,
              std::u16string primary_label,
              const gfx::VectorIcon* icon)
      : host_(host),
        text_(std::move(text)),
        primary_label_(std::move(primary_label)),
        icon_(icon) {
    const Theme& theme = host->theme();
    SetCornerRadius(radius::kCard, true);
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    GetViewAccessibility().SetRole(ax::mojom::Role::kAlert);
    GetViewAccessibility().SetName(text_);
  }

  void UpdateContent(std::u16string text,
                     std::u16string primary_label,
                     const gfx::VectorIcon* icon) {
    text_ = std::move(text);
    primary_label_ = std::move(primary_label);
    icon_ = icon;
    GetViewAccessibility().SetName(text_);
    SchedulePaint();
  }

  const std::u16string& text() const { return text_; }
  const std::u16string& primary_label() const { return primary_label_; }
  LayerMotion& motion() { return motion_; }

  gfx::Size ContentSize() const {
    const int text_w = std::min(260, gfx::GetStringWidth(text_, TextFont()));
    return gfx::Size(kPadding + kIconSize + kIconGap + text_w + kTextGap +
                         PillWidth(primary_label_) + kPillGap +
                         PillWidth(L("updates.toast.later")) + kTrailing,
                     kHeight);
  }

  gfx::Rect DismissRect() const {
    const gfx::Rect rect = glass_rect();
    const int width = PillWidth(L("updates.toast.later"));
    return gfx::Rect(rect.right() - kTrailing - width,
                     rect.y() + (rect.height() - kPillHeight) / 2, width,
                     kPillHeight);
  }

  gfx::Rect PrimaryRect() const {
    const gfx::Rect dismiss = DismissRect();
    const int width = PillWidth(primary_label_);
    return gfx::Rect(dismiss.x() - kPillGap - width, dismiss.y(), width,
                     kPillHeight);
  }

  Part PartAt(const gfx::Point& point) const {
    if (!glass_rect().Contains(point)) {
      return Part::kNone;
    }
    if (PrimaryRect().Contains(point)) {
      return Part::kPrimary;
    }
    if (DismissRect().Contains(point)) {
      return Part::kDismiss;
    }
    return Part::kBody;
  }

  void OnPaint(gfx::Canvas* canvas) override {
    GlassView::OnPaint(canvas);
    const Theme& theme = host_->theme();
    const gfx::Rect rect = glass_rect();
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(theme.surface_panel,
                              theme.is_dark ? 0.66f * 255 : 0.76f * 255));
    canvas->DrawRoundRect(gfx::RectF(rect), radius::kCard, fill);

    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme.OnSurface(0.1f));
    gfx::RectF stroke(rect);
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius::kCard - 0.5f, border);

    canvas->Save();
    canvas->Translate(gfx::Vector2d(
        rect.x() + kPadding, rect.y() + (rect.height() - kIconSize) / 2));
    gfx::PaintVectorIcon(canvas, *icon_, kIconSize, theme.accent);
    canvas->Restore();

    const int text_x = rect.x() + kPadding + kIconSize + kIconGap;
    canvas->DrawStringRectWithFlags(
        text_, TextFont(), theme.on_surface,
        gfx::Rect(text_x, rect.y(),
                  std::max(0, PrimaryRect().x() - kTextGap - text_x),
                  rect.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);

    const gfx::Rect primary = PrimaryRect();
    fill.setColor(
        WithAlpha(theme.accent, hovered_ == Part::kPrimary ? 0.26f : 0.16f));
    canvas->DrawRoundRect(gfx::RectF(primary), kPillHeight / 2.0f, fill);
    canvas->DrawStringRectWithFlags(
        primary_label_, PillFont(), theme.accent, primary,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);

    const gfx::Rect dismiss = DismissRect();
    fill.setColor(theme.OnSurface(hovered_ == Part::kDismiss ? 0.11f : 0.06f));
    canvas->DrawRoundRect(gfx::RectF(dismiss), kPillHeight / 2.0f, fill);
    canvas->DrawStringRectWithFlags(
        L("updates.toast.later"), PillFont(), theme.on_surface, dismiss,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void OnMouseMoved(const ui::MouseEvent& event) override {
    const Part part = PartAt(event.location());
    if (part != hovered_) {
      hovered_ = part;
      SchedulePaint();
    }
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_ = Part::kNone;
    pressed_ = Part::kNone;
    SchedulePaint();
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    pressed_ =
        event.IsOnlyLeftMouseButton() ? PartAt(event.location()) : Part::kNone;
    return pressed_ != Part::kNone;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    const Part part = PartAt(event.location());
    const bool clicked = part != Part::kNone && part == pressed_;
    pressed_ = Part::kNone;
    if (!clicked) {
      return;
    }
    if (part == Part::kDismiss) {
      host_->DismissAction();
      return;
    }
    host_->PrimaryAction();
  }

 private:
  const raw_ptr<UpdateToastHost> host_;
  std::u16string text_;
  std::u16string primary_label_;
  raw_ptr<const gfx::VectorIcon> icon_;
  Part hovered_ = Part::kNone;
  Part pressed_ = Part::kNone;
  LayerMotion motion_;
};

BEGIN_METADATA(UpdateToast)
END_METADATA

UpdateToastHost::UpdateToastHost(BrowserWindowInterface* browser)
    : browser_(browser) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
}

UpdateToastHost::~UpdateToastHost() = default;

void UpdateToastHost::SetTheme(const Theme& theme) {
  theme_ = theme;
  if (toast_) {
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    toast_->SetStyle(style);
    toast_->SchedulePaint();
  }
}

gfx::Rect UpdateToastHost::ToastRect() const {
  return toast_ ? toast_->glass_rect() + toast_->bounds().OffsetFromOrigin()
                : gfx::Rect();
}

void UpdateToastHost::PrimaryAction() {
  updater::UpdaterService* service = updater::UpdaterService::Get();
  const updater::UpdaterService::StateSnapshot s = service->Snapshot();
  if (s.status == updater::UpdateStatus::kReady) {
    service->ApplyUpdate();
    return;
  }
  service->DismissNotification();
  OpenStarrySettings(browser_, "general");
}

void UpdateToastHost::DismissAction() {
  updater::UpdaterService::Get()->DismissNotification();
}

void UpdateToastHost::AddedToWidget() {
  observation_.Observe(updater::UpdaterService::Get());
  Update();
}

void UpdateToastHost::RemovedFromWidget() {
  observation_.Reset();
}

void UpdateToastHost::Layout(PassKey) {
  PlaceToast();
}

bool UpdateToastHost::DoesIntersectRect(const views::View* target,
                                        const gfx::Rect& rect) const {
  return ToastRect().Intersects(rect);
}

void UpdateToastHost::OnUpdateStateChanged() {
  Update();
}

void UpdateToastHost::Update() {
  updater::UpdaterService* service = updater::UpdaterService::Get();
  const bool show = service->ShouldShowToast();
  if (show) {
    const updater::UpdaterService::StateSnapshot s = service->Snapshot();
    if (!toast_) {
      Show(s);
    } else {
      const std::u16string label = ToastLabel(s);
      const std::u16string action = PrimaryButtonLabel(s);
      if (label != toast_->text() || action != toast_->primary_label()) {
        const gfx::VectorIcon* icon =
            s.status == updater::UpdateStatus::kReady
                ? &icons::kArrowClockwise
                : &icons::kArrowDownCircle;
        toast_->UpdateContent(label, action, icon);
        PlaceToast();
        NotifyObstacles();
      }
    }
  } else if (toast_) {
    Hide();
  }
}

void UpdateToastHost::Show(const updater::UpdaterService::StateSnapshot& s) {
  const gfx::VectorIcon* icon =
      s.status == updater::UpdateStatus::kReady ? &icons::kArrowClockwise
                                                : &icons::kArrowDownCircle;
  toast_ = AddChildView(std::make_unique<UpdateToast>(
      this, ToastLabel(s), PrimaryButtonLabel(s), icon));
  PlaceToast();
  LayerPose from;
  if (!motion::ReduceMotion()) {
    from.offset = gfx::Vector2dF(-24, 0);
    from.scale = 0.96f;
  }
  const gfx::Rect glass = toast_->glass_rect();
  toast_->motion().AnimateTransformFrom(
      toast_->layer(), from, LayerPose(), MotionCurve::Of(motion::kSelection),
      gfx::PointF(glass.x(), glass.CenterPoint().y()));
  toast_->motion().AnimateOpacityFrom(toast_->layer(), 0, 1,
                                      MotionCurve::EaseOut(0.2));
  NotifyObstacles();
}

void UpdateToastHost::Hide() {
  UpdateToast* toast = toast_;
  toast_ = nullptr;
  toast->SetCanProcessEventsWithinSubtree(false);
  LayerPose to;
  if (!motion::ReduceMotion()) {
    to.offset = gfx::Vector2dF(-16, 0);
  }
  toast->motion().AnimateTransform(toast->layer(), to,
                                   MotionCurve::EaseOut(0.18));
  toast->motion().AnimateOpacity(
      toast->layer(), 0, MotionCurve::EaseOut(0.18),
      base::BindOnce(
          [](base::WeakPtr<UpdateToastHost> host, UpdateToast* toast) {
            if (!host) {
              return;
            }
            for (views::View* child : host->children()) {
              if (child == toast) {
                host->RemoveChildViewT(toast);
                break;
              }
            }
          },
          weak_factory_.GetWeakPtr(), toast));
  NotifyObstacles();
}

void UpdateToastHost::PlaceToast() {
  if (!toast_) {
    return;
  }
  int bottom_offset = kInset;
  if (StarryBrowserView* view = StarryBrowserView::From(browser_);
      view && view->archive_toast()) {
    const gfx::Rect archive_rect = view->archive_toast()->ToastRect();
    if (!archive_rect.IsEmpty()) {
      bottom_offset += archive_rect.height() + kStackGap;
    }
  }
  const gfx::Size size = toast_->ContentSize();
  toast_->SetGlassBounds(gfx::Rect(kInset,
                                   height() - bottom_offset - size.height(),
                                   size.width(), size.height()));
}

void UpdateToastHost::NotifyObstacles() {
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    view->MediaObstaclesChanged();
  }
}

BEGIN_METADATA(UpdateToastHost)
END_METADATA

}  // namespace starry
