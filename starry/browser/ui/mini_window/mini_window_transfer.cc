// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/mini_window/mini_window_transfer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_image.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/viz/common/frame_timing_details.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace starry::mini_window {

namespace {

// 截图最多等这么久，等不到就用卡片的底色飞。
constexpr base::TimeDelta kSnapshotTimeout = base::Milliseconds(250);
// 过渡窗口画到屏幕上最多等这么久，再拿掉小窗口的网页。
constexpr base::TimeDelta kPresentTimeout = base::Milliseconds(100);
// 轮询等待网页挂载到主浏览器窗口并完成布局的间隔与次数上限。
constexpr base::TimeDelta kAttachPollInterval = base::Milliseconds(10);
constexpr int kAttachPollLimit = 12;
// 准备好以后一直没人来 FlyTo / Cancel（移到浏览器时最多等会话恢复 3 秒）：收掉。
constexpr base::TimeDelta kAbandonTimeout = base::Seconds(6);

Transfer* g_current = nullptr;

// 网页在屏幕上的位置。还没放进这个窗口、还没排版、窗口没显示时为空。
std::optional<gfx::Rect> PageRect(BrowserWindowInterface* browser,
                                  content::WebContents* contents) {
  BrowserView* const view = BrowserView::GetBrowserViewForBrowser(browser);
  views::Widget* const widget = view ? view->GetWidget() : nullptr;
  if (!widget || !widget->IsVisible() || !view->multi_contents_view()) {
    return std::nullopt;
  }
  widget->LayoutRootViewIfNecessary();
  ContentsContainerView* const container =
      view->multi_contents_view()->GetContentsContainerViewFor(contents);
  ContentsWebView* const web = container ? container->contents_view() : nullptr;
  if (!web || !web->IsDrawn() || web->bounds().IsEmpty()) {
    return std::nullopt;
  }
  return web->GetBoundsInScreen();
}

gfx::RectF Lerp(const gfx::RectF& from, const gfx::RectF& to, double t) {
  const auto mix = [t](float a, float b) {
    return static_cast<float>(a + (b - a) * t);
  };
  return gfx::RectF(mix(from.x(), to.x()), mix(from.y(), to.y()),
                    std::max(mix(from.width(), to.width()), 0.f),
                    std::max(mix(from.height(), to.height()), 0.f));
}

}  // namespace

// 过渡窗口中的网页截图视图：圆角卡片带阴影，截图按等比铺满裁剪（aspect fill）绘制。
class TransferView : public views::View {
  METADATA_HEADER(TransferView, views::View)

 public:
  TransferView(const SkBitmap& image, SkColor surface)
      : image_size_(image.width(), image.height()), surface_(surface) {
    if (!image.drawsNothing()) {
      image_ = cc::PaintImage::CreateFromBitmap(image);
    }
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }
  TransferView(const TransferView&) = delete;
  TransferView& operator=(const TransferView&) = delete;
  ~TransferView() override = default;

  const gfx::RectF& card() const { return card_; }

  // card 是这个视图的坐标。
  void SetCard(const gfx::RectF& card) {
    if (card == card_) {
      return;
    }
    gfx::Rect dirty = gfx::ToEnclosingRect(card_);
    dirty.Union(gfx::ToEnclosingRect(card));
    dirty.Inset(gfx::ShadowValue::GetMargin(Shadows()));
    card_ = card;
    SchedulePaintInRect(dirty);
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    if (card_.IsEmpty()) {
      return;
    }
    const float radius = radius::kContent;
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(surface_);
    fill.setLooper(gfx::CreateShadowDrawLooper(Shadows()));
    canvas->DrawRoundRect(card_, radius, fill);
    if (!image_ || image_size_.IsEmpty()) {
      return;
    }
    gfx::ScopedCanvas scoped_canvas(canvas);
    canvas->ClipPath(SkPath::RRect(SkRRect::MakeRectXY(
                         gfx::RectFToSkRect(card_), radius, radius)),
                     /*do_anti_alias=*/true);
    // 铺满卡片，居中裁掉多出来的（截图是像素，比例一样）。
    const float scale = std::max(card_.width() / image_size_.width(),
                                 card_.height() / image_size_.height());
    const float width = card_.width() / scale;
    const float height = card_.height() / scale;
    const SkRect source =
        SkRect::MakeXYWH((image_size_.width() - width) / 2,
                         (image_size_.height() - height) / 2, width, height);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    canvas->sk_canvas()->drawImageRect(
        image_, source, gfx::RectFToSkRect(card_),
        SkSamplingOptions(SkFilterMode::kLinear), &flags,
        SkCanvas::kFast_SrcRectConstraint);
  }

