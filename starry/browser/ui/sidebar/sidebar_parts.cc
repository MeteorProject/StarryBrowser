// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/sidebar/sidebar_parts.h"

#include <algorithm>
#include <utility>

#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/glass/glass_material.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"

namespace starry {

namespace {

// 靠近默认宽度该距离以内时自动吸附。
constexpr int kSnapDistance = 6;

SkColor WithAlpha(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(255 * opacity));
}

}  // namespace

SidebarResizeHandle::Callbacks::Callbacks() = default;
SidebarResizeHandle::Callbacks::Callbacks(Callbacks&&) = default;
SidebarResizeHandle::Callbacks& SidebarResizeHandle::Callbacks::operator=(
    Callbacks&&) = default;
SidebarResizeHandle::Callbacks::~Callbacks() = default;

SidebarResizeHandle::SidebarResizeHandle(Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {
  // 自己的图层：画在侧栏的玻璃和内容上面，细线的淡入淡出在合成器上。
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  layer()->SetOpacity(0);
  SetWindowDraggable(this, false);
  SetTooltipText(L("sidebar.resizeHandle.help"));
}

SidebarResizeHandle::~SidebarResizeHandle() = default;

void SidebarResizeHandle::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void SidebarResizeHandle::SetLineInset(int inset) {
  if (line_inset_ != inset) {
    line_inset_ = inset;
    SchedulePaint();
  }
}

void SidebarResizeHandle::OnPaint(gfx::Canvas* canvas) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(theme_.OnSurface(is_dragging() ? 0.32f : 0.2f));
  const gfx::RectF line((width() - 2) / 2.0f, line_inset_, 2,
                        std::max(0, height() - 2 * line_inset_));
  canvas->DrawRoundRect(line, 1, flags);
}

ui::Cursor SidebarResizeHandle::GetCursor(const ui::MouseEvent& event) {
  return ui::Cursor(ui::mojom::CursorType::kColumnResize);
}

bool SidebarResizeHandle::OnMousePressed(const ui::MouseEvent& event) {
  if (!event.IsOnlyLeftMouseButton()) {
    return false;
  }
  if (event.GetClickCount() == 2) {
    start_width_.reset();
    if (callbacks_.reset) {
      callbacks_.reset.Run();
    }
    return true;
  }
  start_width_ = callbacks_.current_width ? callbacks_.current_width.Run() : 0;
  last_width_ = *start_width_;
  // 侧栏的宽度一变，这个视图跟着挪：用窗口坐标量拖了多远。
  start_x_ = event.root_location().x();
  UpdateLine();
  SchedulePaint();
  return true;
}

bool SidebarResizeHandle::OnMouseDragged(const ui::MouseEvent& event) {
  if (!start_width_) {
    return false;
  }
  int width = *start_width_ + event.root_location().x() - start_x_;
  if (std::abs(width - metrics::kSidebarWidth) <= kSnapDistance) {
    width = metrics::kSidebarWidth;
  }
  width = std::clamp(width, metrics::kSidebarMinWidth,
                     metrics::kSidebarMaxWidth);
  if (width != last_width_) {
    last_width_ = width;
    if (callbacks_.resize) {
      callbacks_.resize.Run(width);
    }
  }
  return true;
}

void SidebarResizeHandle::OnMouseReleased(const ui::MouseEvent& event) {
  if (!start_width_) {
    return;
  }
  start_width_.reset();
  if (callbacks_.commit) {
    callbacks_.commit.Run(last_width_);
  }
  UpdateLine();
  SchedulePaint();
}

void SidebarResizeHandle::OnMouseCaptureLost() {
  if (start_width_) {
    start_width_.reset();
    if (callbacks_.commit) {
      callbacks_.commit.Run(last_width_);
    }
  }
  UpdateLine();
}

void SidebarResizeHandle::OnMouseEntered(const ui::MouseEvent& event) {
  hovering_ = true;
  UpdateLine();
}

void SidebarResizeHandle::OnMouseExited(const ui::MouseEvent& event) {
  hovering_ = false;
  UpdateLine();
}

void SidebarResizeHandle::UpdateLine() {
  const bool active = hovering_ || is_dragging();
  motion_.AnimateOpacity(layer(), active ? 1 : 0, motion::Hover());
}

BEGIN_METADATA(SidebarResizeHandle)
END_METADATA

SidebarEdgeTrigger::SidebarEdgeTrigger(base::RepeatingClosure entered)
    : entered_(std::move(entered)) {
  // Aura 用命中视图的图层判断悬停是否交给下面的网页。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetWindowDraggable(this, false);
}

SidebarEdgeTrigger::~SidebarEdgeTrigger() = default;

void SidebarEdgeTrigger::OnMouseEntered(const ui::MouseEvent& event) {
  if (event.IsAnyButton()) {
    return;
  }
  entered_.Run();
}

void SidebarEdgeTrigger::OnMouseMoved(const ui::MouseEvent& event) {
  // 进来时按着键（比如刚拖完东西），松开后再动一下也要能呼出来。
  entered_.Run();
}

BEGIN_METADATA(SidebarEdgeTrigger)
END_METADATA

SidebarShadow::SidebarShadow() {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
}

SidebarShadow::~SidebarShadow() = default;

void SidebarShadow::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void SidebarShadow::SetPanel(const gfx::Rect& panel, float radius) {
  radius_ = radius;
  gfx::Rect bounds = panel;
  bounds.Outset(kOutset);
  SetBoundsRect(bounds);
  SchedulePaint();
}

void SidebarShadow::OnPaint(gfx::Canvas* canvas) {
  gfx::RectF panel(GetLocalBounds());
  panel.Inset(kOutset);
  const SkRRect rrect =
      SkRRect::MakeRectXY(gfx::RectFToSkRect(panel), radius_, radius_);
  canvas->Save();
  // 只画面板外面的部分：面板是半透明的玻璃，阴影不能透上来。
  canvas->sk_canvas()->clipRRect(rrect, SkClipOp::kDifference, true);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(SK_ColorBLACK);
  flags.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(
      gfx::Vector2d(0, 10), 56,
      WithAlpha(SK_ColorBLACK, theme_.is_dark ? 0.45f : 0.22f))}));
  canvas->sk_canvas()->drawRRect(rrect, flags);
  canvas->Restore();
}

BEGIN_METADATA(SidebarShadow)
END_METADATA

SidebarPanelFill::SidebarPanelFill() {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
}

SidebarPanelFill::~SidebarPanelFill() = default;

void SidebarPanelFill::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void SidebarPanelFill::SetOverPage(bool over_page) {
  if (over_page_ != over_page) {
    over_page_ = over_page;
    SchedulePaint();
  }
}

void SidebarPanelFill::SetRadius(float radius) {
  radius_ = radius;
  SchedulePaint();
}

void SidebarPanelFill::OnPaint(gfx::Canvas* canvas) {
  const float opacity = SidebarGlassOverlayOpacity(theme_.is_dark, over_page_);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(WithAlpha(theme_.surface_panel, opacity));
  canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius_, flags);
}

BEGIN_METADATA(SidebarPanelFill)
END_METADATA

}  // namespace starry
