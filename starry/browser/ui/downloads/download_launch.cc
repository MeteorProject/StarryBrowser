// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/downloads/download_launch.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/icon_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/downloads/download_info.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "starry/browser/ui/sidebar/utility_buttons.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/display/screen.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 先在起点弹出、回到原大小，再飞向按钮。
constexpr double kPop = 0.2;
constexpr double kSettle = 0.08;
constexpr double kFlight = 0.58;
constexpr double kTotal = kPop + kSettle + kFlight;
constexpr double kFade = 0.1;
constexpr double kArrival = 0.5;
constexpr int kFlightIcon = 56;
constexpr int kFlightBox = 80;
constexpr base::TimeDelta kToastShown = base::Seconds(3.5);
constexpr base::TimeDelta kToastAfterHover = base::Seconds(1.5);
// 弹性回弹曲线。
constexpr Spring kBouncy{0.5, 0.7};

gfx::PointF Bezier(const gfx::PointF& from,
                   const gfx::PointF& control,
                   const gfx::PointF& to,
                   float t) {
  const float u = 1 - t;
  return gfx::PointF(u * u * from.x() + 2 * u * t * control.x() + t * t * to.x(),
                     u * u * from.y() + 2 * u * t * control.y() + t * t * to.y());
}

// 按文件名取系统的文件图标（只看扩展名，文件还不存在也行）。
void LoadFileIcon(const base::FilePath& path,
                  IconLoader::IconSize size,
                  float scale,
                  base::CancelableTaskTracker* tracker,
                  base::OnceCallback<void(gfx::Image)> done) {
  if (path.empty()) {
    return;
  }
  IconManager* manager = g_browser_process->icon_manager();
  if (gfx::Image* cached = manager->LookupIconFromFilepath(path, size, scale)) {
    std::move(done).Run(*cached);
    return;
  }
  manager->LoadIcon(path, size, scale, std::move(done), tracker);
}

void PaintFileIcon(gfx::Canvas* canvas,
                   const gfx::Image& icon,
                   const gfx::Rect& rect,
                   const Theme& theme) {
  if (!icon.IsEmpty()) {
    const gfx::ImageSkia& image = *icon.ToImageSkia();
    canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), rect.x(),
                         rect.y(), rect.width(), rect.height(),
                         /*filter=*/true);
    return;
  }
  // 还没取到系统图标：一张白纸。
  gfx::RectF doc(rect);
  doc.Inset(gfx::InsetsF::VH(rect.height() / 10.0f, rect.width() / 5.0f));
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(SK_ColorWHITE);
  canvas->DrawRoundRect(doc, 3, fill);
  cc::PaintFlags line;
  line.setAntiAlias(true);
  line.setStyle(cc::PaintFlags::kStroke_Style);
  line.setStrokeWidth(1);
  line.setColor(theme.OnSurface(0.25f));
  canvas->DrawRoundRect(doc, 3, line);
}

}  // namespace

// ---- 飞行的图标 ----

class DownloadFlight : public views::View, public gfx::AnimationDelegate {
  METADATA_HEADER(DownloadFlight, views::View)

