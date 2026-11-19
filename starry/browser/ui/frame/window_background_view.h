// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FRAME_WINDOW_BACKGROUND_VIEW_H_
#define STARRY_BROWSER_UI_FRAME_WINDOW_BACKGROUND_VIEW_H_

#include <vector>

#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"

namespace gfx {
class Canvas;
}

namespace starry {

// 窗口的底：铺满整个窗口的底色，和内容卡片的底（卡片色、圆角、阴影；网页画在它上面）。
// 在 BrowserView 所有子视图的最下面。空白处可以拖动窗口。
//
// 静态的玻璃（GlassView::SetUnderlay）后面的底也由它画：PaintUnderlay。
class WindowBackgroundView : public views::View {
  METADATA_HEADER(WindowBackgroundView, views::View)

 public:
  WindowBackgroundView();
  WindowBackgroundView(const WindowBackgroundView&) = delete;
  WindowBackgroundView& operator=(const WindowBackgroundView&) = delete;
  ~WindowBackgroundView() override;

  void SetTheme(const Theme& theme);
  // 内容卡片的位置（这个视图的坐标）和圆角；没有时表示没有卡片（全屏）。分屏时每一格
  // 一张。
  void SetCards(std::vector<gfx::Rect> cards, int radius);
  // 卡片的底和阴影有多淡（0…1）：小窗口出现时卡片淡入。
  void SetCardOpacity(float opacity);

  // 窗口边框自己画的按钮占的地方（这个视图的坐标）。Windows 和 Linux 上最小化、关闭
  // 这些按钮是边框视图画的，边框视图在浏览器视图下面：底不能盖住它们，这些地方留空。
  // macOS 的红绿灯是系统的视图，在最上面，不用留。
  void SetFrameControlsRects(std::vector<gfx::Rect> rects);

  // 把 `rect`（这个视图的坐标）里的底画到 canvas 的原点处：给盖在它上面的静态玻璃用。
  void PaintUnderlay(gfx::Canvas* canvas, const gfx::Rect& rect) const;

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  void PaintContents(gfx::Canvas* canvas) const;

  Theme theme_ = Theme::DarkBase();
  std::vector<gfx::Rect> cards_;
  int card_radius_ = 0;
  float card_opacity_ = 1;
  std::vector<gfx::Rect> frame_controls_rects_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FRAME_WINDOW_BACKGROUND_VIEW_H_
