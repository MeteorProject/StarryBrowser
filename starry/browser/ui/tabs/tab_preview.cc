// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tab_preview.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/process/process.h"
#include "base/process/process_metrics.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/performance_controls/tab_resource_usage_collector.h"
#include "chrome/browser/ui/performance_controls/tab_resource_usage_tab_helper.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/thumbnails/thumbnail_image.h"
#include "chrome/browser/ui/thumbnails/thumbnail_tab_helper.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "extensions/common/constants.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/command_bar/command_bar_view.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "starry/common/internal_urls.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/event_observer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/event_monitor.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC)
#include "content/public/browser/browser_child_process_host.h"
#endif

namespace starry {

namespace {

constexpr int kWidth = metrics::kTabPreviewWidth;
constexpr int kImageHeight = metrics::kTabPreviewImageHeight;
constexpr int kPaddingX = 12;
constexpr int kPaddingTop = 10;
constexpr int kPaddingBottom = 11;
// 离侧栏、离窗口边多远。
constexpr int kGap = 6;
constexpr int kMargin = 8;
// 出现动画的初始缩放比例与朝标签侧的起始偏移。
constexpr float kAppearScale = 0.97f;
constexpr float kAppearShift = 6;
constexpr base::TimeDelta kShowDelay = base::Milliseconds(500);
constexpr base::TimeDelta kHideDelay = base::Milliseconds(120);
constexpr base::TimeDelta kWarmPeriod = base::Milliseconds(900);
constexpr base::TimeDelta kFirstSampleDelay = base::Milliseconds(500);
constexpr base::TimeDelta kSampleInterval = base::Seconds(1);

SkColor Alpha(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(std::clamp(opacity, 0.0f, 1.0f) *
                                               255));
}

bool IsInternal(const GURL& url) {
  return InternalPageForUrl(url) != InternalPage::kNone || IsInternalURL(url) ||
         url.SchemeIs("devtools") || url.SchemeIs("chrome-untrusted");
}

// 卡片里的内容：由卡片自己画。
class PreviewContent : public views::View {
  METADATA_HEADER(PreviewContent, views::View)

 public:
  explicit PreviewContent(base::RepeatingCallback<void(gfx::Canvas*)> painter)
      : painter_(std::move(painter)) {
    SetCanProcessEventsWithinSubtree(false);
  }
  void OnPaint(gfx::Canvas* canvas) override { painter_.Run(canvas); }

 private:
  base::RepeatingCallback<void(gfx::Canvas*)> painter_;
};

BEGIN_METADATA(PreviewContent)
END_METADATA

}  // namespace

// 卡片：玻璃、面板色的底、缩略图和文字。
class TabPreviewCard : public GlassView {
  METADATA_HEADER(TabPreviewCard, GlassView)

 public:
  enum class Status { kNone, kRunning, kSleeping, kCrashed };
  struct Usage {
    std::optional<int64_t> memory;
    // 第一次读数时还算不出来。
    std::optional<double> cpu;
    // 同一个网页进程里还有几个别的标签。
    int shared = 0;
    bool operator==(const Usage&) const = default;
  };

  TabPreviewCard()
      : x_(0,
           base::BindRepeating(&TabPreviewCard::UpdateFrame,
                               base::Unretained(this))),
        y_(0,
           base::BindRepeating(&TabPreviewCard::UpdateFrame,
                               base::Unretained(this))),
        width_(kWidth,
               base::BindRepeating(&TabPreviewCard::UpdateFrame,
                                   base::Unretained(this))),
        height_(0,
                base::BindRepeating(&TabPreviewCard::UpdateFrame,
                                    base::Unretained(this))) {
    SetCornerRadius(radius::kCard, true);
    SetCanProcessEventsWithinSubtree(false);
    content_ =
        AddChildView(std::make_unique<PreviewContent>(base::BindRepeating(
            &TabPreviewCard::PaintContent, base::Unretained(this))));
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    content_->SchedulePaint();
  }

