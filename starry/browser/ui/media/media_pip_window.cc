// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_pip_window.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/media/media_pip.h"
#include "starry/browser/ui/media/media_video_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/hit_test.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/frame_view.h"

namespace starry {

namespace {

// 边缘这么宽是缩放（macOS 上系统自己也管）。
constexpr int kResizeBorder = 8;
constexpr int kResizeCorner = 16;
// 离工作区的边。
constexpr int kScreenMargin = 16;
// 最小：短边 120；最大：工作区的一半。
constexpr int kMinShortSide = 120;

// 这次运行里挪过、缩放过的位置（屏幕坐标）。
std::optional<gfx::Rect>& LastBounds() {
  static std::optional<gfx::Rect> bounds;
  return bounds;
}

gfx::Rect Lerp(const gfx::Rect& from, const gfx::Rect& to, double t) {
  const auto mix = [t](int a, int b) {
    return static_cast<int>(std::lround(a + (b - a) * t));
  };
  return gfx::Rect(mix(from.x(), to.x()), mix(from.y(), to.y()),
                   std::max(1, mix(from.width(), to.width())),
                   std::max(1, mix(from.height(), to.height())));
}

// 整块是画面：按钮上是按钮，边缘缩放，别处拖动窗口。
class PipFrameView : public views::FrameView {
  METADATA_HEADER(PipFrameView, views::FrameView)

 public:
  explicit PipFrameView(MediaPipWindow* window) : window_(window) {}

  // views::FrameView:
  gfx::Rect GetBoundsForClientView() const override { return bounds(); }
  gfx::Rect GetWindowBoundsForClientBounds(
      const gfx::Rect& client_bounds) const override {
    return bounds();
  }
  int NonClientHitTest(const gfx::Point& point) override {
    if (!bounds().Contains(point)) {
      return HTNOWHERE;
    }
    const int component = GetHTComponentForFrame(
        point, gfx::Insets(kResizeBorder), kResizeCorner, kResizeCorner,
        GetWidget()->widget_delegate()->CanResize());
    if (component != HTNOWHERE) {
      return component;
    }
    if (MediaVideoView* video = window_->video()) {
      gfx::Point local = point;
      views::View::ConvertPointToTarget(this, video, &local);
      if (video->HitsControl(local)) {
        return HTCLIENT;
      }
    }
    return HTCAPTION;
  }

 private:
  const raw_ptr<MediaPipWindow> window_;
};

BEGIN_METADATA(PipFrameView)
END_METADATA

}  // namespace

// 窗口本身：鼠标进出（控件）、空格、大小限制、移动缩放和被系统关掉，转给
// MediaPipWindow。
class MediaPipWindow::PipWidget : public views::Widget {
 public:
  explicit PipWidget(MediaPipWindow* owner) : owner_(owner) {}

  // views::Widget:
  gfx::Size GetMinimumSize() const override { return owner_->MinimumSize(); }
  gfx::Size GetMaximumSize() const override { return owner_->MaximumSize(); }
  void OnNativeWidgetMove() override {
    views::Widget::OnNativeWidgetMove();
    owner_->OnBoundsChanged();
  }
  void OnNativeWidgetSizeChanged(const gfx::Size& new_size) override {
    views::Widget::OnNativeWidgetSizeChanged(new_size);
    owner_->OnBoundsChanged();
  }
  void OnNativeWidgetDestroyed() override {
    views::Widget::OnNativeWidgetDestroyed();
    owner_->OnNativeClosed();
  }
  void OnMouseEvent(ui::MouseEvent* event) override {
    if (MediaVideoView* video = owner_->video()) {
      if (event->type() == ui::EventType::kMouseEntered ||
          event->type() == ui::EventType::kMouseMoved) {
        video->SetHovered(true);
      } else if (event->type() == ui::EventType::kMouseExited) {
        video->SetHovered(false);
      }
    }
    views::Widget::OnMouseEvent(event);
  }
  void OnKeyEvent(ui::KeyEvent* event) override {
    // 空格：播放、暂停（同 Chrome 的画中画）。
    if (event->type() == ui::EventType::kKeyPressed &&
        event->key_code() == ui::VKEY_SPACE && owner_->pip_) {
      owner_->pip_->TogglePlayPause();
      event->SetHandled();
      return;
    }
    views::Widget::OnKeyEvent(event);
  }