 private:
  // 黑色 28% 不透明度阴影，模糊直径 48 DIP，向下偏移 8 DIP。
  static gfx::ShadowValues Shadows() {
    return {gfx::ShadowValue(gfx::Vector2d(0, 8), 48,
                             SkColorSetA(SK_ColorBLACK, 0x47))};
  }

  cc::PaintImage image_;
  const gfx::Size image_size_;
  const SkColor surface_;
  gfx::RectF card_;
};

BEGIN_METADATA(TransferView)
END_METADATA

// static
void Transfer::Prepare(BrowserWindowInterface* mini, ReadyCallback ready) {
  content::WebContents* const contents =
      mini->GetTabStripModel()->GetActiveWebContents();
  if (motion::ReduceMotion() || !contents || !PageRect(mini, contents)) {
    std::move(ready).Run(nullptr);
    return;
  }
  auto snapshotted = base::BindOnce(&Transfer::OnSnapshot, mini->GetWeakPtr(),
                                    contents->GetWeakPtr(), std::move(ready));
  content::RenderWidgetHostView* const view =
      contents->GetRenderWidgetHostView();
  if (!view || contents->IsCrashed()) {
    std::move(snapshotted).Run(SkBitmap());
    return;
  }
  view->CopyFromSurface(gfx::Rect(), gfx::Size(), kSnapshotTimeout,
                        base::BindOnce(
                            [](base::OnceCallback<void(const SkBitmap&)> done,
                               const content::CopyFromSurfaceResult& result) {
                              std::move(done).Run(result.has_value()
                                                      ? result->bitmap
                                                      : SkBitmap());
                            },
                            std::move(snapshotted)));
}

// static
void Transfer::OnSnapshot(base::WeakPtr<BrowserWindowInterface> mini,
                          base::WeakPtr<content::WebContents> contents,
                          ReadyCallback ready,
                          const SkBitmap& image) {
  // 等截图的时候小窗口可能关了、动过。
  StarryBrowserView* const view =
      mini ? StarryBrowserView::From(mini.get()) : nullptr;
  const std::optional<gfx::Rect> page =
      view && contents ? PageRect(mini.get(), contents.get()) : std::nullopt;
  if (!page) {
    std::move(ready).Run(nullptr);
    return;
  }
  const display::Display display =
      display::Screen::Get()->GetDisplayNearestWindow(
          view->GetWidget()->GetNativeWindow());
  // 自己管自己，Close 里删掉。
  Transfer* const transfer =
      new Transfer(image, *page, view->theme().surface_panel, display);
  transfer->Show(std::move(ready));
}

Transfer::Transfer(const SkBitmap& image,
                   const gfx::Rect& page,
                   SkColor surface,
                   const display::Display& display)
    : display_id_(display.id()),
      origin_(display.bounds().origin()),
      from_(page),
      to_(page),
      flight_(
          0,
          base::BindRepeating(&Transfer::StepFlight, base::Unretained(this))),
      fade_(1,
            base::BindRepeating(&Transfer::StepFade, base::Unretained(this))) {
  if (g_current) {
    g_current->Finish();
  }
  g_current = this;
  widget_ = std::make_unique<views::Widget>();
  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW_FRAMELESS);
  params.bounds = display.bounds();
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
  params.activatable = views::Widget::InitParams::Activatable::kNo;
  params.accept_events = false;
  // 盖在浏览器窗口上面：它接下来会被拿到最前面。
  params.z_order = ui::ZOrderLevel::kFloatingUIElement;
  params.name = "StarryMiniWindowTransfer";
  widget_->Init(std::move(params));
  widget_->SetVisibilityAnimationTransition(views::Widget::ANIMATE_NONE);
  view_ =
      widget_->SetContentsView(std::make_unique<TransferView>(image, surface));
  StepFlight();
}

Transfer::~Transfer() {
  if (g_current == this) {
    g_current = nullptr;
  }
}