  // 换成这个标签。缩略图：用户眼前没有显示着、不是新标签页和浏览器自己的页面时才有，
  // 先给缓存里的（休眠的标签是休眠时的截图），Chrome 截到新的再换上。
  void SetTab(tabs::TabInterface* tab, bool shows_usage) {
    content::WebContents* contents = tab->GetContents();
    data_ = StarryTabData(tab);
    title_ = TabDisplayTitle(data_);
    const GURL& url = data_.visible_url;
    const bool blank = IsBlankTabUrl(url);
    const bool internal = IsInternal(url);
    subtitle_.reset();
    if (!blank) {
      if (internal) {
        subtitle_ = base::UTF8ToUTF16(url.spec());
      } else if (url.SchemeIs(extensions::kExtensionScheme)) {
        subtitle_ = L("tabPreview.extensionPage");
      } else if (url.SchemeIsFile()) {
        subtitle_ = base::UTF8ToUTF16(url.ExtractFileName());
      } else if (url.SchemeIsHTTPOrHTTPS() && !url.host().empty()) {
        subtitle_ = base::UTF8ToUTF16(url.host());
      }
      if (subtitle_ && subtitle_->empty()) {
        subtitle_.reset();
      }
    }
    status_ = Status::kNone;
    if (shows_usage && !blank && !internal) {
      status_ = contents->IsCrashed()       ? Status::kCrashed
                : contents->WasDiscarded() ? Status::kSleeping
                                           : Status::kRunning;
    }
    sleeping_ = contents->WasDiscarded();
    shows_image_ = !blank && !internal &&
                   contents->GetVisibility() != content::Visibility::VISIBLE;
    usage_ = Usage();
    image_ = gfx::ImageSkia();
    subscription_.reset();
    thumbnail_.reset();
    if (shows_image_) {
      if (ThumbnailTabHelper* helper = ThumbnailTabHelper::From(tab)) {
        thumbnail_ = helper->thumbnail();
      }
      if (thumbnail_) {
        subscription_ = thumbnail_->Subscribe();
        subscription_->SetSizeHint(gfx::Size(kWidth, kImageHeight));
        subscription_->SetUncompressedImageCallback(base::BindRepeating(
            &TabPreviewCard::OnThumbnail, weak_factory_.GetWeakPtr(),
            base::Unretained(thumbnail_.get())));
        thumbnail_->RequestThumbnailImage();
      }
    }
    content_->SchedulePaint();
  }

  // 收起了：缩略图只在卡片开着时留着。
  void Clear() {
    x_.SetValue(x_.value());
    y_.SetValue(y_.value());
    width_.SetValue(width_.value());
    height_.SetValue(height_.value());
    subscription_.reset();
    thumbnail_.reset();
    image_ = gfx::ImageSkia();
  }

  void SetUsage(const Usage& usage) {
    if (usage_ == usage) {
      return;
    }
    usage_ = usage;
    content_->SchedulePaint();
  }
  const Usage& usage() const { return usage_; }
  const std::u16string& title() const { return title_; }
  bool shows_image() const { return shows_image_; }
  bool has_image() const { return !image_.isNull(); }
  bool thumbnail_has_data() const {
    return thumbnail_ && thumbnail_->has_data();
  }

  // 卡片（不含玻璃的边距）的高度。
  int CardHeight() const {
    return (shows_image_ ? kImageHeight : 0) + kPaddingTop + TitleHeight() +
           (subtitle_ ? 3 + SubtitleFont().GetHeight() : 0) +
           (status_ != Status::kNone ? 7 + UsageHeight() : 0) + kPaddingBottom;
  }

  void Place(const gfx::Rect& card) {
    x_.SetValue(card.x());
    y_.SetValue(card.y());
    width_.SetValue(card.width());
    height_.SetValue(card.height());
    UpdateFrame();
  }