 private:
  const raw_ptr<MediaPipWindow> owner_;
};

MediaPipWindow::MediaPipWindow(base::WeakPtr<MediaPip> pip)
    : pip_(pip),
      flight_(1,
              base::BindRepeating(&MediaPipWindow::OnFlightStep,
                                  base::Unretained(this))) {
  SetCanResize(true);
  SetModalType(ui::mojom::ModalType::kNone);
  // 不显示，但窗口切换器里用它认这个窗口。
  SetShowTitle(false);
  SetTitle(L("media.pip"));
  // 内容放在 delegate 上（ClientView 包着它）；直接 Widget::SetContentsView
  // 会把窗框 （NonClientView）换掉。
  video_ = SetContentsView(
      std::make_unique<MediaVideoView>(pip, MediaVideoView::Place::kDesktop));

  widget_ = std::make_unique<PipWidget>(this);
  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW);
  params.bounds = gfx::Rect(320, 180);
  params.z_order = ui::ZOrderLevel::kFloatingWindow;
  params.visible_on_all_workspaces = true;
  params.remove_standard_frame = true;
  params.name = "StarryPictureInPicture";
  params.layer_type = ui::LAYER_NOT_DRAWN;
  params.delegate = this;
#if !BUILDFLAG(IS_WIN)
  // Windows 上能缩放的窗口不能是透明的（同 Chrome 的画中画）。
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
#endif
#if BUILDFLAG(IS_MAC)
  params.native_widget = CreateMediaPipNativeWidget(widget_.get());
#endif
  widget_->Init(std::move(params));
  widget_->GetRootView()->SetPaintToLayer(ui::LAYER_TEXTURED);
  widget_->GetRootView()->layer()->SetMasksToBounds(true);
  widget_->GetRootView()->layer()->SetFillsBoundsOpaquely(false);
#if !BUILDFLAG(IS_WIN)
  // 窗口的圆角（自己裁，不靠系统的窗框）。
  constexpr float kWindowRadius = 12;
  widget_->GetRootView()->layer()->SetRoundedCornerRadius(
      gfx::RoundedCornersF(kWindowRadius));
  widget_->GetRootView()->layer()->SetIsFastRoundedCorner(true);
#endif
}

MediaPipWindow::~MediaPipWindow() {
  closing_ = true;
  video_ = nullptr;
  widget_.reset();
}

std::unique_ptr<views::FrameView> MediaPipWindow::CreateFrameView(
    views::Widget* widget) {
  return std::make_unique<PipFrameView>(this);
}

gfx::Rect MediaPipWindow::WorkArea() const {
  display::Screen* screen = display::Screen::Get();
  if (LastBounds()) {
    return screen->GetDisplayMatching(*LastBounds()).work_area();
  }
  content::WebContents* contents = pip_ ? pip_->web_contents() : nullptr;
  if (contents) {
    return screen->GetDisplayNearestWindow(contents->GetTopLevelNativeWindow())
        .work_area();
  }
  return screen->GetPrimaryDisplay().work_area();
}

gfx::Size MediaPipWindow::MinimumSize() const {
  const gfx::Size natural = pip_ ? pip_->natural_size() : gfx::Size();
  if (natural.IsEmpty()) {
    return gfx::Size(kMinShortSide * 16 / 9, kMinShortSide);
  }
  const double ratio = static_cast<double>(natural.width()) / natural.height();
  return ratio >= 1
             ? gfx::Size(static_cast<int>(std::lround(kMinShortSide * ratio)),
                         kMinShortSide)
             : gfx::Size(kMinShortSide,
                         static_cast<int>(std::lround(kMinShortSide / ratio)));
}

gfx::Size MediaPipWindow::MaximumSize() const {
  const gfx::Rect work = WorkArea();
  return gfx::Size(work.width() / 2, work.height() / 2);
}