void Transfer::Show(ReadyCallback ready) {
  ready_ = std::move(ready);
  widget_->ShowInactive();
  // 截图画到屏幕上之后再拿掉小窗口的网页，不然中间会闪一下。
  widget_->GetCompositor()->RequestSuccessfulPresentationTimeForNextFrame(
      base::BindOnce(
          [](base::WeakPtr<Transfer> transfer,
             const viz::FrameTimingDetails& details) {
            if (transfer) {
              transfer->RunReady();
            }
          },
          weak_factory_.GetWeakPtr()));
  present_timer_.Start(
      FROM_HERE, kPresentTimeout,
      base::BindOnce(&Transfer::RunReady, base::Unretained(this)));
  timer_.Start(FROM_HERE, kAbandonTimeout,
               base::BindOnce(&Transfer::Finish, base::Unretained(this)));
}

void Transfer::RunReady() {
  present_timer_.Stop();
  if (ready_) {
    std::move(ready_).Run(weak_factory_.GetWeakPtr());
  }
}

void Transfer::FlyTo(BrowserWindowInterface* target,
                     content::WebContents* contents) {
  if (finishing_) {
    return;
  }
  timer_.Stop();
  target_ = target->GetWeakPtr();
  contents_ = contents->GetWeakPtr();
  polls_ = 0;
  poll_timer_.Start(FROM_HERE, kAttachPollInterval, this,
                    &Transfer::PollDestination);
}

void Transfer::Cancel() {
  Finish();
}

void Transfer::PollDestination() {
  std::optional<gfx::Rect> destination;
  if (target_ && contents_) {
    destination = PageRect(target_.get(), contents_.get());
  }
  if (!destination && target_ && contents_ && ++polls_ < kAttachPollLimit) {
    return;
  }
  poll_timer_.Stop();
  BrowserView* const view =
      target_ ? BrowserView::GetBrowserViewForBrowser(target_.get()) : nullptr;
  views::Widget* const window = view ? view->GetWidget() : nullptr;
  if (!window || !window->IsVisible() || window->IsMinimized() ||
      display::Screen::Get()
              ->GetDisplayNearestWindow(window->GetNativeWindow())
              .id() != display_id_) {
    Finish();
    return;
  }
  // 网页还没放好：落在窗口里面。
  gfx::Rect fallback = window->GetWindowBoundsInScreen();
  fallback.Inset(metrics::kContentInset);
  Animate(destination.value_or(fallback));
}

void Transfer::Animate(const gfx::Rect& destination) {
  to_ = gfx::RectF(destination);
  flight_.AnimateTo(1, MotionCurve::Of(motion::kMiniTransfer));
  timer_.Start(FROM_HERE, base::Seconds(motion::kMiniTransferFlight),
               base::BindOnce(&Transfer::Finish, base::Unretained(this)));
}

void Transfer::StepFlight() {
  gfx::RectF card = Lerp(from_, to_, flight_.value());
  card.Offset(-origin_.x(), -origin_.y());
  view_->SetCard(card);
}

void Transfer::Finish() {
  if (finishing_) {
    return;
  }
  finishing_ = true;
  poll_timer_.Stop();
  timer_.Stop();
  RunReady();
  // 标准缓动淡出曲线。
  fade_.AnimateTo(0, MotionCurve::Timed(motion::kMiniTransferFadeOut, 0.25f,
                                        0.1f, 0.25f, 1));
}

void Transfer::StepFade() {
  view_->layer()->SetOpacity(static_cast<float>(fade_.value()));
  if (!fade_.is_animating() && fade_.value() == 0) {
    // 在 fade_ 自己的回调里，删掉自己要等这一轮走完。
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&Transfer::Close, weak_factory_.GetWeakPtr()));
  }
}

void Transfer::Close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  widget_->Hide();
  if (g_current == this) {
    g_current = nullptr;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
}

// static
views::Widget* Transfer::CurrentWidget() {
  return g_current ? g_current->widget_.get() : nullptr;
}

// static
std::optional<gfx::RectF> Transfer::CurrentCard() {
  if (!g_current) {
    return std::nullopt;
  }
  gfx::RectF card = g_current->view_->card();
  card.Offset(g_current->origin_.x(), g_current->origin_.y());
  return card;
}

// static
float Transfer::CurrentOpacity() {
  return g_current ? static_cast<float>(g_current->fade_.value()) : 0;
}

}  // namespace starry::mini_window