  void AnimateTo(const gfx::Rect& card, const MotionCurve& curve) {
    x_.AnimateTo(card.x(), curve);
    y_.AnimateTo(card.y(), curve);
    width_.AnimateTo(card.width(), curve);
    height_.AnimateTo(card.height(), curve);
  }

  int target_height() const { return static_cast<int>(height_.target()); }
  gfx::Rect target_bounds() const {
    return gfx::Rect(std::lround(x_.target()), std::lround(y_.target()),
                     std::lround(width_.target()),
                     std::lround(height_.target()));
  }

  LayerMotion& motion() { return motion_; }

 private:
  void UpdateFrame() {
    // 位置、大小一起排版，玻璃、阴影和内容共用同一帧的矩形。玻璃的边距随高度
    // 改变，不能按含阴影的图层原点插值，也不能让背景滤镜和内容分开缩放。
    SetGlassBounds(
        gfx::Rect(std::lround(x_.value()), std::lround(y_.value()),
                  std::max(1, static_cast<int>(std::lround(width_.value()))),
                  std::max(1, static_cast<int>(std::lround(height_.value())))));
    content_->SetBoundsRect(glass_rect());
  }

  static const gfx::FontList& TitleFont() {
    return UiFont(13, gfx::Font::Weight::SEMIBOLD);
  }
  static const gfx::FontList& SubtitleFont() { return UiFont(11); }
  static const gfx::FontList& ValueFont() {
    return UiFont(12, gfx::Font::Weight::SEMIBOLD);
  }
  static const gfx::FontList& NoteFont() { return UiFont(11); }

  int TitleHeight() const {
    int width = kWidth - 2 * kPaddingX;
    int height = 0;
    gfx::Canvas::SizeStringInt(title_, TitleFont(), &width, &height, 0,
                               gfx::Canvas::MULTI_LINE);
    return std::clamp(height, TitleFont().GetHeight(),
                      TitleFont().GetHeight() * 2);
  }

  int UsageHeight() const {
    int height = 1 + 7;
    if (status_ == Status::kRunning) {
      height += ValueFont().GetHeight();
      if (usage_.shared > 0) {
        height += 7 + UiFont(11).GetHeight();
      }
    } else {
      height += NoteFont().GetHeight();
    }
    return height;
  }

  void OnThumbnail(const ThumbnailImage* source, gfx::ImageSkia image) {
    if (source != thumbnail_.get()) {
      return;
    }
    image_ = sleeping_ ? gfx::ImageSkiaOperations::CreateHSLShiftedImage(
                             image, color_utils::HSL{-1, 0.4, -1})
                       : image;
    content_->SchedulePaint();
  }

