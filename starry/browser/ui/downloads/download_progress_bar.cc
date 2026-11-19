// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/downloads/download_progress_bar.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "base/functional/bind.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/skia_conversions.h"

namespace starry {

namespace {

// 光扫一遍、不定进度条往返一次的周期（秒）。
constexpr double kSheenPeriod = 1.6;
constexpr double kStripePeriod = 2.0;
constexpr base::TimeDelta kFrameInterval = base::Hertz(30);

double Phase(double period) {
  const double seconds =
      (base::TimeTicks::Now() - base::TimeTicks()).InSecondsF();
  return std::fmod(seconds, period) / period;
}

double Lerp(double from, double to, double t) {
  return from + (to - from) * t;
}

}  // namespace

DownloadProgressBar::DownloadProgressBar(base::RepeatingClosure repaint)
    : repaint_(repaint), blend_(1, repaint) {}

DownloadProgressBar::~DownloadProgressBar() = default;

void DownloadProgressBar::Update(const Download& download, bool animate) {
  state_ = download.State();
  const int64_t total = download.TotalBytes();
  indeterminate_ = state_ == DownloadState::kRunning && total <= 0;
  std::vector<Piece> pieces;
  const std::vector<DownloadSegment>& segments = download.Segments();
  segmented_ = download.IsStarry() && total > 0 && segments.size() > 1;
  if (segmented_) {
    std::vector<DownloadSegment> sorted = segments;
    std::ranges::sort(sorted, {}, &DownloadSegment::start);
    for (const DownloadSegment& segment : sorted) {
      Piece piece;
      piece.key = segment.start;
      piece.start = static_cast<double>(segment.start) / total;
      piece.end = static_cast<double>(std::min(segment.end, total)) / total;
      piece.filled = piece.start + static_cast<double>(std::min(
                                       segment.received, segment.length())) /
                                       total;
      pieces.push_back(piece);
    }
  } else {
    Piece piece;
    piece.filled =
        total > 0
            ? std::clamp(static_cast<double>(download.ReceivedBytes()) / total,
                         0.0, 1.0)
            : 0;
    pieces.push_back(piece);
  }
  const bool changed =
      pieces.size() != to_.size() ||
      !std::ranges::equal(pieces, to_, [](const Piece& a, const Piece& b) {
        return a.key == b.key && a.end == b.end && a.filled == b.filled;
      });
  if (changed) {
    if (animate && !motion::ReduceMotion()) {
      from_ = Shown();
      to_ = std::move(pieces);
      blend_.SetValue(0);
      blend_.AnimateTo(1, motion::Progress());
    } else {
      from_ = pieces;
      to_ = std::move(pieces);
      blend_.SetValue(1);
    }
  }
  UpdateTimer();
}

void DownloadProgressBar::SetActive(bool active) {
  active_ = active;
  UpdateTimer();
}

double DownloadProgressBar::ShownFractionForTesting() const {
  double shown = 0;
  for (const Piece& piece : Shown()) {
    shown += piece.filled - piece.start;
  }
  return shown;
}

// 过渡中的样子：同一段（按起点认）从原来的样子动到新的；新切出来的段从零长出来。
std::vector<DownloadProgressBar::Piece> DownloadProgressBar::Shown() const {
  const double t = std::clamp(blend_.value(), 0.0, 1.0);
  std::vector<Piece> shown;
  for (const Piece& target : to_) {
    Piece from = target;
    from.filled = target.start;
    const auto it = std::ranges::find(from_, target.key, &Piece::key);
    if (it != from_.end()) {
      from = *it;
    }
    Piece piece = target;
    piece.end = Lerp(from.end, target.end, t);
    piece.filled = std::min(Lerp(from.filled, target.filled, t), piece.end);
    shown.push_back(piece);
  }
  return shown;
}

void DownloadProgressBar::UpdateTimer() {
  const bool moving =
      active_ && !motion::ReduceMotion() && state_ == DownloadState::kRunning;
  if (moving && !frame_timer_.IsRunning()) {
    frame_timer_.Start(FROM_HERE, kFrameInterval,
                       base::BindRepeating(&DownloadProgressBar::OnFrame,
                                           base::Unretained(this)));
  } else if (!moving && frame_timer_.IsRunning()) {
    frame_timer_.Stop();
  }
}

void DownloadProgressBar::OnFrame() {
  repaint_.Run();
}

void DownloadProgressBar::Paint(gfx::Canvas* canvas,
                                const gfx::RectF& bounds,
                                const Theme& theme,
                                SkColor gap) const {
  if (bounds.IsEmpty()) {
    return;
  }
  const float radius = bounds.height() / 2;
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(theme.OnSurface(theme.is_dark ? 0.1f : 0.08f));
  canvas->DrawRoundRect(bounds, radius, flags);

  SkColor tint = SkColorSetA(theme.on_surface_variant, 0.7f * 255);
  if (state_ == DownloadState::kRunning) {
    tint = theme.accent;
  } else if (state_ == DownloadState::kInterrupted) {
    tint = theme.danger();
  }
  flags.setColor(tint);

  canvas->Save();
  canvas->ClipPath(SkPath::RRect(SkRRect::MakeRectXY(gfx::RectFToSkRect(bounds),
                                                     radius, radius)),
                   /*do_anti_alias=*/true);
  const float width = bounds.width();
  if (indeterminate_) {
    if (!motion::ReduceMotion()) {
      // 来回移动的一小段：宽 30%，按余弦缓动。
      const double eased =
          0.5 - std::cos(Phase(kStripePeriod) * 2 * std::numbers::pi) / 2;
      canvas->DrawRoundRect(
          gfx::RectF(bounds.x() + eased * width * 0.7f, bounds.y(),
                     width * 0.3f, bounds.height()),
          radius, flags);
    }
    canvas->Restore();
    return;
  }

  const std::vector<Piece> pieces = Shown();
  std::vector<gfx::RectF> fills;
  for (const Piece& piece : pieces) {
    float fill = static_cast<float>((piece.filled - piece.start) * width);
    // 没分段时是一整条胶囊，有一点进度时至少和高度一样宽。
    if (!segmented_ && piece.filled > 0) {
      fill = std::max(fill, bounds.height());
    }
    if (fill <= 0) {
      continue;
    }
    fills.emplace_back(bounds.x() + static_cast<float>(piece.start) * width,
                       bounds.y(), fill, bounds.height());
  }
  for (const gfx::RectF& fill : fills) {
    if (segmented_) {
      canvas->DrawRect(fill, flags);
    } else {
      canvas->DrawRoundRect(fill, radius, flags);
    }
  }
  // 下载中扫过的一道光，只在已下载的部分上。
  if (state_ == DownloadState::kRunning && !motion::ReduceMotion()) {
    const float sheen = std::max(60.0f, width * 0.25f);
    const float x = bounds.x() - width * 0.3f +
                    static_cast<float>(Phase(kSheenPeriod)) * width * 1.6f;
    const SkPoint points[2] = {SkPoint::Make(x, 0),
                               SkPoint::Make(x + sheen, 0)};
    const SkColor4f colors[3] = {SkColor4f{1, 1, 1, 0},
                                 SkColor4f{1, 1, 1, 0.35f},
                                 SkColor4f{1, 1, 1, 0}};
    cc::PaintFlags light;
    light.setAntiAlias(true);
    light.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr,
                                                        3, SkTileMode::kClamp));
    for (const gfx::RectF& fill : fills) {
      gfx::RectF lit = fill;
      lit.Intersect(gfx::RectF(x, bounds.y(), sheen, bounds.height()));
      if (!lit.IsEmpty()) {
        canvas->DrawRect(lit, light);
      }
    }
  }
  // 段的起点：一道 1 pt 的细缝，看得出分成了几段。
  if (segmented_) {
    cc::PaintFlags seam;
    seam.setColor(SkColorSetA(gap, 0.9f * 255));
    for (size_t i = 1; i < pieces.size(); ++i) {
      canvas->DrawRect(
          gfx::RectF(bounds.x() + static_cast<float>(pieces[i].start) * width,
                     bounds.y(), 1, bounds.height()),
          seam);
    }
  }
  canvas->Restore();
}

}  // namespace starry
