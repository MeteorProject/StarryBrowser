// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_island.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "chrome/browser/picture_in_picture/picture_in_picture_window_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/media/media_parts.h"
#include "starry/browser/ui/media/media_player_view.h"
#include "starry/browser/ui/media/media_video_view.h"
#include "starry/browser/ui/media/media_waveform.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/event_observer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/event_monitor.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

using media_session::mojom::MediaSessionAction;

// 胶囊：高 46、圆角 23，离内容卡片的边 16；封面 32（外面一圈 2
// 的进度，半径 19.5）， 文字从 46 开始，右边 30 的播放暂停。宽度跟着文字，在
// 196…264 之间。
constexpr int kCapsuleHeight = 46;
constexpr float kCapsuleRadius = 23;
constexpr int kCapsuleMinWidth = 196;
constexpr int kCapsuleMaxWidth = 264;
constexpr int kInset = 16;
constexpr int kArtDiameter = 32;
constexpr int kTextX = 48;
constexpr int kToggle = 30;
// 展开：宽 340、圆角 26，四周留 14。
constexpr int kExpandedWidth = 340;
constexpr float kExpandedRadius = 26;
constexpr int kPadding = 14;
// 鼠标离开展开的卡片多远才算离开。
constexpr int kLeaveSlack = 12;
// 视频在角上时展开的播放器摘在它朝里的一边，隔开这么多。
constexpr int kStackGap = 10;
// 卫星：直径 36（封面 30 + 一圈进度），和胶囊、彼此隔 8；最多 3 颗。
constexpr int kSatellite = 36;
constexpr int kSatelliteGap = 8;
constexpr size_t kMaxSatellites = 3;
// 坞（飞行的终点）的圆角。
constexpr float kDockRadius = 12;
// 飞行的弧线拱起这么高。
constexpr float kFlightArc = 36;
constexpr float kVideoFlightArc = 24;
// 飞行用 0…1 的进度：精确到千分之一，保留轻微过冲后的回稳，避免末帧跳动。
constexpr double kVideoFlightEpsilon = 0.001;
// 视频：横的宽 304，竖的高 260；圆角 14。
constexpr int kVideoWidth = 304;
constexpr int kVideoTallHeight = 260;
constexpr float kVideoRadius = 14;
// 让位：和挡着的东西隔开这么多。
constexpr int kAvoidGap = 10;
// 拖着视频时指针出了窗口再走这么远才算「松手弹到桌面」（往角上一甩不会误弹），回到
// 窗口里才取消。
constexpr int kPopOutSlack = 16;

gfx::Rect Lerp(const gfx::Rect& from, const gfx::Rect& to, double t) {
  const auto mix = [t](int a, int b) {
    return static_cast<int>(std::lround(a + (b - a) * t));
  };
  return gfx::Rect(mix(from.x(), to.x()), mix(from.y(), to.y()),
                   std::max(0, mix(from.width(), to.width())),
                   std::max(0, mix(from.height(), to.height())));
}

float Clamp01(double v) {
  return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

std::u16string Subtitle(const MediaEntry& entry) {
  return entry.artist.empty() ? entry.source : entry.artist;
}

gfx::Point ScreenPoint(const views::View* view, const gfx::Point& point) {
  gfx::Point screen = point;
  views::View::ConvertPointToScreen(view, &screen);
  return screen;
}

// 视频拖到窗口外面、松手会弹到桌面时盖在画面上：暗下来，看得见的那部分中间是图标和
// 「松手弹到桌面」。
class PopOutHint : public views::View {
  METADATA_HEADER(PopOutHint, views::View)

 public:
  PopOutHint() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetCanProcessEventsWithinSubtree(false);
  }

  // 画面在网页范围里看得见的那部分（本视图的坐标）：图标和字放在它中间。
  void SetVisibleArea(const gfx::Rect& area) {
    if (area_ != area) {
      area_ = area;
      SchedulePaint();
    }
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags dim;
    dim.setColor(SkColorSetA(SK_ColorBLACK, 0.5f * 255));
    canvas->DrawRect(gfx::RectF(GetLocalBounds()), dim);
    const gfx::Rect area = area_.IsEmpty() ? GetLocalBounds() : area_;
    constexpr int kGlyph = 22;
    constexpr int kGap = 8;
    const gfx::FontList& font = UiFont(13, gfx::Font::Weight::SEMIBOLD);
    const int text_height = font.GetHeight();
    const int top = area.CenterPoint().y() - (kGlyph + kGap + text_height) / 2;
    PaintMediaGlyph(
        canvas, MediaGlyph::kPopOut,
        gfx::RectF(area.CenterPoint().x() - kGlyph / 2.0f, top, kGlyph, kGlyph),
        SK_ColorWHITE);
    canvas->DrawStringRectWithFlags(
        L("media.releaseToPopOut"), font, SK_ColorWHITE,
        gfx::Rect(area.x() + 8, top + kGlyph + kGap,
                  std::max(0, area.width() - 16), text_height),
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  gfx::Rect area_;
};

BEGIN_METADATA(PopOutHint)
END_METADATA

}  // namespace

// 窗口的事件转给岛（views::View 自己也有 OnEvent）。
class MediaIsland::EventWatcher : public ui::EventObserver {
 public:
  explicit EventWatcher(MediaIsland* island) : island_(island) {}
  void OnEvent(const ui::Event& event) override {
    if (!event.IsLocatedEvent()) {
      return;
    }
    if (event.type() == ui::EventType::kMouseExited) {
      island_->OnWindowMouseMove(gfx::Point(-10000, -10000));
      return;
    }
    gfx::Point point = event.AsLocatedEvent()->root_location();
#if !BUILDFLAG(IS_MAC)
    // Aura 的 EventMonitor 会把 root_location 转成屏幕坐标（macOS 上是窗口
    // 坐标）；这里转回 Widget 坐标，和 OnWindowMouseMove / Press 的约定一致。
    if (views::Widget* widget = island_->GetWidget();
        widget && widget->GetRootView()) {
      views::View::ConvertPointFromScreen(widget->GetRootView(), &point);
    }
#endif
    if (event.type() == ui::EventType::kMousePressed) {
      island_->OnWindowMousePress(point);
    } else {
      island_->OnWindowMouseMove(point);
    }
  }

 private:
  const raw_ptr<MediaIsland> island_;
};

// 胶囊：实色（封面取色的渐变）、阴影、细边；点空白处展开，按住拖动岛。飞行时也是它在飞，
// 圆角跟着变。
class MediaIsland::Capsule : public views::Button,
                             public views::ViewTargeterDelegate {
  METADATA_HEADER(Capsule, views::Button)

 public:
  static constexpr int kMargin = 18;

  explicit Capsule(MediaIsland* island)
      : views::Button(base::BindRepeating(
            [](MediaIsland* island) { island->SetExpanded(true); },
            base::Unretained(island))),
        island_(island) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetWindowDraggable(this, false);
    toggle_ = AddChildView(std::make_unique<MediaButton>(
        base::BindRepeating(
            [](Capsule* capsule) {
              capsule->island_->hub_->TogglePlayPause(capsule->entry_.id);
            },
            base::Unretained(this)),
        MediaGlyph::kPause, L("media.pause"), kToggle, 12,
        MediaButton::Style::kTonal));
    // 标题下面是滚动的响度（最近几秒）。
    wave_ = AddChildView(std::make_unique<MediaWaveform>(
        island->hub_, MediaWaveform::Style::kRoll));
    wave_->SetBar(2, 1.5f);
  }

  // 胶囊（不含阴影的边距）在父视图里的位置。
  void SetSurface(const gfx::Rect& surface, float radius) {
    radius_ = radius;
    gfx::Rect bounds = surface;
    bounds.Outset(kMargin);
    SetBoundsRect(bounds);
    SchedulePaint();
  }
  gfx::Rect surface() const {
    gfx::Rect rect = GetLocalBounds();
    rect.Inset(kMargin);
    return rect;
  }

  void Set(const MediaEntry& entry, const Theme& window) {
    entry_ = entry;
    window_ = window;
    theme_ = MediaThemeFor(&entry, window);
    toggle_->SetTheme(theme_);
    toggle_->SetGlyph(entry.playing ? MediaGlyph::kPause : MediaGlyph::kPlay,
                      L(entry.playing ? "media.pause" : "media.play"));
    wave_->Set(entry, theme_.accent, theme_.accent, theme_.OnSurface(0.3f));
    GetViewAccessibility().SetName(
        L("media.accessibility",
          {{"title", entry.title}, {"source", entry.source}}));
    SetTooltipText(entry.title);
    SchedulePaint();
  }

  // 文字要多宽（定胶囊的宽度）。
  int PreferredWidth() const {
    const int text =
        std::max(gfx::GetStringWidth(entry_.title,
                                     UiFont(12, gfx::Font::Weight::SEMIBOLD)),
                 gfx::GetStringWidth(Subtitle(entry_), UiFont(11)));
    return std::clamp(kTextX + text + 12 + kToggle + 8, kCapsuleMinWidth,
                      kCapsuleMaxWidth);
  }

  // views::Button:
  void Layout(PassKey) override {
    const gfx::Rect s = surface();
    toggle_->SetBounds(s.right() - 8 - kToggle,
                       s.CenterPoint().y() - kToggle / 2, kToggle, kToggle);
    const int x = s.x() + kTextX;
    const int top = s.CenterPoint().y() - 16;
    wave_->SetBounds(x, top + 19, std::max(0, toggle_->x() - 10 - x), 11);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const gfx::RectF s(surface());
    if (s.IsEmpty()) {
      return;
    }
    // 阴影和底：左边是封面的颜色，往右过渡到面板色。
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    const SkPoint points[2] = {gfx::PointFToSkPoint(s.origin()),
                               gfx::PointFToSkPoint(s.top_right())};
    const SkColor4f colors[2] = {SkColor4f::FromColor(theme_.primary_container),
                                 SkColor4f::FromColor(window_.surface_panel)};
    fill.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr,
                                                       2, SkTileMode::kClamp));
    fill.setLooper(gfx::CreateShadowDrawLooper(
        {gfx::ShadowValue(gfx::Vector2d(0, 6), 18,
                          SkColorSetA(SK_ColorBLACK, 0.26f * 255)),
         gfx::ShadowValue(gfx::Vector2d(0, 1), 3,
                          SkColorSetA(SK_ColorBLACK, 0.18f * 255))}));
    canvas->DrawRoundRect(s, radius_, fill);
    cc::PaintFlags edge;
    edge.setAntiAlias(true);
    edge.setStyle(cc::PaintFlags::kStroke_Style);
    edge.setStrokeWidth(1);
    edge.setColor(theme_.OnSurface(0.12f));
    gfx::RectF stroke = s;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, std::max(0.0f, radius_ - 0.5f), edge);

    // 封面和一圈进度。
    const gfx::PointF center(s.x() + 7 + kArtDiameter / 2.0f + 2,
                             s.CenterPoint().y());
    PaintArtwork(canvas,
                 gfx::RectF(center.x() - kArtDiameter / 2.0f,
                            center.y() - kArtDiameter / 2.0f, kArtDiameter,
                            kArtDiameter),
                 kArtDiameter / 2.0f, entry_, theme_);
    if (const std::optional<double> progress = entry_.Progress()) {
      PaintProgressRing(canvas, center, kArtDiameter / 2.0f + 3.5f, 2,
                        *progress, theme_.OnSurface(0.14f), theme_.accent);
    }
    // 标题。
    const int x = static_cast<int>(s.x()) + kTextX;
    const int right = toggle_->x() - 8;
    const int top = static_cast<int>(s.CenterPoint().y()) - 16;
    DrawMediaText(canvas, entry_.title, 12, /*semibold=*/true,
                  theme_.on_surface, gfx::Rect(x, top, right - x, 17));
  }

  // 按住拖动：超过拖动的距离就不算点击。
  bool OnMousePressed(const ui::MouseEvent& event) override {
    press_ = ScreenPoint(this, event.location());
    dragging_ = false;
    return views::Button::OnMousePressed(event);
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    last_ = ScreenPoint(this, event.location());
    if (!dragging_ && ExceededDragThreshold(last_ - press_)) {
      dragging_ = true;
      SetState(STATE_NORMAL);
      island_->BeginDrag(press_);
    }
    if (dragging_) {
      island_->DragTo(last_);
      return true;
    }
    return views::Button::OnMouseDragged(event);
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (dragging_) {
      dragging_ = false;
      SetState(STATE_NORMAL);
      island_->EndDrag(ScreenPoint(this, event.location()));
      return;
    }
    views::Button::OnMouseReleased(event);
  }
  void OnMouseCaptureLost() override {
    if (dragging_) {
      dragging_ = false;
      island_->EndDrag(last_);
    }
    views::Button::OnMouseCaptureLost();
  }

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override {
    return surface().Intersects(rect);
  }

  const MediaEntry& entry() const { return entry_; }

 private:
  const raw_ptr<MediaIsland> island_;
  raw_ptr<MediaButton> toggle_ = nullptr;
  raw_ptr<MediaWaveform> wave_ = nullptr;
  MediaEntry entry_;
  Theme window_ = Theme::DarkBase();
  Theme theme_ = Theme::DarkBase();
  float radius_ = kCapsuleRadius;
  gfx::Point press_;
  gfx::Point last_;
  bool dragging_ = false;
};