  void PaintContent(gfx::Canvas* canvas) {
    const gfx::Rect bounds = content_->GetLocalBounds();
    canvas->sk_canvas()->clipRRect(
        SkRRect::MakeRectXY(gfx::RectToSkRect(bounds), radius::kCard,
                            radius::kCard),
        true);
    // 玻璃表面叠加一层面板底色。
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(Alpha(theme_.surface_panel, theme_.is_dark ? 0.66f : 0.76f));
    canvas->DrawRoundRect(gfx::RectF(bounds), radius::kCard, fill);

    int y = 0;
    if (shows_image_) {
      const gfx::Rect picture(0, 0, bounds.width(), kImageHeight);
      // 不透明：玻璃后面的网页不要透到占位里。
      canvas->FillRect(picture, theme_.surface_panel);
      canvas->FillRect(picture, theme_.OnSurface(theme_.is_dark ? 0.06f : 0.04f));
      if (!image_.isNull() && image_.width() > 0 && image_.height() > 0) {
        // 等比放大铺满预览区并顶边对齐。
        const float scale =
            std::max(static_cast<float>(picture.width()) / image_.width(),
                     static_cast<float>(picture.height()) / image_.height());
        const int width = std::ceil(image_.width() * scale);
        const int height = std::ceil(image_.height() * scale);
        canvas->Save();
        canvas->ClipRect(picture);
        canvas->DrawImageInt(image_, 0, 0, image_.width(), image_.height(),
                             (picture.width() - width) / 2, 0, width, height,
                             true);
        canvas->Restore();
      } else if (!data_.favicon.IsEmpty() && content_->GetColorProvider()) {
        const int size = 24;
        const gfx::ImageSkia icon =
            data_.favicon.Rasterize(content_->GetColorProvider());
        cc::PaintFlags faded;
        faded.setAlphaf(0.8f);
        canvas->DrawImageInt(icon, 0, 0, icon.width(), icon.height(),
                             picture.CenterPoint().x() - size / 2,
                             picture.CenterPoint().y() - size / 2, size, size,
                             true, faded);
      }
      canvas->FillRect(gfx::Rect(0, kImageHeight - 1, bounds.width(), 1),
                       theme_.OnSurface(0.08f));
      y = kImageHeight;
    }

    const int text_width = bounds.width() - 2 * kPaddingX;
    y += kPaddingTop;
    const int title_height = TitleHeight();
    canvas->DrawStringRectWithFlags(
        title_, TitleFont(), theme_.on_surface,
        gfx::Rect(kPaddingX, y, text_width, title_height),
        gfx::Canvas::MULTI_LINE | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    y += title_height;
    if (subtitle_) {
      y += 3;
      const std::u16string elided = gfx::ElideText(
          *subtitle_, SubtitleFont(), text_width, gfx::ELIDE_MIDDLE);
      canvas->DrawStringRectWithFlags(
          elided, SubtitleFont(), theme_.on_surface_variant,
          gfx::Rect(kPaddingX, y, text_width, SubtitleFont().GetHeight()),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
      y += SubtitleFont().GetHeight();
    }
    if (status_ == Status::kNone) {
      return;
    }
    y += 7;
    canvas->FillRect(gfx::Rect(kPaddingX, y, text_width, 1),
                     theme_.OnSurface(0.08f));
    y += 1 + 7;
    if (status_ == Status::kRunning) {
      const int row = ValueFont().GetHeight();
      int x = kPaddingX;
      const auto metric = [&](const gfx::VectorIcon& icon,
                              const std::u16string& label,
                              const std::u16string& value, bool warning) {
        canvas->Save();
        canvas->Translate(gfx::Vector2d(x, y + (row - 10) / 2));
        gfx::PaintVectorIcon(canvas, icon, 10, theme_.on_surface_variant);
        canvas->Restore();
        x += 10 + 4;
        const int label_width = gfx::GetStringWidth(label, UiFont(11));
        canvas->DrawStringRectWithFlags(label, UiFont(11),
                                        theme_.on_surface_variant,
                                        gfx::Rect(x, y, label_width + 1, row),
                                        gfx::Canvas::NO_SUBPIXEL_RENDERING);
        x += label_width + 4;
        const int value_width = gfx::GetStringWidth(value, ValueFont());
        canvas->DrawStringRectWithFlags(
            value, ValueFont(), warning ? theme_.warning() : theme_.on_surface,
            gfx::Rect(x, y, value_width + 1, row),
            gfx::Canvas::NO_SUBPIXEL_RENDERING);
        x += value_width + 16;
      };
      metric(icons::kMemoryChip, L("tabPreview.memory"),
             usage_.memory ? ui::FormatBytes(base::ByteSize(
                                 static_cast<uint64_t>(*usage_.memory)))
                           : u"—",
             false);
      metric(icons::kCpu, u"CPU",
             usage_.cpu ? base::UTF8ToUTF16(base::StringPrintf("%.1f%%",
                                                               *usage_.cpu))
                        : u"—",
             usage_.cpu.value_or(0) >= 50);
      y += row;
      if (usage_.shared > 0) {
        y += 7;
        canvas->DrawStringRectWithFlags(
            L("tabPreview.sharedProcess", {{"count", usage_.shared}}),
            UiFont(11), theme_.on_surface_variant,
            gfx::Rect(kPaddingX, y, text_width, UiFont(11).GetHeight()),
            gfx::Canvas::NO_SUBPIXEL_RENDERING);
      }
      return;
    }
    const bool crashed = status_ == Status::kCrashed;
    const int row = NoteFont().GetHeight();
    canvas->Save();
    canvas->Translate(gfx::Vector2d(kPaddingX, y + (row - 10) / 2));
    gfx::PaintVectorIcon(
        canvas, crashed ? icons::kExclamationTriangleFill : icons::kMoonZzz, 10,
        theme_.on_surface_variant);
    canvas->Restore();
    canvas->DrawStringRectWithFlags(
        crashed ? L("tabPreview.crashed") : L("tabPreview.sleeping"),
        NoteFont(), theme_.on_surface_variant,
        gfx::Rect(kPaddingX + 15, y, text_width - 15, row),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  Theme theme_ = Theme::DarkBase();
  raw_ptr<PreviewContent> content_ = nullptr;
  tabs::TabData data_;
  std::u16string title_;
  std::optional<std::u16string> subtitle_;
  Status status_ = Status::kNone;
  bool sleeping_ = false;
  bool shows_image_ = false;
  Usage usage_;
  gfx::ImageSkia image_;
  scoped_refptr<ThumbnailImage> thumbnail_;
  std::unique_ptr<ThumbnailImage::Subscription> subscription_;
  LayerMotion motion_;
  ValueMotion x_;
  ValueMotion y_;
  ValueMotion width_;
  ValueMotion height_;
  base::WeakPtrFactory<TabPreviewCard> weak_factory_{this};
};

BEGIN_METADATA(TabPreviewCard)
END_METADATA

// views::View 也有 OnEvent，所以窗口的事件由这个小对象转给 TabPreview。
class TabPreview::EventWatcher : public ui::EventObserver {
 public:
  explicit EventWatcher(TabPreview* preview) : preview_(preview) {}
  void OnEvent(const ui::Event& event) override { preview_->OnWindowEvent(); }

 private:
  const raw_ptr<TabPreview> preview_;
};

TabPreview::TabPreview(StarryBrowserView* browser_view)
    : browser_view_(browser_view),
      watcher_(std::make_unique<EventWatcher>(this)) {
  SetCanProcessEventsWithinSubtree(false);
  card_ = AddChildView(std::make_unique<TabPreviewCard>());
  card_->layer()->SetOpacity(0);
  card_->SetVisible(false);
}

TabPreview::~TabPreview() {
  monitor_.reset();
}

void TabPreview::SetTheme(const Theme& theme) {
  theme_ = theme;
  card_->SetTheme(theme);
}

bool TabPreview::Enabled() const {
  return prefs::PrefsFor(browser_view_->browser()->GetProfile())
      ->GetBoolean(prefs::kTabPreviewsEnabled);
}

void TabPreview::Hover(tabs::TabInterface* tab,
                       const gfx::Rect& anchor,
                       std::optional<int> edge,
                       bool inside) {
  if (!tab) {
    return;
  }
  const tabs::TabHandle handle = tab->GetHandle();
  if (inside) {
    hovering_gap_ = false;
    hovered_ = handle;
    hovered_anchor_ = anchor;
    hovered_edge_ = edge;
    if (suppressed_ == handle || !Enabled()) {
      return;
    }
    hide_timer_.Stop();
    show_timer_.Stop();
    // 等待期间就开始监听：卡片出来之前点了、右键了，也不要再弹出来。
    InstallMonitor();
    if (shown_tab_ || IsWarm()) {
      Show(handle);
      return;
    }
    show_timer_.Start(FROM_HERE, kShowDelay,
                      base::BindOnce(
                          [](base::WeakPtr<TabPreview> preview,
                             tabs::TabHandle handle) {
                            if (preview && preview->hovered_ == handle) {
                              preview->Show(handle);
                            }
                          },
                          weak_factory_.GetWeakPtr(), handle));
    return;
  }
  if (suppressed_ == handle) {
    suppressed_.reset();
  }
  // 进下一个标签的事件可能先到：那时已经在为下一个标签计时或显示了，不要打断。
  if (hovered_ != handle) {
    return;
  }
  hovered_.reset();
  show_timer_.Stop();
  if (!shown_tab_) {
    RemoveMonitor();
    return;
  }
  ScheduleHide();
}

void TabPreview::HoverGap(bool inside) {
  if (hovering_gap_ == inside) {
    return;
  }
  hovering_gap_ = inside;
  if (inside) {
    hide_timer_.Stop();
  } else {
    ScheduleHide();
  }
}

void TabPreview::ScheduleHide() {
  if (hovered_ || hovering_gap_ || !shown_tab_ || hide_timer_.IsRunning()) {
    return;
  }
  hide_timer_.Start(
      FROM_HERE, kHideDelay,
      base::BindOnce(
          [](base::WeakPtr<TabPreview> preview) {
            if (preview && !preview->hovered_ && !preview->hovering_gap_) {
              preview->Dismiss();
            }
          },
          weak_factory_.GetWeakPtr()));
}

void TabPreview::Show(tabs::TabHandle handle) {
  tabs::TabInterface* tab = handle.Get();
  if (!tab || shown_tab_ == handle) {
    return;
  }
  // 命令栏开着时不显示。
  if (CommandBarView* bar = browser_view_->command_bar();
      bar && bar->GetVisible()) {
    return;
  }
  // 正在淡出的卡片仍然是同一张卡片，重新进入时接着它当前的位置和透明度动。
  const bool moving = card_->GetVisible();
  shown_tab_ = handle;
  shown_anchor_ = hovered_anchor_;
  shown_edge_ = hovered_edge_;
  card_->SetTab(tab, prefs::PrefsFor(browser_view_->browser()->GetProfile())
                         ->GetBoolean(prefs::kTabPreviewShowsUsage));
  Place(moving);
  StartSampling();
  InstallMonitor();
}

void TabPreview::Place(bool moving) {
  const int height = card_->CardHeight();
  const int max_x = std::max(width() - kWidth - kMargin, kMargin);
  const int max_y = std::max(this->height() - height - kMargin, kMargin);
  // 侧栏布局放在侧栏右边、和标签顶边对齐；顶部标签布局放在标签下面、和标签左边对齐。
  const gfx::Rect rect =
      shown_edge_ ? gfx::Rect(std::min(*shown_edge_ + kGap, max_x),
                              std::clamp(shown_anchor_.y(), kMargin, max_y),
                              kWidth, height)
                  : gfx::Rect(std::clamp(shown_anchor_.x(), kMargin, max_x),
                              std::min(shown_anchor_.bottom() + kGap, max_y),
                              kWidth, height);
  ui::Layer* layer = card_->layer();
  const bool still = motion::ReduceMotion();
  if (moving && card_->GetVisible()) {
    // 换标签：位置和大小都从现在的值与速度接着动。
    card_->AnimateTo(rect, MotionCurve::Of(motion::kTabPreviewMove));
    card_->motion().AnimateOpacity(layer, 1, motion::Hover());
    return;
  }
  // 出现：卡片直接放在终点，从标签那一侧轻微放大、移入并淡入。缩放中心位于卡片上正对
  // 标签的点（侧栏：左边缘与标签中线等高；顶部标签：顶边与标签中点对齐），卡片因窗口
  // 边缘被推开时仍保持从标签位置展开。
  card_->Place(rect);
  card_->SetVisible(true);
  if (still) {
    card_->motion().SetTransform(layer, LayerPose());
  } else {
    const gfx::Rect glass = card_->glass_rect();
    const gfx::Point tab = shown_anchor_.IsEmpty()
                               ? rect.CenterPoint()
                               : shown_anchor_.CenterPoint();
    LayerPose from;
    from.scale = kAppearScale;
    gfx::PointF origin;
    if (shown_edge_) {
      from.offset = gfx::Vector2dF(-kAppearShift, 0);
      origin = gfx::PointF(glass.x(), glass.y() + std::clamp(tab.y() - rect.y(),
                                                             0, rect.height()));
    } else {
      from.offset = gfx::Vector2dF(0, -kAppearShift);
      origin = gfx::PointF(
          glass.x() + std::clamp(tab.x() - rect.x(), 0, rect.width()),
          glass.y());
    }
    card_->motion().AnimateTransformFrom(
        layer, from, LayerPose(), MotionCurve::Of(motion::kTabPreview), origin);
  }
  card_->motion().AnimateOpacityFrom(layer, 0, 1,
                                     MotionCurve::Of(motion::kTabPreview));
}

void TabPreview::Dismiss(std::optional<tabs::TabHandle> suppress,
                         bool cooling) {
  show_timer_.Stop();
  hide_timer_.Stop();
  hovering_gap_ = false;
  RemoveMonitor();
  if (suppress) {
    suppressed_ = suppress;
  }
  if (!shown_tab_) {
    return;
  }
  shown_tab_.reset();
  sample_timer_.Stop();
  metrics_.reset();
  metrics_pid_ = base::kNullProcessId;
  hidden_at_ = cooling ? base::TimeTicks() : base::TimeTicks::Now();
  card_->motion().AnimateOpacity(card_->layer(), 0, motion::TabPreviewHide(),
                                 base::BindOnce(
                                     [](base::WeakPtr<TabPreview> preview) {
                                       if (preview && !preview->shown_tab_) {
                                         preview->card_->SetVisible(false);
                                         preview->card_->Clear();
                                       }
                                     },
                                     weak_factory_.GetWeakPtr()));
}

bool TabPreview::IsWarm() const {
  return !hidden_at_.is_null() &&
         base::TimeTicks::Now() - hidden_at_ < kWarmPeriod;
}

void TabPreview::InstallMonitor() {
  if (monitor_ || !GetWidget()) {
    return;
  }
  monitor_ = views::EventMonitor::CreateWindowMonitor(
      watcher_.get(), GetWidget()->GetNativeWindow(),
      {ui::EventType::kMousePressed, ui::EventType::kMousewheel,
       ui::EventType::kScroll, ui::EventType::kKeyPressed});
}

void TabPreview::RemoveMonitor() {
  monitor_.reset();
}

void TabPreview::OnWindowEvent() {
  // 点击（包括开始拖动、右键菜单）、滚动、按键都先收起卡片，事件照常往下传。
  Dismiss(hovered_, /*cooling=*/true);
}

void TabPreview::StartSampling() {
  sample_timer_.Stop();
  metrics_.reset();
  metrics_pid_ = base::kNullProcessId;
  if (!prefs::PrefsFor(browser_view_->browser()->GetProfile())
           ->GetBoolean(prefs::kTabPreviewShowsUsage)) {
    return;
  }
  Sample();
  sample_timer_.Start(FROM_HERE, kFirstSampleDelay,
                      base::BindOnce(&TabPreview::Sample,
                                     weak_factory_.GetWeakPtr()));
}

void TabPreview::Sample() {
  tabs::TabInterface* tab = shown_tab_ ? shown_tab_->Get() : nullptr;
  if (!tab) {
    // 标签被关掉了（比如扩展关的）：卡片也收起。
    if (shown_tab_) {
      Dismiss();
    }
    return;
  }
  sample_timer_.Start(FROM_HERE, kSampleInterval,
                      base::BindOnce(&TabPreview::Sample,
                                     weak_factory_.GetWeakPtr()));
  content::WebContents* contents = tab->GetContents();
  if (contents->WasDiscarded() || contents->IsCrashed()) {
    return;
  }
  TabPreviewCard::Usage usage = card_->usage();
  // 内存：Chrome 性能面板的数（按网页进程估的），读数在后台更新，下一次就是新的。
  if (TabResourceUsageTabHelper* helper = TabResourceUsageTabHelper::From(tab)) {
    const int64_t bytes = helper->GetMemoryUsage().InBytes();
    usage.memory = bytes > 0 ? std::optional<int64_t>(bytes) : std::nullopt;
  }
  if (TabResourceUsageCollector* collector = TabResourceUsageCollector::Get()) {
    collector->ImmediatelyRefreshMetrics(contents);
  }
  // CPU：网页进程这一段时间的占用。导航可能换了进程：pid 变了就从头算。
  content::RenderProcessHost* process =
      contents->GetPrimaryMainFrame()->GetProcess();
  usage.cpu.reset();
  if (process && process->IsReady() && process->GetProcess().IsValid()) {
    const base::ProcessId pid = process->GetProcess().Pid();
    if (pid != metrics_pid_ || !metrics_) {
#if BUILDFLAG(IS_MAC)
      metrics_ = base::ProcessMetrics::CreateProcessMetrics(
          process->GetProcess().Handle(),
          content::BrowserChildProcessHost::GetPortProvider());
#else
      metrics_ = base::ProcessMetrics::CreateProcessMetrics(
          process->GetProcess().Handle());
#endif
      metrics_pid_ = pid;
      (void)metrics_->GetPlatformIndependentCPUUsage();
    } else if (auto cpu = metrics_->GetPlatformIndependentCPUUsage();
               cpu.has_value()) {
      usage.cpu = *cpu;
    }
    // 同一个网页进程里的别的标签（比如 window.open 打开的页面和打开它的页面）。
    usage.shared = 0;
    for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
      TabStripModel* model = browser->GetTabStripModel();
      for (int i = 0; model && i < model->count(); ++i) {
        content::WebContents* other = model->GetWebContentsAt(i);
        if (other != contents && !other->WasDiscarded() &&
            other->GetPrimaryMainFrame()->GetProcess() == process) {
          ++usage.shared;
        }
      }
    }
  }
  card_->SetUsage(usage);
  // 有没有「共用进程」那一行，卡片的高度会变。
  if (card_->CardHeight() != card_->target_height()) {
    Place(/*moving=*/true);
  }
}

std::string TabPreview::DebugDescription() const {
  const gfx::Rect target = card_->target_bounds();
  const LayerPose pose = card_->motion().CurrentPose(card_->layer());
  const std::string animation = base::StringPrintf(
      " visible=%d opacity=%.3f scale=%.3f offset=%s target=%s height=%d->%d",
      card_->GetVisible(), card_->motion().CurrentOpacity(card_->layer()),
      pose.scale, pose.offset.ToString().c_str(), target.ToString().c_str(),
      card_->glass_bounds().height(), card_->target_height());
  if (!shown_tab_) {
    return "preview=none" + animation;
  }
  const TabPreviewCard::Usage& usage = card_->usage();
  return base::StrCat(
      {"preview=[", base::UTF16ToUTF8(card_->title()),
       "] card=", card_->glass_bounds().ToString(), animation, " image=",
       card_->shows_image() ? (card_->has_image() ? "yes" : "pending") : "no",
       card_->thumbnail_has_data() ? "(cached)" : "", " memory=",
       usage.memory ? base::NumberToString(*usage.memory / 1024 / 1024) + "MB"
                    : "-",
       " cpu=", usage.cpu ? base::NumberToString(*usage.cpu) : "-",
       " shared=", base::NumberToString(usage.shared)});
}

BEGIN_METADATA(TabPreview)
END_METADATA

}  // namespace starry
