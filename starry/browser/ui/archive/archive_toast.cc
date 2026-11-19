// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/archive/archive_toast.h"

#include <algorithm>
#include <memory>

#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/archive/archive_notice.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "starry/browser/ui/webui/starry_pages.h"
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

// 高 40：左 12、图标 16、间距 9、文字、间距 12、两个胶囊（高 26，左右 11，之间 4）、右 7。
constexpr int kHeight = 40;
constexpr int kPadding = 12;
constexpr int kIconSize = 16;
constexpr int kIconGap = 9;
constexpr int kTextGap = 12;
constexpr int kPillHeight = 26;
constexpr int kPillPadding = 11;
constexpr int kPillGap = 4;
constexpr int kTrailing = 7;
// 离内容卡片的左边、下边多远。
constexpr int kInset = 12;

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

}  // namespace

class ArchiveToast : public GlassView {
  METADATA_HEADER(ArchiveToast, GlassView)

 public:
  enum class Part { kNone, kBody, kUndo, kView };

  // has_view：有「查看」（归档的提示有，整理进文件夹的没有）。
  ArchiveToast(ArchiveToastHost* host,
               std::u16string text,
               const gfx::VectorIcon* icon,
               bool has_view)
      : host_(host), text_(std::move(text)), icon_(icon), has_view_(has_view) {
    const Theme& theme = host->theme();
    SetCornerRadius(radius::kCard, true);
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    GetViewAccessibility().SetRole(ax::mojom::Role::kAlert);
    GetViewAccessibility().SetName(Text());
  }

  const std::u16string& Text() const { return text_; }

  void SetText(std::u16string text) {
    text_ = std::move(text);
    GetViewAccessibility().SetName(text_);
    SchedulePaint();
  }

  LayerMotion& motion() { return motion_; }

  gfx::Size ContentSize() const {
    const int text = std::min(260, gfx::GetStringWidth(Text(), TextFont()));
    return gfx::Size(
        kPadding + kIconSize + kIconGap + text + kTextGap +
            PillWidth(L("archive.undo")) +
            (has_view_ ? kPillGap + PillWidth(L("archive.toast.view")) : 0) +
            kTrailing,
        kHeight);
  }

  gfx::Rect ViewRect() const {
    const gfx::Rect rect = glass_rect();
    const int width = has_view_ ? PillWidth(L("archive.toast.view")) : 0;
    return gfx::Rect(rect.right() - kTrailing - width,
                     rect.y() + (rect.height() - kPillHeight) / 2, width,
                     kPillHeight);
  }

  gfx::Rect UndoRect() const {
    const gfx::Rect view = ViewRect();
    const int width = PillWidth(L("archive.undo"));
    return gfx::Rect(view.x() - (has_view_ ? kPillGap : 0) - width, view.y(),
                     width, kPillHeight);
  }