BEGIN_METADATA(MediaIsland, Capsule)
END_METADATA

// 卫星：其余的一个媒体，圆的封面外面一圈进度（暂停时变淡）。点一下换到岛上（视频在角上时
// 在它旁边展开播放器）。
class MediaIsland::Satellite : public views::Button {
  METADATA_HEADER(Satellite, views::Button)

 public:
  explicit Satellite(MediaIsland* island)
      : views::Button(
            base::BindRepeating(&Satellite::Pressed, base::Unretained(this))),
        island_(island) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetWindowDraggable(this, false);
    SetAnimationDuration(base::Milliseconds(180));
  }

  void Set(const MediaEntry& entry, const Theme& window) {
    entry_ = entry;
    window_ = window;
    theme_ = MediaThemeFor(&entry, window);
    SetTooltipText(entry.title);
    GetViewAccessibility().SetName(
        L("media.accessibility",
          {{"title", entry.title}, {"source", entry.source}}));
    SchedulePaint();
  }

  // views::Button:
  void PaintButtonContents(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    const gfx::PointF center = bounds.CenterPoint();
    const float radius = bounds.width() / 2;
    const float hover = static_cast<float>(hover_animation().GetCurrentValue());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(window_.surface_panel);
    fill.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
        gfx::Vector2d(0, 3), 10, SkColorSetA(SK_ColorBLACK, 0.25f * 255))}));
    canvas->DrawCircle(center, radius - 1, fill);
    const float art = radius - 4 + hover;
    PaintArtwork(
        canvas,
        gfx::RectF(center.x() - art, center.y() - art, art * 2, art * 2), art,
        entry_, theme_);
    if (!entry_.playing) {
      cc::PaintFlags dim;
      dim.setAntiAlias(true);
      dim.setColor(SkColorSetA(SK_ColorBLACK, 0.35f * 255));
      canvas->DrawCircle(center, art, dim);
    }
    PaintProgressRing(
        canvas, center, radius - 2, 2, entry_.Progress().value_or(0),
        window_.OnSurface(0.14f),
        entry_.playing ? theme_.accent : window_.on_surface_variant);
  }

 private:
  void Pressed() { island_->SelectMedia(entry_.id); }

  const raw_ptr<MediaIsland> island_;
  MediaEntry entry_;
  Theme window_ = Theme::DarkBase();
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(MediaIsland, Satellite)
END_METADATA

// 展开时玻璃上面的一层：面板色（读得清字）和从上往下淡掉的封面色，一圈细线。
class MediaIsland::Tint : public views::View {
  METADATA_HEADER(Tint, views::View)

 public:
  Tint() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void Set(const Theme& window, const Theme& media, float radius) {
    window_ = window;
    media_ = media;
    radius_ = radius;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags panel;
    panel.setAntiAlias(true);
    panel.setColor(SkColorSetA(window_.surface_panel,
                               window_.is_dark ? 0.8f * 255 : 0.78f * 255));
    canvas->DrawRoundRect(bounds, radius_, panel);
    cc::PaintFlags wash;
    wash.setAntiAlias(true);
    const SkPoint points[2] = {gfx::PointFToSkPoint(bounds.origin()),
                               gfx::PointFToSkPoint(bounds.bottom_left())};
    const SkColor4f colors[3] = {
        SkColor4f::FromColor(SkColorSetA(media_.primary_container, 0.8f * 255)),
        SkColor4f::FromColor(SkColorSetA(media_.primary_container, 0.2f * 255)),
        SkColor4f::FromColor(SkColorSetA(media_.primary_container, 0))};
    const SkScalar positions[3] = {0, 0.55f, 0.9f};
    wash.setShader(cc::PaintShader::MakeLinearGradient(
        points, colors, positions, 3, SkTileMode::kClamp));
    canvas->DrawRoundRect(bounds, radius_, wash);
    cc::PaintFlags edge;
    edge.setAntiAlias(true);
    edge.setStyle(cc::PaintFlags::kStroke_Style);
    edge.setStrokeWidth(1);
    edge.setColor(window_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, std::max(0.0f, radius_ - 0.5f), edge);
  }

 private:
  Theme window_ = Theme::DarkBase();
  Theme media_ = Theme::DarkBase();
  float radius_ = kExpandedRadius;
};

BEGIN_METADATA(MediaIsland, Tint)
END_METADATA

