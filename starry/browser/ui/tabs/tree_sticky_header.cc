// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tree_sticky_header.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 同列表里的行：左 10、图标 16、图标和名字之间 10，右 6。底下的线左右各缩 10。
constexpr int kLeading = 10;
constexpr int kIconSize = 16;
constexpr int kSpacing = 10;
constexpr int kTrailing = 6;
constexpr int kLineInset = 10;
constexpr int kFontSize = 12;

}  // namespace

TreeStickyHeader::TreeStickyHeader(
    base::RepeatingCallback<void(tabs::TabHandle)> clicked,
    base::RepeatingClosure changed)
    : clicked_(std::move(clicked)),
      changed_(std::move(changed)),
      fade_(1,
            base::BindRepeating(&TreeStickyHeader::OnStep,
                                base::Unretained(this))) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  // 读屏走列表里的那几行，这里只是看的。
  GetViewAccessibility().SetIsIgnored(true);
}

TreeStickyHeader::~TreeStickyHeader() = default;

void TreeStickyHeader::SetRows(std::vector<Row> rows, const Theme& theme) {
  const bool same =
      std::ranges::equal(rows, rows_, [](const Row& a, const Row& b) {
        return a.handle == b.handle && a.indent == b.indent;
      });
  theme_ = theme;
  if (same) {
    rows_ = std::move(rows);
    SchedulePaint();
    return;
  }
  // 换了一组祖先：旧的淡出、新的淡入，下面让出来的高度跟着过渡。
  // 上一次交叉淡化还没过半：看得见的主要还是旧的那一组，从它淡出。
  if (!fade_.is_animating() || fade_.value() >= 0.5) {
    old_rows_ = std::move(rows_);
  }
  rows_ = std::move(rows);
  hovered_.reset();
  pressed_.reset();
  fade_.SetValue(0);
  fade_.AnimateTo(
      GetWidget() && IsDrawn() ? GetWidget()->GetCompositor() : nullptr, 1,
      motion::Hover());
}

float TreeStickyHeader::VisibleHeight() const {
  const float now = static_cast<float>(rows_.size() * kRowHeight);
  if (!fade_.is_animating()) {
    return now;
  }
  const float before = static_cast<float>(old_rows_.size() * kRowHeight);
  const float t = static_cast<float>(fade_.value());
  return before + (now - before) * t;
}

int TreeStickyHeader::PreferredHeight() const {
  const size_t count = fade_.is_animating()
                           ? std::max(rows_.size(), old_rows_.size())
                           : rows_.size();
  return static_cast<int>(count) * kRowHeight;
}

void TreeStickyHeader::OnStep() {
  if (!fade_.is_animating()) {
    old_rows_.clear();
  }
  SchedulePaint();
  changed_.Run();
}

void TreeStickyHeader::OnPaint(gfx::Canvas* canvas) {
  if (!fade_.is_animating()) {
    PaintRows(canvas, rows_);
    return;
  }
  const float t = static_cast<float>(fade_.value());
  canvas->SaveLayerAlpha(static_cast<uint8_t>(255 * (1 - t)));
  PaintRows(canvas, old_rows_);
  canvas->Restore();
  canvas->SaveLayerAlpha(static_cast<uint8_t>(255 * t));
  PaintRows(canvas, rows_);
  canvas->Restore();
}

void TreeStickyHeader::PaintRows(gfx::Canvas* canvas,
                                 const std::vector<Row>& rows) {
  if (rows.empty()) {
    return;
  }
  const ui::ColorProvider* provider = GetColorProvider();
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    const int top = static_cast<int>(i) * kRowHeight;
    if (hovered_ == i && &rows == &rows_) {
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(theme_.OnSurface(0.06f));
      canvas->DrawRoundRect(
          gfx::RectF(row.indent, top, std::max(0, width() - row.indent),
                     kRowHeight),
          radius::kRow, fill);
    }
    const int icon_x = row.indent + kLeading;
    if (!row.icon.IsEmpty() && provider) {
      const gfx::ImageSkia image = row.icon.Rasterize(provider);
      if (!image.isNull()) {
        canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), icon_x,
                             top + (kRowHeight - kIconSize) / 2, kIconSize,
                             kIconSize, true);
      }
    }
    const int title_x = icon_x + kIconSize + kSpacing;
    canvas->DrawStringRectWithFlags(
        row.title, UiFont(kFontSize), theme_.on_surface_variant,
        gfx::Rect(title_x, top, std::max(0, width() - kTrailing - title_x),
                  kRowHeight),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
  // 和下面滚过去的行隔开。
  const int bottom = static_cast<int>(rows.size()) * kRowHeight;
  canvas->FillRect(gfx::Rect(kLineInset, bottom - 1,
                             std::max(0, width() - 2 * kLineInset), 1),
                   theme_.OnSurface(0.08f));
}

std::optional<size_t> TreeStickyHeader::RowAt(const gfx::Point& point) const {
  if (point.y() < 0 || point.x() < 0 || point.x() >= width()) {
    return std::nullopt;
  }
  const size_t index = static_cast<size_t>(point.y() / kRowHeight);
  return index < rows_.size() ? std::optional<size_t>(index) : std::nullopt;
}

bool TreeStickyHeader::OnMousePressed(const ui::MouseEvent& event) {
  pressed_ =
      event.IsOnlyLeftMouseButton() ? RowAt(event.location()) : std::nullopt;
  return pressed_.has_value();
}

void TreeStickyHeader::OnMouseReleased(const ui::MouseEvent& event) {
  const std::optional<size_t> pressed = std::exchange(pressed_, std::nullopt);
  if (pressed && RowAt(event.location()) == pressed) {
    clicked_.Run(rows_[*pressed].handle);
  }
}

void TreeStickyHeader::OnMouseMoved(const ui::MouseEvent& event) {
  const std::optional<size_t> row = RowAt(event.location());
  if (row != hovered_) {
    hovered_ = row;
    SchedulePaint();
  }
}

void TreeStickyHeader::OnMouseExited(const ui::MouseEvent& event) {
  if (hovered_) {
    hovered_.reset();
    SchedulePaint();
  }
}

std::u16string TreeStickyHeader::GetRenderedTooltipText(
    const gfx::Point& point) const {
  const std::optional<size_t> row = RowAt(point);
  return row ? rows_[*row].title : std::u16string();
}

BEGIN_METADATA(TreeStickyHeader)
END_METADATA

}  // namespace starry