  Part PartAt(const gfx::Point& point) const {
    if (!glass_rect().Contains(point)) {
      return Part::kNone;
    }
    if (UndoRect().Contains(point)) {
      return Part::kUndo;
    }
    if (has_view_ && ViewRect().Contains(point)) {
      return Part::kView;
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
        Text(), TextFont(), theme.on_surface,
        gfx::Rect(text_x, rect.y(),
                  std::max(0, UndoRect().x() - kTextGap - text_x),
                  rect.height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);

    const gfx::Rect undo = UndoRect();
    fill.setColor(
        WithAlpha(theme.accent, hovered_ == Part::kUndo ? 0.26f : 0.16f));
    canvas->DrawRoundRect(gfx::RectF(undo), kPillHeight / 2.0f, fill);
    canvas->DrawStringRectWithFlags(
        L("archive.undo"), PillFont(), theme.accent, undo,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    if (!has_view_) {
      return;
    }
    const gfx::Rect view = ViewRect();
    fill.setColor(theme.OnSurface(hovered_ == Part::kView ? 0.11f : 0.06f));
    canvas->DrawRoundRect(gfx::RectF(view), kPillHeight / 2.0f, fill);
    canvas->DrawStringRectWithFlags(
        L("archive.toast.view"), PillFont(), theme.on_surface, view,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void OnMouseEntered(const ui::MouseEvent& event) override {
    if (ArchiveNotice* notice = host_->Notice()) {
      notice->Hold(true);
    }
    OnMouseMoved(event);
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
    if (ArchiveNotice* notice = host_->Notice()) {
      notice->Hold(false);
    }
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
    ArchiveNotice* notice = host_->Notice();
    if (part == Part::kUndo) {
      if (notice) {
        notice->Undo();
      }
      return;
    }
    // 点提示条的别处、「查看」：看归档（整理进文件夹的只收起）。
    if (notice) {
      notice->Dismiss();
    }
    if (has_view_) {
      host_->ShowArchive();
    }
  }

 private:
  const raw_ptr<ArchiveToastHost> host_;
  std::u16string text_;
  const raw_ptr<const gfx::VectorIcon> icon_;
  const bool has_view_;
  Part hovered_ = Part::kNone;
  Part pressed_ = Part::kNone;
  LayerMotion motion_;
};

BEGIN_METADATA(ArchiveToast)
END_METADATA

ArchiveToastHost::ArchiveToastHost(BrowserWindowInterface* browser)
    : browser_(browser) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
}

ArchiveToastHost::~ArchiveToastHost() = default;

void ArchiveToastHost::SetTheme(const Theme& theme) {
  theme_ = theme;
  if (toast_) {
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    toast_->SetStyle(style);
    toast_->SchedulePaint();
  }
}

ArchiveNotice* ArchiveToastHost::Notice() const {
  return ArchiveNotice::From(browser_);
}

gfx::Rect ArchiveToastHost::ToastRect() const {
  return toast_ ? toast_->glass_rect() + toast_->bounds().OffsetFromOrigin()
                : gfx::Rect();
}

void ArchiveToastHost::ShowArchive() {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  SidebarView* sidebar = view ? view->sidebar() : nullptr;
  if (sidebar && !view->chrome_layout().tab_strip()) {
    if (sidebar->is_auto_hide()) {
      sidebar->Peek();
    }
    sidebar->ShowArchive(true);
    return;
  }
  OpenStarryArchive(browser_);
}

std::u16string ArchiveToastHost::TextForTesting() const {
  return toast_ ? toast_->Text() : std::u16string();
}

void ArchiveToastHost::AddedToWidget() {
  if (ArchiveNotice* notice = Notice()) {
    notice_subscription_ = notice->AddChangedCallback(
        base::BindRepeating(&ArchiveToastHost::Update, base::Unretained(this)));
  }
  Update();
}

void ArchiveToastHost::RemovedFromWidget() {
  notice_subscription_ = {};
}

void ArchiveToastHost::Layout(PassKey) {
  PlaceToast();
}

bool ArchiveToastHost::DoesIntersectRect(const views::View* target,
                                         const gfx::Rect& rect) const {
  return ToastRect().Intersects(rect);
}

void ArchiveToastHost::Update() {
  ArchiveNotice* notice = Notice();
  const bool show = notice && notice->shown();
  if (show && !toast_) {
    Show(notice);
  } else if (show) {
    std::u16string text = notice->ToastText();
    if (text != toast_->Text()) {
      toast_->SetText(std::move(text));
      PlaceToast();
      NotifyObstacles();
    }
  } else if (toast_) {
    Hide();
  }
}

void ArchiveToastHost::Show(ArchiveNotice* notice) {
  const bool organize = notice->kind() == ArchiveNotice::Kind::kOrganize;
  toast_ = AddChildView(std::make_unique<ArchiveToast>(
      this, notice->ToastText(),
      organize ? &icons::kFolder : &icons::kArchivebox, !organize));
  PlaceToast();
  // 从左边滑进来、稍微放大（同下载开始的提示条）。
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

void ArchiveToastHost::Hide() {
  ArchiveToast* toast = toast_;
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
          [](base::WeakPtr<ArchiveToastHost> host, ArchiveToast* toast) {
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

void ArchiveToastHost::PlaceToast() {
  if (!toast_) {
    return;
  }
  const gfx::Size size = toast_->ContentSize();
  toast_->SetGlassBounds(gfx::Rect(kInset, height() - kInset - size.height(),
                                   size.width(), size.height()));
}

void ArchiveToastHost::NotifyObstacles() {
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    view->MediaObstaclesChanged();
  }
}

BEGIN_METADATA(ArchiveToastHost)
END_METADATA

}  // namespace starry