// 窗内的画中画：圆角的视频画面（MediaVideoView）和阴影。画面上控件以外的地方按住拖动岛，
// 双击回到标签页。拿走以后留着最后一帧淡出。
class MediaIsland::VideoCard : public views::View,
                               public views::ViewTargeterDelegate {
  METADATA_HEADER(VideoCard, views::View)

 public:
  static constexpr int kMargin = 18;

  VideoCard(MediaIsland* island, MediaPip* pip) : island_(island), pip_(pip) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetWindowDraggable(this, false);
    clip_ = AddChildView(std::make_unique<views::View>());
    clip_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    clip_->layer()->SetMasksToBounds(true);
    clip_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(kVideoRadius));
    clip_->layer()->SetIsFastRoundedCorner(true);
    video_ = clip_->AddChildView(std::make_unique<MediaVideoView>(
        pip->GetWeakPtr(), MediaVideoView::Place::kInline));
    SetWindowDraggable(video_, false);
    hint_ = clip_->AddChildView(std::make_unique<PopOutHint>());
  }

  MediaVideoView* video() const { return video_; }
  MediaPip* pip() const { return pip_; }
  void Detach() {
    pip_ = nullptr;
    SetCanProcessEventsWithinSubtree(false);
    video_->SetHovered(false);
  }
  LayerMotion& fade() { return fade_; }

  void SetSurface(const gfx::Rect& surface) {
    gfx::Rect bounds = surface;
    bounds.Outset(kMargin);
    SetBoundsRect(bounds);
    clip_->SetBounds(kMargin, kMargin, surface.width(), surface.height());
    video_->SetBoundsRect(clip_->GetLocalBounds());
    hint_->SetBoundsRect(clip_->GetLocalBounds());
  }
  // 拖到窗口外面、松手会弹到桌面：画面上的提示淡入。visible 是画面看得见的部分
  // （画面的坐标）。
  void SetPopOutHint(bool armed, const gfx::Rect& visible) {
    hint_->SetVisibleArea(visible);
    if (armed == hint_armed_) {
      return;
    }
    hint_armed_ = armed;
    hint_fade_.AnimateOpacity(hint_->layer(), armed ? 1 : 0,
                              MotionCurve::EaseOut(motion::kMediaFade));
  }
  bool hint_armed() const { return hint_armed_; }
  gfx::Rect surface() const {
    gfx::Rect rect = GetLocalBounds();
    rect.Inset(kMargin);
    return rect;
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF s(surface());
    if (s.IsEmpty()) {
      return;
    }
    cc::PaintFlags shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(SK_ColorBLACK);
    shadow.setLooper(gfx::CreateShadowDrawLooper(
        {gfx::ShadowValue(gfx::Vector2d(0, 8), 22,
                          SkColorSetA(SK_ColorBLACK, 0.32f * 255)),
         gfx::ShadowValue(gfx::Vector2d(0, 1), 3,
                          SkColorSetA(SK_ColorBLACK, 0.2f * 255))}));
    canvas->DrawRoundRect(s, kVideoRadius, shadow);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton()) {
      return false;
    }
    press_ = ScreenPoint(this, event.location());
    dragging_ = false;
    if (event.GetClickCount() == 2 && pip_) {
      pip_->BackToTab();
    }
    return true;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    last_ = ScreenPoint(this, event.location());
    if (!dragging_ && ExceededDragThreshold(last_ - press_)) {
      dragging_ = true;
      island_->BeginDrag(press_);
    }
    if (dragging_) {
      island_->DragTo(last_);
    }
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (dragging_) {
      dragging_ = false;
      island_->EndDrag(ScreenPoint(this, event.location()));
    }
  }
  void OnMouseCaptureLost() override {
    if (dragging_) {
      dragging_ = false;
      island_->EndDrag(last_);
    }
  }

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override {
    return surface().Intersects(rect);
  }

 private:
  const raw_ptr<MediaIsland> island_;
  raw_ptr<MediaPip> pip_;
  raw_ptr<views::View> clip_ = nullptr;
  raw_ptr<MediaVideoView> video_ = nullptr;
  raw_ptr<PopOutHint> hint_ = nullptr;
  LayerMotion hint_fade_;
  bool hint_armed_ = false;
  LayerMotion fade_;
  gfx::Point press_;
  gfx::Point last_;
  bool dragging_ = false;
};

BEGIN_METADATA(MediaIsland, VideoCard)
END_METADATA

MediaIsland::MediaIsland(StarryBrowserView* browser_view)
    : browser_view_(browser_view),
      hub_(MediaHub::Get(browser_view->browser()->GetProfile())),
      expand_(
          0,
          base::BindRepeating(&MediaIsland::Relayout, base::Unretained(this))),
      flight_(0,
              base::BindRepeating(&MediaIsland::OnFlightStep,
                                  base::Unretained(this))),
      video_flight_(1,
                    base::BindRepeating(&MediaIsland::OnVideoFlightStep,
                                        base::Unretained(this))),
      snap_(
          0,
          base::BindRepeating(&MediaIsland::Relayout, base::Unretained(this))),
      lift_(
          0,
          base::BindRepeating(&MediaIsland::Relayout, base::Unretained(this))),
      avoid_(
          0,
          base::BindRepeating(&MediaIsland::Relayout, base::Unretained(this))),
      capsule_in_(
          1,
          base::BindRepeating(&MediaIsland::Relayout, base::Unretained(this))),
      watcher_(std::make_unique<EventWatcher>(this)) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  glass_ = AddChildView(std::make_unique<GlassView>());
  SetWindowDraggable(glass_, false);
  tint_ = AddChildView(std::make_unique<Tint>());
  MediaPlayerView::Options options;
  options.placement_glyph = MediaGlyph::kDockSidebar;
  options.placement_tooltip = L("media.dockSidebar");
  options.placement = base::BindRepeating(
      [](MediaIsland* island) {
        // 换了样子以后这个按钮所在的卡片会收起：不在它自己的回调里改。
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](base::WeakPtr<MediaIsland> island) {
                  if (island) {
                    prefs::PrefsFor(
                        island->browser_view_->browser()->GetProfile())
                        ->SetBoolean(prefs::kMediaDocked, true);
                  }
                },
                island->weak_factory_.GetWeakPtr()));
      },
      base::Unretained(this));
  options.collapse = base::BindRepeating(
      [](MediaIsland* island) { island->SetExpanded(false); },
      base::Unretained(this));
  options.size_changed =
      base::BindRepeating(&MediaIsland::Relayout, base::Unretained(this));
  player_ = AddChildView(std::make_unique<MediaPlayerView>(hub_, options));
  player_->SetPaintToLayer();
  player_->layer()->SetFillsBoundsOpaquely(false);
  SetWindowDraggable(player_, false);
  capsule_ = AddChildView(std::make_unique<Capsule>(this));
  for (size_t i = 0; i < kMaxSatellites; ++i) {
    satellites_.push_back(AddChildView(std::make_unique<Satellite>(this)));
  }
  SetVisible(false);
  layer()->SetOpacity(0);

  PrefService* prefs = prefs::PrefsFor(browser_view->browser()->GetProfile());
  prefs_.Init(prefs);
  for (const char* name :
       {prefs::kMediaControlsEnabled, prefs::kMediaAutoPip}) {
    prefs_.Add(name, base::BindRepeating(&MediaIsland::OnMediaSettingsChanged,
                                         base::Unretained(this)));
  }
  prefs_.Add(prefs::kMediaDocked,
             base::BindRepeating(&MediaIsland::OnDockedChanged,
                                 base::Unretained(this)));
  prefs_.Add(prefs::kMediaCorner,
             base::BindRepeating(&MediaIsland::OnCornerChanged,
                                 base::Unretained(this)));
  prefs_.Add(prefs::kSidebarLayout,
             base::BindRepeating(&MediaIsland::Update, base::Unretained(this)));
  docked_ = Docked();
  corner_ =
      std::clamp(prefs->GetInteger(prefs::kMediaCorner), 0, kRight | kBottom);
  browser_view->browser()->GetTabStripModel()->AddObserver(this);
  observation_.Observe(hub_);
  MediaPip::AddHost(this);
  Update();
}

MediaIsland::~MediaIsland() {
  // 窗口要关了：放在这里的画中画关掉（标签跟着窗口一起关）。
  MediaPip::RemoveHost(this);
  pip_ = nullptr;
  monitor_.reset();
}

void MediaIsland::SetTheme(const Theme& theme) {
  if (theme_ == theme) {
    return;
  }
  theme_ = theme;
  GlassStyle style;
  style.dark = theme.is_dark;
  style.luma = theme.is_dark ? 0.1f : 0.9f;
  glass_->SetStyle(style);
  player_->SetTheme(theme);
  Update();
}

void MediaIsland::SetCardBounds(const gfx::Rect& card) {
  // 卡片不变时，紧凑地址栏的高度仍可能改变网页的裁剪范围。
  UpdateVideoStacking();
  if (card_ == card) {
    return;
  }
  card_ = card;
  Update();
}

bool MediaIsland::Docked() const {
  return prefs::PrefsFor(browser_view_->browser()->GetProfile())
      ->GetBoolean(prefs::kMediaDocked);
}

std::optional<base::UnguessableToken> MediaIsland::ActiveTabMedia() const {
  TabStripModel* model = browser_view_->browser()->GetTabStripModel();
  content::WebContents* contents =
      model ? model->GetActiveWebContents() : nullptr;
  if (!contents) {
    return std::nullopt;
  }
  return content::MediaSession::GetRequestIdFromWebContents(contents);
}

std::optional<base::UnguessableToken> MediaIsland::VideoMediaId() const {
  content::WebContents* contents = pip_ ? pip_->web_contents() : nullptr;
  if (!contents) {
    return std::nullopt;
  }
  return content::MediaSession::GetRequestIdFromWebContents(contents);
}

