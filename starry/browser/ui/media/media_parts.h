// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_PARTS_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_PARTS_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view.h"

namespace gfx {
class Canvas;
}

namespace starry {

struct MediaEntry;

// 媒体卡片配色：有封面时按封面主色通过 Theme::Make 生成着色主题，否则沿用窗口主题。
Theme MediaThemeFor(const MediaEntry* entry, const Theme& window);

// 媒体控制按钮使用的矢量图标，在正方形 box 内绘制。
enum class MediaGlyph {
  kPlay,
  kPause,
  kPrevious,
  kNext,
  kBack10,
  kForward10,
  kPictureInPicture,
  kExitPictureInPicture,
  kShowTab,
  kDockSidebar,
  kDockTabStrip,
  kFloat,
  kSpeaker,
  kMuted,
  kCollapse,
  // xmark。
  kClose,
  // 方框里一支箭头往右上角伸出去（弹到桌面）。
  kPopOut,
};
void PaintMediaGlyph(gfx::Canvas* canvas,
                     MediaGlyph glyph,
                     const gfx::RectF& box,
                     SkColor color);

// 封面：铺满 rect、居中裁掉多出来的，裁成圆角（radius 是 rect
// 一半时是圆）。没有封面时 是主题色的渐变，中间是网站图标或音符。
void PaintArtwork(gfx::Canvas* canvas,
                  const gfx::RectF& rect,
                  float radius,
                  const MediaEntry& entry,
                  const Theme& theme);

// 一圈进度（从正上方顺时针）：底下一圈 track，上面 progress 那么长。
void PaintProgressRing(gfx::Canvas* canvas,
                       const gfx::PointF& center,
                       float radius,
                       float stroke,
                       double progress,
                       SkColor track,
                       SkColor fill);

// 1:24、1:02:03。
std::u16string FormatMediaTime(base::TimeDelta time);

// 只写一行，放不下截断加「…」。
void DrawMediaText(gfx::Canvas* canvas,
                   const std::u16string& text,
                   int size,
                   bool semibold,
                   SkColor color,
                   const gfx::Rect& rect,
                   bool align_right = false);

// 圆形的媒体按钮：ghost 平时没有底，悬停垫一层浅底；tonal 一直有浅底；filled
// 是实心的 主题色（播放、暂停的大按钮）。按下缩到 0.96（同 IconButton）。
class MediaButton : public views::Button {
  METADATA_HEADER(MediaButton, views::Button)

 public:
  // overlay：盖在视频画面上，半透明的深色底、白色图形和一圈细线。
  enum class Style { kGhost, kTonal, kFilled, kOverlay };

  MediaButton(PressedCallback callback,
              MediaGlyph glyph,
              const std::u16string& tooltip,
              int size = 28,
              int glyph_size = 14,
              Style style = Style::kGhost);
  MediaButton(const MediaButton&) = delete;
  MediaButton& operator=(const MediaButton&) = delete;
  ~MediaButton() override;

  void SetGlyph(MediaGlyph glyph, const std::u16string& tooltip);
  MediaGlyph glyph() const { return glyph_; }
  void SetTheme(const Theme& theme);
  // 盖在视频画面上时用白色。
  void SetGlyphColor(std::optional<SkColor> color);

  // views::Button:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void PaintButtonContents(gfx::Canvas* canvas) override;
  void StateChanged(ButtonState old_state) override;

 private:
  MediaGlyph glyph_;
  int size_;
  int glyph_size_;
  Style style_;
  Theme theme_ = Theme::DarkBase();
  std::optional<SkColor> glyph_color_;
  bool pressed_scale_ = false;
  LayerMotion press_motion_;
};

// 进度条：4
// 高的轨道（点、拖的范围是整个视图的高度），放过的部分用强调色；悬停、拖动时
// 出现圆钮。拖动时跟手，松开才跳过去（一路跳会让网页反复缓冲）。
class MediaSeekBar : public views::View {
  METADATA_HEADER(MediaSeekBar, views::View)

 public:
  static constexpr int kHeight = 14;

  explicit MediaSeekBar(base::RepeatingCallback<void(double fraction)> seek);
  MediaSeekBar(const MediaSeekBar&) = delete;
  MediaSeekBar& operator=(const MediaSeekBar&) = delete;
  ~MediaSeekBar() override;

  // progress 为空时（直播）不画放过的部分，也不能拖。
  void Set(std::optional<double> progress, SkColor fill, const Theme& theme);
  bool dragging() const { return drag_.has_value(); }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;

 private:
  double FractionAt(int x) const;

  base::RepeatingCallback<void(double)> seek_;
  std::optional<double> progress_;
  std::optional<double> drag_;
  SkColor fill_ = SK_ColorWHITE;
  Theme theme_ = Theme::DarkBase();
  bool hovered_ = false;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_PARTS_H_