 public:
  DownloadFlight(DownloadLaunchView* host,
                 const gfx::PointF& from,
                 const base::FilePath& icon_path)
      : host_(host), from_(from), animation_(this) {
    SetCanProcessEventsWithinSubtree(false);
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetBounds(0, 0, kFlightBox, kFlightBox);
    LoadFileIcon(icon_path, IconLoader::NORMAL, 2.0f, &tracker_,
                 base::BindOnce(
                     [](base::WeakPtr<DownloadFlight> flight, gfx::Image image) {
                       if (flight) {
                         flight->icon_ = std::move(image);
                         flight->SchedulePaint();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
    to_ = host->ButtonCenter().value_or(from);
    animation_.SetDuration(base::Seconds(kTotal));
    animation_.Start();
    Step(0);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::Rect rect((kFlightBox - kFlightIcon) / 2,
                         (kFlightBox - kFlightIcon) / 2, kFlightIcon,
                         kFlightIcon);
    cc::PaintFlags shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(SK_ColorTRANSPARENT);
    shadow.setLooper(gfx::CreateShadowDrawLooper(
        {gfx::ShadowValue(gfx::Vector2d(0, 6), 12, SkColorSetA(SK_ColorBLACK, 64))}));
    gfx::RectF body(rect);
    body.Inset(gfx::InsetsF::VH(6, 10));
    canvas->DrawRoundRect(body, 4, shadow);
    PaintFileIcon(canvas, icon_, rect, host_->theme());
  }

  // gfx::AnimationDelegate:
  void AnimationProgressed(const gfx::Animation* animation) override {
    Step(animation_.GetCurrentValue() * kTotal);
  }
  void AnimationEnded(const gfx::Animation* animation) override {
    Step(kTotal);
    host_->Land(this);
  }

 private:
  // 计算 t 秒时的缩放、透明度和弧线位置。
  void Step(double t) {
    // 大小：弹到 1.12（带回弹），回到 1，一路缩到 0.3。
    const MotionCurve pop = MotionCurve::Of(kBouncy);
    const double popped = pop.ValueAt(0.4, 1.12, 0, kPop);
    double scale = 1;
    if (t < kPop) {
      scale = pop.ValueAt(0.4, 1.12, 0, t);
    } else if (t < kPop + kSettle) {
      scale = MotionCurve::EaseInOut(kSettle).ValueAt(popped, 1, 0, t - kPop);
    } else {
      scale = MotionCurve::EaseInOut(kFlight)
                  .ValueAt(1, 0.3, 0, t - kPop - kSettle);
    }
    // 不透明度：很快显出来，到了按钮再淡掉。
    double opacity = 1;
    if (t < kPop * 0.6) {
      opacity = t / (kPop * 0.6);
    } else if (t > kTotal - kFade) {
      opacity = std::max(0.0, 1 - (t - (kTotal - kFade)) / kFade);
    }
    // 沿弧线走了多少：先停在起点，再飞过去。
    const double lead = kPop + kSettle;
    const float progress =
        t < lead ? 0
                 : static_cast<float>(
                       MotionCurve::EaseInOut(kFlight).ValueAt(0, 1, 0, t - lead));
    // 按钮可能在动（侧栏在滑）：每一帧重新取终点。
    if (std::optional<gfx::PointF> center = host_->ButtonCenter()) {
      to_ = *center;
    }
    // 弧线：控制点在起点到终点前三分之一处、比两端都高，先往上抛再落进按钮；不超出窗口顶上。
    const float distance = (to_ - from_).Length();
    const float lift = std::clamp(distance * 0.35f, 60.0f, 180.0f);
    const gfx::PointF control(
        from_.x() + (to_.x() - from_.x()) * 0.35f,
        std::max(std::min(from_.y(), to_.y()) - lift, 24.0f));
    const gfx::PointF point = Bezier(from_, control, to_, progress);
    // 向左飞时往左歪，向右飞时往右歪。
    const float tilt = to_.x() < from_.x() ? -14 : 14;

    gfx::Transform transform;
    transform.Translate(point.x() - kFlightBox / 2.0f,
                        point.y() - kFlightBox / 2.0f);
    transform.Translate(kFlightBox / 2.0f, kFlightBox / 2.0f);
    transform.Rotate(tilt * progress);
    transform.Scale(scale, scale);
    transform.Translate(-kFlightBox / 2.0f, -kFlightBox / 2.0f);
    layer()->SetTransform(transform);
    layer()->SetOpacity(static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
  }

  const raw_ptr<DownloadLaunchView> host_;
  const gfx::PointF from_;
  gfx::PointF to_;
  gfx::Image icon_;
  gfx::LinearAnimation animation_;
  base::CancelableTaskTracker tracker_;
  base::WeakPtrFactory<DownloadFlight> weak_factory_{this};
};

BEGIN_METADATA(DownloadFlight)
END_METADATA

// ---- 按钮接住时扩散的一圈 ----

class ArrivalPulse : public views::View {
  METADATA_HEADER(ArrivalPulse, views::View)

 public:
  explicit ArrivalPulse(const Theme& theme) : theme_(theme) {
    SetCanProcessEventsWithinSubtree(false);
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  void Start(base::OnceClosure done) {
    const bool still = motion::ReduceMotion();
    const gfx::PointF center(width() / 2.0f, height() / 2.0f);
    LayerPose from;
    from.scale = still ? 1 : 0.9f;
    LayerPose to;
    to.scale = still ? 1 : 1.9f;
    motion_.AnimateTransformFrom(layer(), from, to,
                                 MotionCurve::EaseOut(kArrival), center);
    motion_.AnimateOpacityFrom(layer(), 0.8f, 0, MotionCurve::EaseOut(kArrival),
                               std::move(done));
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags ring;
    ring.setAntiAlias(true);
    ring.setStyle(cc::PaintFlags::kStroke_Style);
    ring.setStrokeWidth(2);
    ring.setColor(theme_.accent);
    canvas->DrawCircle(gfx::PointF(width() / 2.0f, height() / 2.0f), 13, ring);
  }

 private:
  Theme theme_;
  LayerMotion motion_;
};

BEGIN_METADATA(ArrivalPulse)
END_METADATA

// ---- 侧栏收着时的提示条 ----

class DownloadToast : public GlassView {
  METADATA_HEADER(DownloadToast, GlassView)

 public:
  DownloadToast(DownloadLaunchView* host,
                const std::string& guid,
                const std::u16string& filename,
                const base::FilePath& icon_path)
      : host_(host), guid_(guid), filename_(filename) {
    const Theme& theme = host->theme();
    SetCornerRadius(radius::kCard, true);
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    SetTooltipText(L("downloads.toast.show.help"));
    LoadFileIcon(icon_path, IconLoader::NORMAL, 2.0f, &tracker_,
                 base::BindOnce(
                     [](base::WeakPtr<DownloadToast> toast, gfx::Image image) {
                       if (toast) {
                         toast->icon_ = std::move(image);
                         toast->SchedulePaint();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
  }

  const std::string& guid() const { return guid_; }
  LayerMotion& motion() { return motion_; }
  std::u16string Text() const {
    return base::StrCat({finished_ ? L("downloads.toast.finished")
                                   : L("downloads.toast.started"),
                         u" ", filename_});
  }

  void Update(Download* item) {
    // Chrome 还没定下文件名时还是网址里的名字。
    if (!item->TargetPath().empty()) {
      filename_ = item->Filename();
    }
    const DownloadState state = item->State();
    active_ = IsActive(state);
    finished_ = state == DownloadState::kComplete;
    progress_ = DownloadProgress(*item);
    SchedulePaint();
  }

  // 内容的大小：左 10、右 14、上下 8；图标区 34，间距 10；文件名最宽 240。
  gfx::Size ContentSize() const {
    const int text = std::min(
        240,
        std::max({gfx::GetStringWidth(filename_, NameFont()),
                  gfx::GetStringWidth(L("downloads.toast.started"), LabelFont()),
                  gfx::GetStringWidth(L("downloads.toast.finished"),
                                      LabelFont())}));
    return gfx::Size(10 + 34 + 10 + text + 10 + 10 + 14, 8 + 34 + 8);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    GlassView::OnPaint(canvas);
    const Theme& theme = host_->theme();
    const gfx::Rect rect = glass_rect();
    // 面板底色和细边。
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(theme.surface_panel,
                              theme.is_dark ? 0.66f * 255 : 0.76f * 255));
    canvas->DrawRoundRect(gfx::RectF(rect), radius::kCard, fill);
    if (hovered_ || pressed_) {
      fill.setColor(theme.OnSurface(pressed_ ? 0.08f : 0.04f));
      canvas->DrawRoundRect(gfx::RectF(rect), radius::kCard, fill);
    }
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme.OnSurface(0.1f));
    gfx::RectF stroke(rect);
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius::kCard - 0.5f, border);

    // 图标（下载中时外面一圈进度）。
    const gfx::PointF center(rect.x() + 10 + 17, rect.y() + rect.height() / 2.0f);
    if (active_) {
      cc::PaintFlags track;
      track.setAntiAlias(true);
      track.setStyle(cc::PaintFlags::kStroke_Style);
      track.setStrokeWidth(2);
      track.setColor(theme.OnSurface(0.12f));
      canvas->DrawCircle(center, 16, track);
      cc::PaintFlags arc = track;
      arc.setStrokeCap(cc::PaintFlags::kRound_Cap);
      arc.setColor(theme.accent);
      SkPathBuilder path;
      path.addArc(SkRect::MakeLTRB(center.x() - 16, center.y() - 16,
                                   center.x() + 16, center.y() + 16),
                  -90, 360 * std::max(0.02, progress_.value_or(0.02)));
      canvas->DrawPath(path.detach(), arc);
    }
    const int icon_size = active_ ? 22 : 28;
    PaintFileIcon(canvas, icon_,
                  gfx::Rect(static_cast<int>(center.x()) - icon_size / 2,
                            static_cast<int>(center.y()) - icon_size / 2,
                            icon_size, icon_size),
                  theme);

    // 「开始下载」11 粗、文件名 12.5 中等，右边一个 › 。
    const int text_x = rect.x() + 10 + 34 + 10;
    const int text_width = rect.right() - 14 - 10 - 10 - text_x;
    const int block = LabelFont().GetHeight() + 1 + NameFont().GetHeight();
    int y = rect.y() + (rect.height() - block) / 2;
    canvas->DrawStringRectWithFlags(
        finished_ ? L("downloads.toast.finished")
                  : L("downloads.toast.started"),
        LabelFont(),
        theme.on_surface_variant,
        gfx::Rect(text_x, y, text_width, LabelFont().GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    y += LabelFont().GetHeight() + 1;
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(filename_, NameFont(), text_width, gfx::ELIDE_MIDDLE),
        NameFont(), theme.on_surface,
        gfx::Rect(text_x, y, text_width, NameFont().GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    canvas->Save();
    canvas->Translate(gfx::Vector2d(rect.right() - 14 - 10 + (hovered_ ? 2 : 0),
                                    rect.y() + (rect.height() - 10) / 2));
    gfx::PaintVectorIcon(canvas, icons::kChevronRight, 10,
                         theme.on_surface_variant);
    canvas->Restore();
  }

  void OnMouseEntered(const ui::MouseEvent& event) override {
    hovered_ = true;
    host_->HoldToast(true);
    SchedulePaint();
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_ = false;
    pressed_ = false;
    host_->HoldToast(false);
    SchedulePaint();
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    pressed_ = event.IsLeftMouseButton();
    SchedulePaint();
    return pressed_;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    const bool clicked = pressed_ && HitTestPoint(event.location());
    pressed_ = false;
    SchedulePaint();
    if (!clicked) {
      return;
    }
    // 点一下：收起提示、滑出侧栏、打开下载列表。
    BrowserWindowInterface* browser = host_->browser();
    host_->DismissToast();
    if (StarryBrowserView* view = StarryBrowserView::From(browser)) {
      view->ToggleDownloadsPanel();
    }
  }

 private:
  static const gfx::FontList& LabelFont() {
    return UiFont(11, gfx::Font::Weight::SEMIBOLD);
  }
  static const gfx::FontList& NameFont() {
    return UiFont(12, gfx::Font::Weight::MEDIUM);
  }

  const raw_ptr<DownloadLaunchView> host_;
  const std::string guid_;
  std::u16string filename_;
  bool active_ = true;
  bool finished_ = false;
  std::optional<double> progress_;
  bool hovered_ = false;
  bool pressed_ = false;
  gfx::Image icon_;
  LayerMotion motion_;
  base::CancelableTaskTracker tracker_;
  base::WeakPtrFactory<DownloadToast> weak_factory_{this};
};

BEGIN_METADATA(DownloadToast)
END_METADATA

// ---- 这一层 ----

DownloadLaunchView::DownloadLaunchView(BrowserWindowInterface* browser)
    : browser_(browser) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  if (DownloadService* service = DownloadService::Get(browser->GetProfile())) {
    service_ = service;
    observation_.Observe(service);
  }
}

DownloadLaunchView::~DownloadLaunchView() = default;

void DownloadLaunchView::SetTheme(const Theme& theme) {
  theme_ = theme;
  if (toast_) {
    toast_->SchedulePaint();
  }
}

bool DownloadLaunchView::DoesIntersectRect(const views::View* target,
                                           const gfx::Rect& rect) const {
  return toast_ && toast_->glass_bounds().Intersects(rect);
}

std::optional<gfx::PointF> DownloadLaunchView::ButtonCenter() const {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  UtilityButtons* buttons = view ? view->active_utility_buttons() : nullptr;
  if (!buttons) {
    return std::nullopt;
  }
  const gfx::Rect bounds = buttons->DownloadsButtonBounds();
  if (bounds.IsEmpty() || !buttons->IsDrawn()) {
    return std::nullopt;
  }
  return gfx::PointF(
      views::View::ConvertRectToTarget(buttons, this, bounds).CenterPoint());
}

bool DownloadLaunchView::BelongsHere(const Download& download) const {
  content::WebContents* contents = download.GetWebContents();
  tabs::TabInterface* tab =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  return tab && tab->GetBrowserWindowInterface() == browser_;
}

void DownloadLaunchView::OnDownloadAdded(Download* download) {
  // 只要这一回刚开始的（从历史记录、存盘的列表里读回来的、交回 Chrome 重试的不算）。
  if (!download->IsNew() || !IsActive(download->State()) ||
      !BelongsHere(*download)) {
    return;
  }
  // 名字这时多半还没定，先用网址里的文件名取图标（只看扩展名）。
  std::u16string filename = download->Filename();
  if (download->TargetPath().empty()) {
    const std::string from_url = download->Url().ExtractFileName();
    if (!from_url.empty()) {
      filename = base::UTF8ToUTF16(from_url);
    }
  }
  Launch(download->Guid(), filename, base::FilePath::FromUTF16Unsafe(filename));
}

void DownloadLaunchView::OnDownloadUpdated(Download* download) {
  if (toast_ && toast_->guid() == download->Guid()) {
    toast_->Update(download);
  }
}

void DownloadLaunchView::OnDownloadServiceShutdown() {
  observation_.Reset();
  service_ = nullptr;
}

void DownloadLaunchView::Launch(const std::string& guid,
                                const std::u16string& filename,
                                const base::FilePath& icon_path) {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  // 小窗口没有下载按钮：总是左下角的提示条，点它到浏览器窗口看下载列表。
  if (view && view->is_mini_window()) {
    ShowToast(guid, filename, icon_path);
    return;
  }
  SidebarView* sidebar = view ? view->sidebar() : nullptr;
  if (!sidebar) {
    return;
  }
  // 侧栏收着、按钮不在屏幕上：左下角的提示条。
  if (sidebar->is_auto_hide() && !sidebar->is_peeking()) {
    ShowToast(guid, filename, icon_path);
    return;
  }
  // 起点：鼠标在窗口里时是鼠标处（多半是刚点的链接），否则是窗口中间偏右上。
  gfx::Point pointer = display::Screen::Get()->GetCursorScreenPoint();
  views::View::ConvertPointFromScreen(this, &pointer);
  gfx::PointF from(width() * 0.55f, height() * 0.45f);
  if (GetLocalBounds().Contains(pointer)) {
    from = gfx::PointF(pointer);
  }
  if (motion::ReduceMotion()) {
    if (std::optional<gfx::PointF> center = ButtonCenter()) {
      Pulse(*center);
    }
    return;
  }
  AddChildView(std::make_unique<DownloadFlight>(this, from, icon_path));
  ++flights_;
}

void DownloadLaunchView::Land(DownloadFlight* flight) {
  // 在动画的回调里：下一轮再删。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<DownloadLaunchView> host,
                        DownloadFlight* flight) {
                       if (!host) {
                         return;
                       }
                       for (views::View* child : host->children()) {
                         if (child == flight) {
                           host->RemoveChildViewT(flight);
                           host->flights_ = std::max<size_t>(host->flights_, 1) - 1;
                           break;
                         }
                       }
                     },
                     weak_factory_.GetWeakPtr(), flight));
  if (std::optional<gfx::PointF> center = ButtonCenter()) {
    Pulse(*center);
  }
  if (StarryBrowserView* view = StarryBrowserView::From(browser_);
      view && view->sidebar()) {
    if (UtilityButtons* buttons = view->active_utility_buttons()) {
      buttons->BounceDownloadsButton();
    }
  }
}

void DownloadLaunchView::Pulse(const gfx::PointF& center) {
  auto* pulse = AddChildView(std::make_unique<ArrivalPulse>(theme_));
  pulse->SetBounds(static_cast<int>(center.x()) - 30,
                   static_cast<int>(center.y()) - 30, 60, 60);
  pulse->Start(base::BindOnce(
      [](base::WeakPtr<DownloadLaunchView> host, ArrivalPulse* pulse) {
        if (!host) {
          return;
        }
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](base::WeakPtr<DownloadLaunchView> host, ArrivalPulse* pulse) {
                  if (!host) {
                    return;
                  }
                  for (views::View* child : host->children()) {
                    if (child == pulse) {
                      host->RemoveChildViewT(pulse);
                      break;
                    }
                  }
                },
                host, pulse));
      },
      weak_factory_.GetWeakPtr(), pulse));
}

void DownloadLaunchView::ShowToast(const std::string& guid,
                                   const std::u16string& filename,
                                   const base::FilePath& icon_path) {
  if (toast_) {
    RemoveChildViewT(toast_.ExtractAsDangling());
    toast_ = nullptr;
  }
  toast_ = AddChildView(
      std::make_unique<DownloadToast>(this, guid, filename, icon_path));
  if (Download* item = service_ ? service_->Find(guid) : nullptr) {
    toast_->Update(item);
  }
  const gfx::Size size = toast_->ContentSize();
  const int inset = metrics::kContentInset + 12;
  toast_->SetGlassBounds(gfx::Rect(inset, height() - inset - size.height(),
                                   size.width(), size.height()));
  // 从左边滑进来、稍微放大。
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
  toast_held_ = false;
  toast_timer_.Start(FROM_HERE, kToastShown,
                     base::BindOnce(&DownloadLaunchView::DismissToast,
                                    base::Unretained(this)));
}

void DownloadLaunchView::HoldToast(bool hold) {
  toast_held_ = hold;
  if (hold) {
    toast_timer_.Stop();
  } else if (toast_) {
    toast_timer_.Start(FROM_HERE, kToastAfterHover,
                       base::BindOnce(&DownloadLaunchView::DismissToast,
                                      base::Unretained(this)));
  }
}

void DownloadLaunchView::DismissToast() {
  toast_timer_.Stop();
  if (!toast_) {
    return;
  }
  DownloadToast* toast = toast_;
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
          [](base::WeakPtr<DownloadLaunchView> host, DownloadToast* toast) {
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
}

std::u16string DownloadLaunchView::ToastTextForTesting() const {
  return toast_ ? toast_->Text() : std::u16string();
}

void DownloadLaunchView::LaunchForTesting(const std::u16string& filename,
                                          bool toast) {
  const base::FilePath path = base::FilePath::FromUTF16Unsafe(filename);
  if (toast) {
    ShowToast(std::string(), filename, path);
  } else {
    Launch(std::string(), filename, path);
  }
}

BEGIN_METADATA(DownloadLaunchView)
END_METADATA

}  // namespace starry