std::optional<base::UnguessableToken> MediaIsland::DesktopVideoMediaId() const {
  MediaPip* pip = MediaPip::Current();
  content::WebContents* contents =
      pip && !pip->is_inline() ? pip->web_contents() : nullptr;
  if (!contents) {
    return std::nullopt;
  }
  return content::MediaSession::GetRequestIdFromWebContents(contents);
}

bool MediaIsland::ShowsVideo() const {
  // 画中画开着就一直显示画面：点别的卫星是在旁边展开播放器，不把视频藏起来。
  return pip_ && video_card_;
}

void MediaIsland::SelectMedia(const base::UnguessableToken& id) {
  if (dismissed_ == id) {
    dismissed_.reset();
  }
  if (ShowsVideo()) {
    SetExpanded(true);
    if (expanded_) {
      player_->Select(id);
    }
    return;
  }
  selected_ = id;
  Update();
}

void MediaIsland::UpdateDismissed() {
  if (!dismissed_) {
    return;
  }
  const MediaEntry* entry = hub_->Find(*dismissed_);
  if (!entry) {
    dismissed_.reset();
  } else if (!entry->playing) {
    dismissed_paused_ = true;
  } else if (dismissed_paused_) {
    // 暂停以后又放起来了。
    dismissed_.reset();
  }
}

void MediaIsland::VideoClosedByUser(content::WebContents* contents) {
  dismissed_ = content::MediaSession::GetRequestIdFromWebContents(contents);
  dismissed_paused_ = false;
}

void MediaIsland::DesktopVideoMoved() {
  UpdateAvoidance();
}

const MediaEntry* MediaIsland::Shown() const {
  const std::optional<base::UnguessableToken> active = ActiveTabMedia();
  const std::optional<base::UnguessableToken> desktop = DesktopVideoMediaId();
  const std::optional<base::UnguessableToken> dismissed = dismissed_;
  const auto usable = [&active, &desktop, &dismissed](const MediaEntry* entry) {
    return entry && (!active || entry->id != *active) &&
           (!desktop || entry->id != *desktop) &&
           (!dismissed || entry->id != *dismissed);
  };
  if (selected_) {
    const MediaEntry* entry = hub_->Find(*selected_);
    if (usable(entry)) {
      return entry;
    }
  }
  if (const MediaEntry* primary = hub_->Primary(); usable(primary)) {
    return primary;
  }
  for (const MediaEntry* entry : hub_->Entries()) {
    if (usable(entry)) {
      return entry;
    }
  }
  return nullptr;
}

void MediaIsland::OnMediaChanged() {
  Update();
}

void MediaIsland::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (pip_ && change.type() == TabStripModelChange::kRemoved) {
    // 视频的标签关了、拖到别的窗口了：画中画关掉，画面回到网页。
    for (const auto& removed : change.GetRemove()->contents) {
      if (removed.contents == pip_->web_contents()) {
        pip_->ReturnToPage();
        break;
      }
    }
  }
  if (selection.active_tab_changed()) {
    content::WebContents* old_contents = selection.old_contents;
    content::WebContents* new_contents = selection.new_contents;
    if (dismissed_ && new_contents &&
        content::MediaSession::GetRequestIdFromWebContents(new_contents) ==
            *dismissed_) {
      // 切到关掉的那个标签了：之后切走时照常。
      dismissed_.reset();
    }
    if (pip_ && new_contents && new_contents == pip_->web_contents()) {
      ReturnVideo();
    } else if (pip_ && returning_ && old_contents == pip_->web_contents()) {
      // 飞回去的路上又切走了：掉头飞回角上。
      returning_ = false;
      video_flight_.AnimateTo(1, MotionCurve::Of(motion::kMediaVideoToIsland),
                              kVideoFlightEpsilon);
    } else if (old_contents && old_contents != new_contents) {
      MaybeInlineVideo(old_contents);
    }
    Update();
  }
}

bool MediaIsland::CanHostVideo(content::WebContents* contents) const {
  // 收起时也接：收起只管音乐，视频照样浮在角上。
  if (!contents || card_.IsEmpty() || !GetWidget()) {
    return false;
  }
  if (!prefs::PrefsFor(browser_view_->browser()->GetProfile())
           ->GetBoolean(prefs::kMediaControlsEnabled)) {
    return false;
  }
  TabStripModel* model = browser_view_->browser()->GetTabStripModel();
  return model &&
         model->GetIndexOfWebContents(contents) != TabStripModel::kNoTab &&
         model->GetActiveWebContents() != contents;
}

void MediaIsland::MaybeInlineVideo(content::WebContents* contents) {
  if (!CanHostVideo(contents) || !MediaPip::AutomaticEnabled(contents)) {
    return;
  }
  // 岛上已经有一段：只有自动进来、已经暂停的让给刚切走的这一个（新的画中画会先关掉它），
  // 在放的、手动放上来的不动。
  const bool replace = pip_ && pip_->automatically_entered() &&
                       !pip_->playing() && pip_->web_contents() != contents;
  if (pip_ && !replace) {
    return;
  }
  // 已经有别的画中画（桌面小窗、文档画中画）时不抢：整个浏览器只能有一个。
  if (MediaPip::ForWebContents(contents) ||
      PictureInPictureWindowManager::GetInstance()->GetChildWebContents()) {
    return;
  }
  const std::optional<base::UnguessableToken> replaced =
      replace ? VideoMediaId() : std::nullopt;
  for (const MediaEntry* entry : hub_->Entries()) {
    if (entry->in_picture_in_picture && entry->id != replaced) {
      return;
    }
  }
  const MediaEntry* entry =
      hub_->Find(content::MediaSession::GetRequestIdFromWebContents(contents));
  if (!entry || !entry->playing || !entry->has_video ||
      !entry->Can(MediaSessionAction::kEnterPictureInPicture)) {
    return;
  }
  content::MediaSession* session = content::MediaSession::Get(contents);
  if (!session) {
    return;
  }
  MediaPip::RequestInline(contents, this, /*automatic=*/true);
  session->EnterPictureInPicture();
}

void MediaIsland::AttachVideo(MediaPip* pip) {
  if (pip_ && pip_ != pip) {
    DetachVideo(pip_);
  }
  // 还是胶囊的时候：展开着的播放器在角上收回，胶囊在原处淡出，画面从网页飞过来。
  const bool capsule_shown =
      shown_ && !flying_ && !ShowsVideo() && capsule_->GetVisible();
  capsule_exit_ = capsule_shown ? CompactRect() : gfx::Rect();
  CollapseInPlace();
  if (capsule_shown) {
    capsule_in_.AnimateTo(0, MotionCurve::EaseOut(motion::kMediaFade));
  } else {
    capsule_in_.SetValue(0);
  }
  pip_ = pip;
  selected_.reset();
  if (dismissed_ == VideoMediaId()) {
    dismissed_.reset();
  }
  returning_ = false;
  hover_timer_.Stop();
  leave_timer_.Stop();
  // 在玻璃和播放器下面：窗口矮的时候展开的播放器会压到画面上。
  video_card_ = AddChildViewAt(std::make_unique<VideoCard>(this, pip),
                               GetIndexOf(glass_).value());
  UpdateVideoStacking();
  pip->SetView(video_card_->video());
  // 画面从网页里原来的位置飞过来；岛本身不再淡入（不然开头看不见）。
  // 媒体会话可能先让胶囊开始入场；先停掉父层的缩放，免得坐标转换把它也算进去。
  fade_.SetTransform(layer(), LayerPose());
  video_source_ = VideoSourceRect();
  const bool fly = !video_source_.IsEmpty() && !motion::ReduceMotion();
  if (fly) {
    SetShown(true, /*animate=*/false);
    video_flight_.SetValue(0);
    video_flight_.AnimateTo(1, MotionCurve::Of(motion::kMediaVideoToIsland),
                            kVideoFlightEpsilon);
  } else {
    video_flight_.SetValue(1);
  }
  Update();
}

void MediaIsland::DetachVideo(MediaPip* pip) {
  if (pip_ != pip) {
    return;
  }
  if (expanded_) {
    // 摘在视频上面的播放器在原处收回（不跳到角上），里面选的那个留在岛上。
    selected_ = player_->selected();
  }
  CollapseInPlace();
  pip_ = nullptr;
  VideoCard* card = video_card_;
  video_card_ = nullptr;
  video_flight_.SetValue(1);
  video_leave_timer_.Stop();
  pop_out_armed_ = false;
  // 主体回到胶囊时：它和卫星在角上淡入（画面同时淡出）。
  capsule_exit_ = gfx::Rect();
  capsule_in_.SetValue(0);
  capsule_in_.AnimateTo(1, MotionCurve::EaseOut(motion::kMediaFade));
  if (card) {
    card->Detach();
    // 飞回网页落地了：马上淡掉（网页里的视频接着画）；别的情况缩一点再淡掉。
    if (!returning_ && !motion::ReduceMotion()) {
      LayerPose to;
      to.scale = 0.92f;
      const gfx::Rect s = card->surface();
      card->fade().AnimateTransform(
          card->layer(), to, MotionCurve::Of(motion::kLift),
          gfx::PointF(s.CenterPoint().x(), s.CenterPoint().y()));
    }
    card->fade().AnimateOpacity(
        card->layer(), 0,
        MotionCurve::EaseOut(returning_ ? motion::kMediaFade / 2
                                        : motion::kMediaFade),
        base::BindOnce(
            [](base::WeakPtr<MediaIsland> island, VideoCard* card) {
              if (island) {
                // 不在它自己的动画回调里删它。
                base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                    FROM_HERE, base::BindOnce(&MediaIsland::RemoveCard, island,
                                              base::Unretained(card)));
              }
            },
            weak_factory_.GetWeakPtr(), base::Unretained(card)));
  }
  returning_ = false;
  Update();
}