gfx::Rect MediaPipWindow::TargetBounds() const {
  const gfx::Rect work = WorkArea();
  const gfx::Size natural = pip_ ? pip_->natural_size() : gfx::Size(16, 9);
  const double ratio =
      natural.IsEmpty()
          ? 16.0 / 9
          : static_cast<double>(natural.width()) / natural.height();
  // 记着的大小按面积换成新的比例；没有时宽是工作区的五分之一。
  double area = 0;
  if (LastBounds()) {
    area = static_cast<double>(LastBounds()->width()) * LastBounds()->height();
  } else {
    const double width = std::clamp(work.width() / 5.0, 280.0, 480.0);
    area = width * width / (16.0 / 9);
  }
  double width = std::sqrt(area * ratio);
  double height = width / ratio;
  const gfx::Size min = MinimumSize();
  const gfx::Size max = MaximumSize();
  const double fit =
      std::min({1.0, max.width() / width, max.height() / height});
  width *= fit;
  height *= fit;
  if (width < min.width() || height < min.height()) {
    width = min.width();
    height = min.height();
  }
  gfx::Rect bounds(0, 0, static_cast<int>(std::lround(width)),
                   static_cast<int>(std::lround(height)));
  if (LastBounds()) {
    // 右下角不动（窗口多半停在右下角）。
    bounds.set_origin(gfx::Point(LastBounds()->right() - bounds.width(),
                                 LastBounds()->bottom() - bounds.height()));
  } else {
    bounds.set_origin(
        gfx::Point(work.right() - kScreenMargin - bounds.width(),
                   work.bottom() - kScreenMargin - bounds.height()));
  }
  bounds.AdjustToFit(work);
  return bounds;
}

void MediaPipWindow::ShowWindow(const std::optional<gfx::Rect>& from) {
  const gfx::Rect target = TargetBounds();
  if (pip_ && !pip_->natural_size().IsEmpty()) {
    widget_->SetAspectRatio(gfx::SizeF(pip_->natural_size()));
  }
  if (from && !from->IsEmpty() && !motion::ReduceMotion()) {
    flight_from_ = *from;
    flight_to_ = target;
    flying_ = true;
    widget_->SetBounds(*from);
    widget_->SetOpacity(0);
    widget_->ShowInactive();
    flight_.SetValue(0);
    flight_.AnimateTo(1, MotionCurve::Of(motion::kMediaPipFlight));
  } else {
    flying_ = false;
    widget_->SetBounds(target);
    widget_->SetOpacity(1);
    widget_->ShowInactive();
  }
  video_->Update();
}

void MediaPipWindow::Hide() {
  flight_.SetValue(1);
  flying_ = false;
  widget_->Hide();
}

views::Widget* MediaPipWindow::widget() const {
  return widget_.get();
}

bool MediaPipWindow::IsVisible() const {
  return widget_ && widget_->IsVisible();
}

bool MediaPipWindow::IsActive() const {
  return widget_ && widget_->IsActive();
}

gfx::Rect MediaPipWindow::GetBoundsInScreen() const {
  return widget_ ? widget_->GetWindowBoundsInScreen() : gfx::Rect();
}

void MediaPipWindow::OnFlightStep() {
  const double t = flight_.value();
  widget_->SetBounds(Lerp(flight_from_, flight_to_, t));
  widget_->SetOpacity(static_cast<float>(std::clamp(t / 0.3, 0.0, 1.0)));
  if (!flight_.is_animating()) {
    flying_ = false;
    widget_->SetBounds(flight_to_);
    widget_->SetOpacity(1);
    if (pip_) {
      pip_->DesktopBoundsChanged();
    }
  }
}

void MediaPipWindow::UpdateNaturalSize() {
  if (!pip_ || pip_->natural_size().IsEmpty()) {
    return;
  }
  widget_->SetAspectRatio(gfx::SizeF(pip_->natural_size()));
  if (!flying_ && widget_->IsVisible()) {
    widget_->SetBounds(TargetBounds());
  }
  video_->Update();
}

void MediaPipWindow::OnBoundsChanged() {
  if (!flying_ && widget_ && widget_->IsVisible()) {
    LastBounds() = widget_->GetWindowBoundsInScreen();
    if (pip_) {
      pip_->DesktopBoundsChanged();
    }
  }
}

void MediaPipWindow::OnNativeClosed() {
  video_ = nullptr;
  if (!closing_ && pip_) {
    pip_->OnDesktopWindowClosed();
  }
}

}  // namespace starry