void MediaIsland::RemoveCard(VideoCard* card) {
  if (card == video_card_ || !Contains(card)) {
    return;
  }
  RemoveChildViewT(card);
  UpdateVideoStacking();
}

void MediaIsland::UpdateVideoStacking() {
  // 淡出的画面也要留在网页上一层，最后一张视频卡片移除后再恢复胶囊的层级。
  const bool has_video =
      std::ranges::any_of(children(), [](const views::View* child) {
        return views::IsViewClass<VideoCard>(child);
      });
  // 停靠侧栏的空白背景在窗口底层，单靠堆叠顺序挡不住飞出网页的画面。
  layer()->SetClipRect(
      has_video ? views::View::ConvertRectToTarget(browser_view_, this,
                                                   browser_view_->PageBounds())
                : gfx::Rect());
  browser_view_->UpdateMediaIslandStacking(has_video);
}

void MediaIsland::VideoChanged(MediaPip* pip) {
  if (pip == pip_) {
    Update();
  }
}

gfx::Rect MediaIsland::VideoSourceRect() const {
  if (!pip_) {
    return gfx::Rect();
  }
  gfx::Rect source = pip_->SourceBounds();
  if (source.IsEmpty()) {
    return gfx::Rect();
  }
  // 用视频所属标签的内容区。新标签此时可能还没布局，拿它的位置会让起点偏移；
  // 视频标签已经隐藏时 PageBoundsFor 会退回内容卡片的位置。
  const gfx::Rect page = views::View::ConvertRectToTarget(
      browser_view_, this, browser_view_->PageBoundsFor(pip_->web_contents()));
  source.Offset(page.OffsetFromOrigin());
  if (!source.Intersects(card_)) {
    return gfx::Rect();
  }
  return source;
}

void MediaIsland::ReturnVideo() {
  if (!pip_ || returning_) {
    return;
  }
  video_source_ = VideoSourceRect();
  if (video_source_.IsEmpty() || motion::ReduceMotion() || !shown_ ||
      !ShowsVideo()) {
    returning_ = true;
    pip_->ReturnToPage();
    return;
  }
  returning_ = true;
  video_card_->video()->SetHovered(false);
  video_flight_.AnimateTo(0, MotionCurve::Of(motion::kMediaVideoToPage),
                          kVideoFlightEpsilon);
}

void MediaIsland::OnVideoFlightStep() {
  // 先画完最后一帧，淡出的画面才能停在播放器的准确位置。
  Relayout();
  if (returning_ && pip_ && !video_flight_.is_animating() &&
      video_flight_.value() <= 0.001) {
    // 落地了：关掉画中画，网页里的视频接着画。
    pip_->ReturnToPage();
  }
}

void MediaIsland::Update() {
  UpdateDismissed();
  docked_ = Docked();
  const bool video = ShowsVideo();
  const std::optional<base::UnguessableToken> video_id = VideoMediaId();
  // 收起时音乐在坞里：岛上只放视频。
  const MediaEntry* entry = video ? (video_id ? hub_->Find(*video_id) : nullptr)
                                  : (docked_ ? nullptr : Shown());
  if (!video && entry) {
    capsule_->Set(*entry, theme_);
    if (!expanded_) {
      player_->Select(entry->id);
    }
  }
  if (video_card_) {
    video_card_->SetVisible(video);
  }
  player_->SetExclude(ActiveTabMedia());
  // 卫星：其余的（不含当前标签里的、桌面小窗里的），最多 3 颗；收起时其余的在坞里。
  const std::optional<base::UnguessableToken> active = ActiveTabMedia();
  const std::optional<base::UnguessableToken> desktop = DesktopVideoMediaId();
  const std::optional<base::UnguessableToken> main =
      video ? video_id
            : (entry ? std::optional<base::UnguessableToken>(entry->id)
                     : std::nullopt);
  std::vector<const MediaEntry*> others;
  if (!docked_) {
    for (const MediaEntry* other : hub_->Entries()) {
      if ((!main || other->id != *main) && (!active || other->id != *active) &&
          (!desktop || other->id != *desktop)) {
        others.push_back(other);
      }
    }
  }
  for (size_t i = 0; i < satellites_.size(); ++i) {
    const bool used = i < others.size();
    satellites_[i]->SetVisible(used);
    if (used) {
      satellites_[i]->Set(*others[i], theme_);
    }
  }
  if (!flying_) {
    SetShown((video || entry) && !card_.IsEmpty() &&
             prefs::PrefsFor(browser_view_->browser()->GetProfile())
                 ->GetBoolean(prefs::kMediaControlsEnabled));
  }
  if (!video && !entry && expanded_) {
    SetExpanded(false);
  }
  UpdateAvoidance();
  Relayout();
}

void MediaIsland::SetShown(bool shown, bool animate) {
  if (shown && !animate) {
    // 胶囊已经在入场时也要清掉动画，让视频从播放器的原位、完整不透明地出发。
    fade_.SetTransform(layer(), LayerPose());
    fade_.SetOpacity(layer(), 1);
  }
  if (shown_ == shown) {
    return;
  }
  shown_ = shown;
  if (shown) {
    SetVisible(true);
    if (animate && motion::ReduceMotion()) {
      fade_.AnimateOpacity(layer(), 1,
                           MotionCurve::EaseOut(motion::kMediaFade));
    } else if (animate) {
      LayerPose from;
      from.scale = 0.96f;
      from.offset = gfx::Vector2dF(0, (corner_ & kBottom) ? 8 : -8);
      const gfx::Rect main = MainRect();
      fade_.AnimateTransformFrom(
          layer(), from, LayerPose(), MotionCurve::Of(motion::kLift),
          gfx::PointF((corner_ & kRight) ? main.right() : main.x(),
                      (corner_ & kBottom) ? main.bottom() : main.y()));
      fade_.AnimateOpacityFrom(layer(), layer()->opacity(), 1,
                               MotionCurve::EaseOut(motion::kMediaFade));
    }
  } else {
    hover_timer_.Stop();
    leave_timer_.Stop();
    expanded_ = false;
    fade_.AnimateOpacity(
        layer(), 0, MotionCurve::EaseOut(motion::kMediaFade),
        base::BindOnce(
            [](base::WeakPtr<MediaIsland> island) {
              if (island && !island->shown_ && !island->flying_) {
                island->SetVisible(false);
                island->expand_.SetValue(0);
              }
            },
            weak_factory_.GetWeakPtr()));
  }
  UpdateMonitor();
}

void MediaIsland::UpdateMonitor() {
  if (shown_ && GetWidget() && !monitor_) {
    monitor_ = views::EventMonitor::CreateWindowMonitor(
        watcher_.get(), GetWidget()->GetNativeWindow(),
        {ui::EventType::kMousePressed, ui::EventType::kMouseMoved,
         ui::EventType::kMouseExited});
  } else if (!shown_ && monitor_) {
    monitor_.reset();
  }
}

void MediaIsland::AddedToWidget() {
  UpdateMonitor();
}

void MediaIsland::RemovedFromWidget() {
  monitor_.reset();
}

void MediaIsland::SetExpanded(bool expanded) {
  hover_timer_.Stop();
  leave_timer_.Stop();
  if (expanded && (!shown_ || flying_ || dragging_)) {
    return;
  }
  if (expanded_ == expanded) {
    return;
  }
  expanded_ = expanded;
  if (expanded) {
    player_exit_.reset();
    if (const MediaEntry* entry = Shown()) {
      player_->Select(entry->id);
    }
    expand_.AnimateTo(1, MotionCurve::Of(motion::kMediaIslandExpand));
  } else {
    // 在展开的卡片里换过的那个留在岛上。
    selected_ = player_->selected();
    expand_.AnimateTo(0, MotionCurve::Of(motion::kMediaIslandCollapse));
    Update();
  }
  Relayout();
}

void MediaIsland::CollapseInPlace() {
  hover_timer_.Stop();
  leave_timer_.Stop();
  if (!expanded_ && expand_.value() <= 0.01) {
    return;
  }
  player_exit_ = std::make_pair(CollapsedRect(), ExpandedRect());
  expanded_ = false;
  expand_.AnimateTo(0, MotionCurve::Of(motion::kMediaIslandCollapse));
}

// 位置 -----------------------------------------------------------------------

gfx::Rect MediaIsland::Anchored(const gfx::Size& size) const {
  const bool right = corner_ & kRight;
  const bool bottom = corner_ & kBottom;
  const int avoid = static_cast<int>(std::lround(avoid_.value()));
  const int x =
      right ? card_.right() - kInset - size.width() : card_.x() + kInset;
  const int y = bottom ? card_.bottom() - kInset - size.height() - avoid
                       : card_.y() + kInset + avoid;
  return gfx::Rect(x, y, size.width(), size.height());
}

gfx::Vector2d MediaIsland::MoveOffset() const {
  if (dragging_) {
    return drag_offset_;
  }
  const double t = snap_.value();
  return gfx::Vector2d(static_cast<int>(std::lround(snap_offset_.x() * t)),
                       static_cast<int>(std::lround(snap_offset_.y() * t)));
}

gfx::Rect MediaIsland::CompactRect() const {
  return Anchored(gfx::Size(capsule_->PreferredWidth(), kCapsuleHeight)) +
         MoveOffset();
}

gfx::Rect MediaIsland::ExpandedRect() const {
  const int height = player_->ContentHeight() + 2 * kPadding;
  const int width = std::min(kExpandedWidth, card_.width() - 2 * kInset);
  gfx::Rect rect = Anchored(gfx::Size(width, height)) + MoveOffset();
  if (ShowsVideo()) {
    // 视频在角上：播放器摘在它朝里的一边（下面的角在它上面，上面的角在它下面），不盖
    // 画面；卡片太矮时最多顶到卡片的边。
    const gfx::Rect video = VideoRect();
    const int y = (corner_ & kBottom) ? video.y() - kStackGap - height
                                      : video.bottom() + kStackGap;
    const int top = card_.y() + kInset;
    rect.set_y(
        std::clamp(y, top, std::max(top, card_.bottom() - kInset - height)));
  }
  return rect;
}

gfx::Rect MediaIsland::CollapsedRect() const {
  if (!ShowsVideo()) {
    return CompactRect();
  }
  const gfx::Rect expanded = ExpandedRect();
  return gfx::Rect(
      expanded.x(),
      (corner_ & kBottom) ? expanded.bottom() - kCapsuleHeight : expanded.y(),
      expanded.width(), kCapsuleHeight);
}

gfx::Rect MediaIsland::ExpandedArea() const {
  gfx::Rect area = ExpandedRect();
  if (ShowsVideo()) {
    area.Union(FootprintAt(VideoRect()));
  }
  return area;
}

gfx::Size MediaIsland::VideoSize() const {
  gfx::Size natural = pip_ ? pip_->natural_size() : gfx::Size();
  if (natural.IsEmpty()) {
    natural = gfx::Size(16, 9);
  }
  const double ratio = std::clamp(
      static_cast<double>(natural.width()) / natural.height(), 9.0 / 16, 2.4);
  double width = ratio >= 1 ? kVideoWidth : kVideoTallHeight * ratio;
  double height = width / ratio;
  // 窄的窗口里不超过卡片的一半宽。
  const double room = std::max(160, card_.width() / 2);
  if (width > room) {
    height *= room / width;
    width = room;
  }
  return gfx::Size(static_cast<int>(std::lround(width)),
                   static_cast<int>(std::lround(height)));
}

gfx::Rect MediaIsland::VideoRect() const {
  return Anchored(VideoSize()) + MoveOffset();
}

gfx::Rect MediaIsland::MainRect() const {
  return ShowsVideo() ? VideoRect() : CompactRect();
}

gfx::Rect MediaIsland::FootprintAt(const gfx::Rect& main) const {
  size_t count = 0;
  for (const Satellite* satellite : satellites_) {
    if (satellite->GetVisible()) {
      ++count;
    }
  }
  if (count == 0) {
    return main;
  }
  gfx::Rect footprint = main;
  // 视频的卫星竖着一列，胶囊的横着一排。
  const int extent =
      ShowsVideo() ? kSatellite + kSatelliteGap
                   : static_cast<int>(count) * (kSatellite + kSatelliteGap);
  if (corner_ & kRight) {
    footprint.set_x(main.x() - extent);
  }
  footprint.set_width(main.width() + extent);
  return footprint;
}

gfx::Rect MediaIsland::DockRect() const {
  gfx::Rect dock = browser_view_->MediaDockBounds();
  if (dock.IsEmpty()) {
    // 坞不在屏幕上（侧栏自动隐藏、收起了）：飞到窗口左边缘。
    const gfx::Rect compact = CompactRect();
    return gfx::Rect(-compact.width() / 2, compact.y(), compact.width() / 2,
                     compact.height());
  }
  return views::View::ConvertRectToTarget(browser_view_, this, dock);
}

gfx::Rect MediaIsland::SurfaceRect() const {
  if (!GetVisible()) {
    return gfx::Rect();
  }
  if (flying_ && !ShowsVideo()) {
    return capsule_->surface() + capsule_->bounds().OffsetFromOrigin();
  }
  if (ShowsVideo()) {
    return VideoRect();
  }
  return expand_.value() > 0.5 ? ExpandedRect() : CompactRect();
}

// 让位 ------------------------------------------------------------------------

void MediaIsland::UpdateAvoidance() {
  if (card_.IsEmpty() || dragging_) {
    return;
  }
  const bool bottom = corner_ & kBottom;
  // 不算让位、不算拖动时在角上的位置。
  const gfx::Size size =
      ShowsVideo() ? VideoSize()
                   : gfx::Size(capsule_->PreferredWidth(), kCapsuleHeight);
  const int right_x = card_.right() - kInset - size.width();
  const gfx::Rect base(
      (corner_ & kRight) ? right_x : card_.x() + kInset,
      bottom ? card_.bottom() - kInset - size.height() : card_.y() + kInset,
      size.width(), size.height());
  const gfx::Rect footprint = FootprintAt(base);
  std::vector<gfx::Rect> obstacles;
  for (const gfx::Rect& rect : browser_view_->MediaObstacles()) {
    obstacles.push_back(
        views::View::ConvertRectToTarget(browser_view_, this, rect));
  }
  int shift = 0;
  // 挪开一个以后可能又碰上另一个：最多挪几次。
  for (size_t round = 0; round <= obstacles.size(); ++round) {
    gfx::Rect at = footprint;
    at.Offset(0, bottom ? -shift : shift);
    bool moved = false;
    for (const gfx::Rect& obstacle : obstacles) {
      gfx::Rect grown = obstacle;
      grown.Outset(kAvoidGap);
      if (!grown.Intersects(at)) {
        continue;
      }
      shift = bottom ? footprint.bottom() - grown.y()
                     : grown.bottom() - footprint.y();
      moved = true;
      at = footprint;
      at.Offset(0, bottom ? -shift : shift);
    }
    if (!moved) {
      break;
    }
  }
  // 最多挪到卡片的另一头。
  shift = std::clamp(shift, 0,
                     std::max(0, card_.height() - 2 * kInset - size.height()));
  if (shift == avoid_target_) {
    return;
  }
  avoid_target_ = shift;
  if (!shown_ || motion::ReduceMotion()) {
    avoid_.SetValue(shift);
    Relayout();
  } else {
    avoid_.AnimateTo(shift, MotionCurve::Of(motion::kMediaIslandAvoid));
  }
}

// 拖动 ------------------------------------------------------------------------

void MediaIsland::BeginDrag(const gfx::Point& screen_point) {
  if (!shown_ || flying_ || expanded_ || returning_) {
    return;
  }
  hover_timer_.Stop();
  // 接着吸附到一半的位置拖。
  drag_base_ = MoveOffset();
  drag_offset_ = drag_base_;
  dragging_ = true;
  pop_out_armed_ = false;
  drag_start_ = screen_point;
  snap_.SetValue(0);
  snap_offset_ = gfx::Vector2d();
  if (!motion::ReduceMotion()) {
    lift_.AnimateTo(1, MotionCurve::Of(motion::kLift));
  }
  Relayout();
}

void MediaIsland::DragTo(const gfx::Point& screen_point) {
  if (!dragging_) {
    return;
  }
  drag_offset_ = drag_base_ + (screen_point - drag_start_);
  Relayout();
  UpdatePopOutHint(screen_point);
}

void MediaIsland::UpdatePopOutHint(const gfx::Point& screen_point) {
  if (!dragging_ || !ShowsVideo() || !GetWidget()) {
    pop_out_armed_ = false;
    if (video_card_) {
      video_card_->SetPopOutHint(false, gfx::Rect());
    }
    return;
  }
  const gfx::Rect window = GetWidget()->GetWindowBoundsInScreen();
  gfx::Rect arm = window;
  arm.Outset(kPopOutSlack);
  pop_out_armed_ = pop_out_armed_ ? !window.Contains(screen_point)
                                  : !arm.Contains(screen_point);
  // 提示放在画面还留在网页里的那部分中间（出了网页的部分被裁掉了）。
  const gfx::Rect video = VideoRect();
  gfx::Rect visible = video;
  visible.Intersect(views::View::ConvertRectToTarget(
      browser_view_, this, browser_view_->PageBounds()));
  visible.Offset(-video.OffsetFromOrigin());
  video_card_->SetPopOutHint(pop_out_armed_, visible);
}

void MediaIsland::EndDrag(const gfx::Point& screen_point) {
  if (!dragging_) {
    return;
  }
  drag_offset_ = drag_base_ + (screen_point - drag_start_);
  UpdatePopOutHint(screen_point);
  const bool pop_out = pop_out_armed_ && ShowsVideo() && pip_;
  const gfx::Rect dropped = MainRect();
  dragging_ = false;
  pop_out_armed_ = false;
  lift_.AnimateTo(0, MotionCurve::Of(motion::kLift));
  // 视频拖出了窗口（提示亮着）：弹到桌面，从松手的地方出现。提示跟着画面一起淡出。
  if (pop_out) {
    gfx::Rect screen_rect = dropped;
    views::View::ConvertRectToScreen(this, &screen_rect);
    snap_.SetValue(0);
    snap_offset_ = gfx::Vector2d();
    pip_->PopOut(screen_rect);
    return;
  }
  if (video_card_) {
    video_card_->SetPopOutHint(false, gfx::Rect());
  }
  // 吸到最近的角：中心在卡片的哪一半。
  const gfx::Point center = dropped.CenterPoint();
  const int corner = (center.x() >= card_.CenterPoint().x() ? kRight : 0) |
                     (center.y() >= card_.CenterPoint().y() ? kBottom : 0);
  if (corner != corner_) {
    corner_ = corner;
    avoid_.SetValue(0);
    avoid_target_ = 0;
    // 别的窗口跟着换（OnCornerChanged 看到已经是这个角，不再动这个窗口）。
    prefs::PrefsFor(browser_view_->browser()->GetProfile())
        ->SetInteger(prefs::kMediaCorner, corner);
    UpdateAvoidance();
  }
  snap_offset_ = dropped.origin() - (MainRect() - MoveOffset()).origin();
  snap_.SetValue(1);
  snap_.AnimateTo(0, MotionCurve::Of(motion::kMediaIslandSnap));
  Relayout();
}

void MediaIsland::OnCornerChanged() {
  const int corner =
      std::clamp(prefs::PrefsFor(browser_view_->browser()->GetProfile())
                     ->GetInteger(prefs::kMediaCorner),
                 0, kRight | kBottom);
  if (corner == corner_) {
    return;
  }
  // 别的窗口里拖过去了：从原来的角滑过去。
  const gfx::Rect from = MainRect();
  corner_ = corner;
  avoid_.SetValue(0);
  avoid_target_ = 0;
  UpdateAvoidance();
  if (shown_ && !motion::ReduceMotion()) {
    snap_offset_ = from.origin() - (MainRect() - MoveOffset()).origin();
    snap_.SetValue(1);
    snap_.AnimateTo(0, MotionCurve::Of(motion::kMediaIslandSnap));
  }
  Relayout();
}

// 收起、浮出
// --------------------------------------------------------------------

void MediaIsland::OnMediaSettingsChanged() {
  PrefService* prefs = prefs::PrefsFor(browser_view_->browser()->GetProfile());
  const bool enabled = prefs->GetBoolean(prefs::kMediaControlsEnabled);
  if (!enabled) {
    flying_ = false;
    flight_.SetValue(0);
    capsule_fade_.SetOpacity(capsule_->layer(), 1);
  }
  if (pip_ && (!enabled || (pip_->automatically_entered() &&
                            !prefs->GetBoolean(prefs::kMediaAutoPip)))) {
    // 只结束自动进入的画中画；手动弹出的桌面小窗、网页播放不受影响。
    pip_->ReturnToPage();
  }
  Update();
}

void MediaIsland::OnDockedChanged() {
  const bool docked = Docked();
  if (docked == docked_) {
    return;
  }
  docked_ = docked;
  // 视频在角上时它不动：音乐进坞、从坞里出来只是旁边的卫星跟着变（坞自己淡入、淡出）。
  const bool can_fly = prefs::PrefsFor(browser_view_->browser()->GetProfile())
                           ->GetBoolean(prefs::kMediaControlsEnabled) &&
                       !motion::ReduceMotion() && !card_.IsEmpty() &&
                       GetWidget() && !ShowsVideo() && Shown() != nullptr;
  if (docked && shown_ && can_fly) {
    // 飞进坞：先收回成胶囊，外框沿弧线飞过去并变成坞的样子，快到时淡掉。
    expanded_ = false;
    expand_.SetValue(0);
    flying_ = true;
    flying_to_dock_ = true;
    flight_fading_ = false;
    flight_.SetValue(0);
    flight_.AnimateTo(1, MotionCurve::Of(motion::kMediaDockFlight));
  } else if (!docked && can_fly) {
    // 从坞里飞出来：起点是现在坞的位置（它马上要收起来）。
    flight_dock_ = DockRect();
    flying_ = true;
    flying_to_dock_ = false;
    shown_ = true;
    SetVisible(true);
    fade_.SetOpacity(layer(), 1);
    flight_fading_ = false;
    capsule_fade_.AnimateOpacityFrom(
        capsule_->layer(), 0, 1,
        MotionCurve::EaseOut(motion::kMediaDockFlightFade));
    flight_.SetValue(1);
    flight_.AnimateTo(0, MotionCurve::Of(motion::kMediaDockFlight));
    UpdateMonitor();
  } else {
    Update();
    return;
  }
  Relayout();
}

void MediaIsland::OnFlightStep() {
  const bool done = flight_.value() == flight_.target();
  if (flying_to_dock_ && flight_.value() > 0.82 && !flight_fading_) {
    // 快到了：和坞交叉淡化。
    flight_fading_ = true;
    capsule_fade_.AnimateOpacity(
        capsule_->layer(), 0,
        MotionCurve::EaseOut(motion::kMediaDockFlightFade));
    browser_view_->RevealMediaDock();
  }
  if (done) {
    flying_ = false;
    if (flying_to_dock_) {
      capsule_fade_.SetOpacity(capsule_->layer(), 1);
      // 飞的路上有视频上了岛：岛留着放视频。
      if (!ShowsVideo()) {
        shown_ = false;
        SetVisible(false);
        fade_.SetOpacity(layer(), 0);
        UpdateMonitor();
      }
    }
    Update();
  }
  Relayout();
}

// 排版 ------------------------------------------------------------------------

void MediaIsland::LayoutSatellites(const gfx::Rect& main,
                                   bool video,
                                   float opacity) {
  const bool right = corner_ & kRight;
  const bool bottom = corner_ & kBottom;
  // 胶囊：在它朝里的一边横着排、和它居中对齐；视频：在朝里的一边竖着一列，和它的外边
  // 对齐。
  int x = right ? main.x() - kSatelliteGap - kSatellite
                : main.right() + kSatelliteGap;
  int y = video ? (bottom ? main.bottom() - kSatellite : main.y())
                : main.CenterPoint().y() - kSatellite / 2;
  for (Satellite* satellite : satellites_) {
    if (!satellite->GetVisible()) {
      continue;
    }
    satellite->SetBounds(x, y, kSatellite, kSatellite);
    satellite->layer()->SetOpacity(opacity);
    satellite->SetCanProcessEventsWithinSubtree(!expanded_ && !dragging_);
    if (video) {
      y += bottom ? -(kSatellite + kSatelliteGap) : kSatellite + kSatelliteGap;
    } else {
      x += right ? -(kSatellite + kSatelliteGap) : kSatellite + kSatelliteGap;
    }
  }
}

void MediaIsland::Relayout() {
  if (card_.IsEmpty()) {
    return;
  }
  const float lift = 1 + static_cast<float>(motion::kMediaIslandLiftScale - 1) *
                             Clamp01(lift_.value());
  const auto lift_transform = [lift](const gfx::Rect& surface,
                                     const gfx::Point& origin) {
    gfx::Transform transform;
    if (lift != 1) {
      const gfx::PointF center(surface.CenterPoint().x() - origin.x(),
                               surface.CenterPoint().y() - origin.y());
      transform.Translate(center.x(), center.y());
      transform.Scale(lift, lift);
      transform.Translate(-center.x(), -center.y());
    }
    return transform;
  };

  if (flying_ && !ShowsVideo()) {
    const gfx::Rect compact = CompactRect();
    const double f = flight_.value();
    const gfx::Rect dock = flying_to_dock_ ? DockRect() : flight_dock_;
    gfx::Rect rect = Lerp(compact, dock, f);
    // 沿一段弧线：中间拱起来一点。
    rect.Offset(0, -static_cast<int>(std::lround(
                       std::sin(Clamp01(f) * std::numbers::pi) * kFlightArc)));
    capsule_->SetVisible(true);
    capsule_->layer()->SetTransform(gfx::Transform());
    capsule_->SetSurface(
        rect, kCapsuleRadius + (kDockRadius - kCapsuleRadius) * Clamp01(f));
    glass_->SetVisible(false);
    tint_->SetVisible(false);
    player_->SetVisible(false);
    for (Satellite* satellite : satellites_) {
      satellite->layer()->SetOpacity(0);
    }
    return;
  }

  const double e = expand_.value();
  // 展开时胶囊、卫星淡掉。
  const float fade_out = 1 - std::min(1.0f, Clamp01(e) * 3);
  const float capsule_in = Clamp01(capsule_in_.value());
  // 主体刚换过时播放器在原来的位置收回，收完以后按现在的主体展开。
  const auto layout_player = [this, e](const gfx::Rect& from) {
    if (!player_exit_) {
      LayoutPlayer(from, ExpandedRect(), e);
      return;
    }
    LayoutPlayer(player_exit_->first, player_exit_->second, e);
    if (!expanded_ && !expand_.is_animating()) {
      player_exit_.reset();
    }
  };

  if (ShowsVideo()) {
    // 视频：画面在角上；飞进来、飞回去时从网页里原来的位置缩放过来。点了卫星时播放器
    // 从画面朝里的那一边长出来。
    // 胶囊在视频上岛前的位置淡出。
    const bool capsule_leaving = capsule_in > 0.01f && !capsule_exit_.IsEmpty();
    capsule_->SetVisible(capsule_leaving);
    if (capsule_leaving) {
      capsule_->SetSurface(capsule_exit_, kCapsuleRadius);
      capsule_->layer()->SetTransform(gfx::Transform());
      if (!capsule_fade_.IsAnimatingOpacity()) {
        capsule_->layer()->SetOpacity(capsule_in * fade_out);
      }
      capsule_->SetCanProcessEventsWithinSubtree(false);
    }
    layout_player(CollapsedRect());
    const gfx::Rect rect = VideoRect();
    video_card_->SetSurface(rect);
    const gfx::Point origin = video_card_->origin();
    gfx::Transform transform = lift_transform(rect, origin);
    const double f = video_flight_.value();
    if ((video_flight_.is_animating() || f != 1) && !video_source_.IsEmpty()) {
      gfx::Rect at = Lerp(video_source_, rect, f);
      at.Offset(
          0, -static_cast<int>(std::lround(
                 std::sin(Clamp01(f) * std::numbers::pi) * kVideoFlightArc)));
      const float scale_x =
          static_cast<float>(at.width()) / std::max(1, rect.width());
      const float scale_y =
          static_cast<float>(at.height()) / std::max(1, rect.height());
      // 以画面的左上角为原点缩放，再挪到 at。
      const gfx::PointF corner(rect.x() - origin.x(), rect.y() - origin.y());
      gfx::Transform fly;
      fly.Translate(at.x() - rect.x(), at.y() - rect.y());
      fly.Translate(corner.x(), corner.y());
      fly.Scale(scale_x, scale_y);
      fly.Translate(-corner.x(), -corner.y());
      transform = fly;
    }
    video_card_->layer()->SetTransform(transform);
    // 飞的时候、展开时卫星淡掉。
    LayoutSatellites(rect, /*video=*/true, Clamp01((f - 0.6) / 0.4) * fade_out);
    return;
  }

  const gfx::Rect compact = CompactRect();
  layout_player(compact);
  // 胶囊在原处淡掉，内容过半才淡入；视频离开后和卫星一起淡入（capsule_in_）。
  capsule_->SetVisible(true);
  capsule_->SetSurface(compact, kCapsuleRadius);
  capsule_->layer()->SetTransform(lift_transform(compact, capsule_->origin()));
  if (!capsule_fade_.IsAnimatingOpacity()) {
    capsule_->layer()->SetOpacity(fade_out * capsule_in);
  }
  capsule_->SetCanProcessEventsWithinSubtree(!expanded_);
  // 卫星在胶囊旁边，展开时淡掉。
  LayoutSatellites(compact, /*video=*/false, fade_out * capsule_in);
}

void MediaIsland::LayoutPlayer(const gfx::Rect& from,
                               const gfx::Rect& to,
                               double e) {
  const float shown_e = Clamp01(e);
  const gfx::Rect& expanded = to;
  const gfx::Rect rect = Lerp(from, expanded, e);
  const float radius =
      kCapsuleRadius + (kExpandedRadius - kCapsuleRadius) * shown_e;
  const bool visible = e > 0.01;
  glass_->SetVisible(visible);
  tint_->SetVisible(visible);
  player_->SetVisible(visible);
  if (!visible) {
    return;
  }
  glass_->SetGlassBounds(rect);
  glass_->SetCornerRadius(radius, true);
  glass_->layer()->SetOpacity(std::min(1.0f, shown_e * 4));
  tint_->SetBoundsRect(rect);
  tint_->Set(theme_, player_->media_theme(), radius);
  tint_->layer()->SetOpacity(std::min(1.0f, shown_e * 4));
  gfx::Rect inner = expanded;
  inner.Inset(kPadding);
  player_->SetBoundsRect(inner);
  player_->layer()->SetOpacity(Clamp01((e - 0.45) / 0.4));
}

void MediaIsland::Layout(PassKey) {
  Relayout();
}

void MediaIsland::OnPaint(gfx::Canvas* canvas) {}

// 鼠标 ------------------------------------------------------------------------

void MediaIsland::OnWindowMouseMove(const gfx::Point& root_point) {
  if (!shown_ || flying_ || dragging_) {
    return;
  }
  gfx::Point point = root_point;
  views::View::ConvertPointFromWidget(this, &point);
  if (expanded_) {
    // 视频在角上时视频和摘在上面的播放器算一块：移到画面上不收回。
    gfx::Rect area = ExpandedArea();
    area.Outset(kLeaveSlack);
    if (area.Contains(point)) {
      leave_timer_.Stop();
    } else if (!leave_timer_.IsRunning()) {
      leave_timer_.Start(FROM_HERE,
                         base::Seconds(motion::kMediaIslandLeaveDelay),
                         base::BindOnce(&MediaIsland::SetExpanded,
                                        base::Unretained(this), false));
    }
    if (!ShowsVideo()) {
      return;
    }
  }
  if (ShowsVideo()) {
    // 视频：移上来马上出控件，离开一会儿再收。
    MediaVideoView* video = video_card_->video();
    if (VideoRect().Contains(point) && !returning_) {
      video_leave_timer_.Stop();
      video->SetHovered(true);
    } else if (video->hovered() && !video_leave_timer_.IsRunning()) {
      video_leave_timer_.Start(
          FROM_HERE, base::Seconds(motion::kMediaIslandLeaveDelay),
          base::BindOnce(
              [](MediaIsland* island) {
                if (island->video_card_) {
                  island->video_card_->video()->SetHovered(false);
                }
              },
              base::Unretained(this)));
    }
    return;
  }
  if (CompactRect().Contains(point)) {
    if (!hover_timer_.IsRunning()) {
      hover_timer_.Start(FROM_HERE,
                         base::Seconds(motion::kMediaIslandHoverDelay),
                         base::BindOnce(&MediaIsland::SetExpanded,
                                        base::Unretained(this), true));
    }
  } else {
    hover_timer_.Stop();
  }
}

void MediaIsland::OnWindowMousePress(const gfx::Point& root_point) {
  if (!expanded_) {
    return;
  }
  gfx::Point point = root_point;
  views::View::ConvertPointFromWidget(this, &point);
  if (!ExpandedArea().Contains(point)) {
    SetExpanded(false);
  }
}

bool MediaIsland::DoesIntersectRect(const views::View* target,
                                    const gfx::Rect& rect) const {
  if (!shown_ || (flying_ && !ShowsVideo())) {
    return false;
  }
  if (expanded_) {
    return ExpandedRect().Intersects(rect) ||
           (ShowsVideo() && VideoRect().Intersects(rect));
  }
  if (MainRect().Intersects(rect)) {
    return true;
  }
  for (const Satellite* satellite : satellites_) {
    if (satellite->GetVisible() && satellite->bounds().Intersects(rect)) {
      return true;
    }
  }
  return false;
}

std::string MediaIsland::DebugState() const {
  const bool video = ShowsVideo();
  const std::optional<base::UnguessableToken> video_id = VideoMediaId();
  const MediaEntry* entry = video ? (video_id ? hub_->Find(*video_id) : nullptr)
                                  : (docked_ ? nullptr : Shown());
  // 卫星：标题和位置（窗口视图的坐标，调试步骤 click 用）。
  std::string satellites;
  for (const Satellite* satellite : satellites_) {
    if (!satellite->GetVisible()) {
      continue;
    }
    base::StrAppend(&satellites,
                    {"[", base::UTF16ToUTF8(satellite->GetTooltipText()), "@",
                     views::View::ConvertRectToTarget(this, browser_view_,
                                                      satellite->bounds())
                         .ToString(),
                     "]"});
  }
  return base::StrCat(
      {"shown=",
       shown_ ? "1" : "0",
       " expanded=",
       expanded_ ? "1" : "0",
       " docked=",
       docked_ ? "1" : "0",
       " flying=",
       flying_ ? "1" : "0",
       " video=",
       video ? "1" : (pip_ ? "hidden" : "0"),
       " returning=",
       returning_ ? "1" : "0",
       " video_source=",
       video_source_.ToString(),
       " video_flight=",
       base::NumberToString(video_flight_.value()),
       " corner=",
       base::NumberToString(corner_),
       " avoid=",
       base::NumberToString(avoid_target_),
       " entries=",
       base::NumberToString(hub_->Entries().size()),
       " title=",
       entry ? base::UTF16ToUTF8(entry->title) : std::string(),
       " playing=",
       entry && entry->playing ? "1" : "0",
       " rect=",
       SurfaceRect().ToString(),
       " player=",
       expanded_ ? ExpandedRect().ToString() : std::string(),
       " player_title=",
       expanded_ && player_->selected() && hub_->Find(*player_->selected())
           ? base::UTF16ToUTF8(hub_->Find(*player_->selected())->title)
           : std::string(),
       " dismissed=",
       dismissed_ ? "1" : "0",
       " popout_armed=",
       pop_out_armed_ ? "1" : "0",
       " capsule_in=",
       base::NumberToString(capsule_in_.value()),
       " satellites=",
       satellites});
}

BEGIN_METADATA(MediaIsland)
END_METADATA

}  // namespace starry
