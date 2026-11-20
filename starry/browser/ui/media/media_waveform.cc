// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_waveform.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "base/functional/bind.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_loop.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 频谱、电平条落下来的快慢（每 1/30 秒留下这么多）。
constexpr float kFall = 0.8f;
// 跟着屏幕刷新画的频谱涨上去的快慢（每 1/30 秒还差这么多）：电平每秒只来 30
// 次左右， 一步涨到位的话涨的那一下还是每秒 30 帧。
constexpr float kRise = 0.2f;
// 一帧里最大的变化不到这么多（整条高度的比例）就不重画。
constexpr float kStill = 0.0005f;
// 跟着屏幕刷新画时两次之间最少隔这么久：60 Hz 的屏每帧都画，120 Hz
// 的屏隔一帧画一次。
constexpr base::TimeDelta kMinStep = base::Milliseconds(12);
// 中间卡了一下之后按这么久算，不一下跳到位。
constexpr base::TimeDelta kMaxStep = base::Milliseconds(100);
// 实时波形按最近的峰值放大：峰值往下这么多（60 dB 的刻度上是 18 dB）画成一个点。
// 压得很满的流行乐鼓点和鼓点之间只差几 dB，不放大是一整块。
constexpr float kLiveRange = 0.3f;
// 比这还小（约 -52 dBFS）当作没声音，画成点。
constexpr float kLiveSilence = 0.13f;
// 峰值涨上去、落下来的快慢（每 1/30 秒还差、留下这么多）：响的那一段移出去时整条慢慢
// 变大，不一下跳。
constexpr float kLivePeakRise = 0.4f;
constexpr float kLivePeakFall = 0.97f;
// 左边淡出去的那一段占整条的多少。
constexpr float kLiveFade = 0.22f;

SkColor WithAlpha(SkColor color, float alpha) {
  return SkColorSetA(
      color, static_cast<U8CPU>(std::lround(SkColorGetA(color) *
                                            std::clamp(alpha, 0.0f, 1.0f))));
}

void Bar(gfx::Canvas* canvas,
         float x,
         float center_y,
         float width,
         float height,
         SkColor color) {
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(color);
  canvas->DrawRoundRect(gfx::RectF(x, center_y - height / 2, width, height),
                        width / 2, flags);
}

}  // namespace

MediaWaveform::MediaWaveform(MediaHub* hub, Style style)
    : hub_(hub), style_(style) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
}

MediaWaveform::~MediaWaveform() {
  Stop();
}

void MediaWaveform::Set(const MediaEntry& entry,
                        SkColor color,
                        SkColor color2,
                        SkColor rest) {
  if (!id_ || *id_ != entry.id) {
    watch_.reset();
    shown_.fill(0);
    live_arrival_ = base::TimeTicks();
    live_peak_ = 0;
  }
  id_ = entry.id;
  playing_ = entry.playing;
  progress_ = entry.Progress();
  duration_seconds_ =
      entry.position ? entry.position->duration().InSecondsF() : 0;
  color_ = color;
  color2_ = color2;
  rest_ = rest;
  Update();
  SchedulePaint();
}

void MediaWaveform::SetSource(const base::UnguessableToken& id,
                              bool playing,
                              SkColor color) {
  if (!id_ || *id_ != id) {
    watch_.reset();
    shown_.fill(0);
  }
  id_ = id;
  playing_ = playing;
  color_ = color;
  color2_ = color;
  rest_ = color;
  Update();
  SchedulePaint();
}

void MediaWaveform::SetBar(float width, float gap) {
  bar_ = width;
  gap_ = gap;
  SchedulePaint();
}

void MediaWaveform::VisibilityChanged(views::View* starting_from,
                                      bool is_visible) {
  Update();
}

void MediaWaveform::AddedToWidget() {
  Update();
}

void MediaWaveform::RemovedFromWidget() {
  watch_.reset();
  Stop();
}

void MediaWaveform::Update() {
  const bool wants = id_ && playing_ && GetWidget() && IsDrawn();
  if (wants && !watch_) {
    watch_ = hub_->WatchLevels(*id_);
  } else if (!wants) {
    watch_.reset();
  }
  if (wants && !Running()) {
    Start();
  } else if (!wants && Running()) {
    Stop();
    shown_.fill(0);
    SchedulePaint();
  }
}

void MediaWaveform::Start() {
  ui::Compositor* compositor =
      Smooth() ? GetWidget()->GetCompositor() : nullptr;
  if (compositor) {
    compositor_ = compositor;
    last_step_ = base::TimeTicks();
    compositor_->AddAnimationObserver(this);
    return;
  }
  ticker_.Start(
      FROM_HERE, base::Seconds(1.0 / LayerLoop::kLoopFps),
      base::BindRepeating(&MediaWaveform::Tick, base::Unretained(this)));
}

void MediaWaveform::Stop() {
  if (compositor_) {
    compositor_->RemoveAnimationObserver(this);
    compositor_ = nullptr;
  }
  ticker_.Stop();
}

void MediaWaveform::OnAnimationStep(base::TimeTicks timestamp) {
  if (last_step_.is_null()) {
    last_step_ = timestamp;
    return;
  }
  const base::TimeDelta elapsed = timestamp - last_step_;
  if (elapsed < kMinStep) {
    return;
  }
  last_step_ = timestamp;
  Advance(std::min(elapsed, kMaxStep));
}

void MediaWaveform::OnCompositingShuttingDown(ui::Compositor* compositor) {
  // 窗口的合成器没了（窗口关掉、藏起来）：停下，下次 Update 再接上。
  Stop();
}

double MediaWaveform::TakePaintRate() {
  const base::TimeTicks now = base::TimeTicks::Now();
  const double seconds =
      paints_since_.is_null() ? 0 : (now - paints_since_).InSecondsF();
  const double rate = seconds > 0 ? paints_ / seconds : 0;
  paints_ = 0;
  paints_since_ = now;
  return rate;
}

const MediaLevels* MediaWaveform::Levels() const {
  return id_ ? hub_->Levels(*id_) : nullptr;
}

void MediaWaveform::Tick() {
  Advance(base::Seconds(1.0 / LayerLoop::kLoopFps));
}

void MediaWaveform::Advance(base::TimeDelta elapsed) {
  if (!watch_ && id_) {
    // 会话（音频焦点）比标签的「有声音」晚到一点。
    watch_ = hub_->WatchLevels(*id_);
  }
  const MediaLevels* levels = Levels();
  bool changed = (levels != nullptr) != had_levels_;
  had_levels_ = levels != nullptr;
  if (levels) {
    // 留下的比例按过了多久算，画得勤、画得疏落下来都一样快。
    const float frames =
        static_cast<float>(elapsed.InSecondsF() * LayerLoop::kLoopFps);
    const float fall = std::pow(kFall, frames);
    const float rise = Smooth() ? std::pow(kRise, frames) : 0;
    const MediaLevels::Frame& frame = levels->frame();
    for (int i = 0; i < MediaLevels::kBands; ++i) {
      const float target = frame.bands[i];
      const float value =
          target + (shown_[i] - target) * (target > shown_[i] ? rise : fall);
      changed = changed || std::abs(value - shown_[i]) > kStill;
      shown_[i] = value;
    }
    if (style_ == Style::kLive) {
      // 来了新的一帧：记下什么时候到的，量一量两帧之间隔多久。
      const base::TimeTicks now = base::TimeTicks::Now();
      if (levels->frame().time != live_frame_time_) {
        if (!live_arrival_.is_null()) {
          live_interval_ =
              std::clamp(live_interval_ * 0.8 + (now - live_arrival_) * 0.2,
                         base::Milliseconds(20), base::Milliseconds(80));
        }
        live_frame_time_ = levels->frame().time;
        live_arrival_ = now;
      }
      const base::circular_deque<float>& history = levels->history();
      const size_t columns = std::min<size_t>(LiveColumns(), history.size());
      float peak = 0;
      for (size_t i = history.size() - columns; i < history.size(); ++i) {
        peak = std::max(peak, history[i]);
      }
      const float keep =
          std::pow(peak > live_peak_ ? kLivePeakRise : kLivePeakFall, frames);
      live_peak_ = peak + (live_peak_ - peak) * keep;
      // 在放的时候一直往左走。
      changed = true;
    }
  }
  // 进度按时间往后走（MediaHub 只在跳转、暂停时通知）。
  if (id_ && style_ == Style::kRecorded) {
    if (const MediaEntry* entry = hub_->Find(*id_)) {
      progress_ = entry->Progress();
    }
  }
  if (changed || !Smooth()) {
    SchedulePaint();
  }
}

void MediaWaveform::OnPaint(gfx::Canvas* canvas) {
  if (!id_ || width() <= 0 || height() <= 0) {
    return;
  }
  if (paints_since_.is_null()) {
    paints_since_ = base::TimeTicks::Now();
  }
  ++paints_;
  const MediaLevels* levels = playing_ ? Levels() : nullptr;
  switch (style_) {
    case Style::kSpectrum:
      PaintSpectrum(canvas, levels);
      break;
    case Style::kLive:
      PaintLive(canvas, levels);
      break;
    case Style::kRoll:
      PaintRoll(canvas, levels);
      break;
    case Style::kRecorded:
      if (duration_seconds_ > 0 && progress_) {
        PaintRecorded(canvas);
      } else {
        PaintRoll(canvas, levels);
      }
      break;
    case Style::kMeter:
      PaintMeter(canvas, levels);
      break;
  }
}

void MediaWaveform::PaintSpectrum(gfx::Canvas* canvas,
                                  const MediaLevels* levels) {
  const int n = std::max(1, static_cast<int>((width() + gap_) / (bar_ + gap_)));
  const float offset = (width() - (n * bar_ + (n - 1) * gap_)) / 2;
  const float half = std::max(1.0f, (n - 1) / 2.0f);
  const float h = height();
  for (int j = 0; j < n; ++j) {
    const float d = std::abs(j - (n - 1) / 2.0f) / half;
    const float index = d * (MediaLevels::kBands - 1);
    const int i0 = static_cast<int>(index);
    const int i1 = std::min(MediaLevels::kBands - 1, i0 + 1);
    const float fraction = index - i0;
    float value = shown_[i0] * (1 - fraction) + shown_[i1] * fraction;
    value *= 1 - 0.25f * d;
    if (!levels) {
      value = 0;
    }
    const float bar_height = std::max(bar_, value * h);
    const SkColor color = Theme::LerpColor(color_, color2_, d);
    Bar(canvas, offset + j * (bar_ + gap_), h / 2, bar_, bar_height,
        levels ? color : WithAlpha(color, 0.5f));
  }
}

int MediaWaveform::LiveColumns() const {
  return static_cast<int>(width() / (bar_ + gap_)) + 2;
}

void MediaWaveform::PaintLive(gfx::Canvas* canvas, const MediaLevels* levels) {
  const float pitch = bar_ + gap_;
  const float w = width();
  const float h = height();
  if (!levels) {
    // 暂停着、还没有声音：一排点。
    const int n = std::max(1, static_cast<int>((w + gap_) / pitch));
    const float offset = (w - (n * bar_ + (n - 1) * gap_)) / 2;
    for (int j = 0; j < n; ++j) {
      Bar(canvas, offset + j * pitch, h / 2, bar_, bar_,
          WithAlpha(color_, 0.5f));
    }
    return;
  }
  // 两帧之间往左挪了多少格：最新的一条从右边外面滑进来，下一帧到时刚好挪满一格，
  // 所以来了新的一帧时每一条都还在原处。晚到时停一下，不往回跳。
  float phase = 1;
  if (!live_arrival_.is_null()) {
    phase =
        std::clamp(static_cast<float>((base::TimeTicks::Now() - live_arrival_) /
                                      live_interval_),
                   0.0f, 1.0f);
  }
  const base::circular_deque<float>& history = levels->history();
  const float floor = live_peak_ - kLiveRange;
  const int n = LiveColumns();
  for (int age = 0; age < n; ++age) {
    const float x = w - bar_ - pitch * (age - 1 + phase);
    if (x <= -bar_) {
      break;
    }
    float value = 0;
    if (static_cast<size_t>(age) < history.size()) {
      value = history[history.size() - 1 - age];
    }
    float t = value < kLiveSilence
                  ? 0
                  : std::clamp((value - floor) / kLiveRange, 0.0f, 1.0f);
    t = std::pow(t, 1.4f);
    // 响的地方偏浅色、更亮；左边那一段慢慢淡出去。
    const float fade = std::clamp(x / (w * kLiveFade), 0.0f, 1.0f);
    const SkColor color = Theme::LerpColor(color_, color2_, 0.5f * t * t);
    Bar(canvas, x, h / 2, bar_, std::max(bar_, t * h),
        WithAlpha(color, 0.2f + 0.8f * fade));
  }
}

void MediaWaveform::PaintRoll(gfx::Canvas* canvas, const MediaLevels* levels) {
  const int n = std::max(1, static_cast<int>((width() + gap_) / (bar_ + gap_)));
  const float offset = width() - (n * bar_ + (n - 1) * gap_);
  const float h = height();
  const size_t count = levels ? levels->history().size() : 0;
  for (int j = 0; j < n; ++j) {
    // 最右边是现在，往左每一格早一帧。
    const int age = n - 1 - j;
    float value = 0;
    if (levels && static_cast<size_t>(age) < count) {
      value = levels->history()[count - 1 - age];
    }
    value = std::pow(value, 1.3f);
    const float alpha = 0.3f + 0.7f * j / std::max(1, n - 1);
    Bar(canvas, offset + j * (bar_ + gap_), h / 2, bar_,
        std::max(bar_, value * h), WithAlpha(levels ? color_ : rest_, alpha));
  }
}

void MediaWaveform::PaintRecorded(gfx::Canvas* canvas) {
  const int n = std::max(1, static_cast<int>((width() + gap_) / (bar_ + gap_)));
  const float offset = (width() - (n * bar_ + (n - 1) * gap_)) / 2;
  const float h = height();
  const std::vector<float>* recorded = hub_->Recorded(*id_);
  const double progress = progress_.value_or(0);
  const double per_column = duration_seconds_ / n;
  for (int j = 0; j < n; ++j) {
    const float x = offset + j * (bar_ + gap_);
    const bool played = (j + 0.5) / n <= progress;
    if (!played) {
      Bar(canvas, x, h / 2, bar_, bar_, rest_);
      continue;
    }
    float value = -1;
    if (recorded) {
      const size_t from =
          static_cast<size_t>(j * per_column * MediaHub::kRecordedPerSecond);
      const size_t to =
          std::max(from + 1, static_cast<size_t>((j + 1) * per_column *
                                                 MediaHub::kRecordedPerSecond));
      for (size_t b = from; b < to && b < recorded->size(); ++b) {
        value = std::max(value, (*recorded)[b]);
      }
    }
    if (value < 0) {
      // 放过了但没记到（跳过去的、刚打开时已经放过的）：同色的点。
      Bar(canvas, x, h / 2, bar_, bar_, WithAlpha(color_, 0.55f));
    } else {
      Bar(canvas, x, h / 2, bar_, std::max(bar_, std::pow(value, 1.2f) * h),
          color_);
    }
  }
}

void MediaWaveform::PaintMeter(gfx::Canvas* canvas, const MediaLevels* levels) {
  // 低、中、高三段各取最响的一格。
  constexpr std::array<std::array<int, 2>, 3> kGroups = {
      {{2, 8}, {10, 18}, {20, 28}}};
  const int n = 3;
  const float total = n * bar_ + (n - 1) * gap_;
  const float offset = (width() - total) / 2;
  const float h = height();
  for (int j = 0; j < n; ++j) {
    float value = 0;
    if (levels) {
      for (int i = kGroups[j][0]; i <= kGroups[j][1]; ++i) {
        value = std::max(value, shown_[i]);
      }
      value = std::clamp(value * 1.2f, 0.18f, 1.0f);
    }
    Bar(canvas, offset + j * (bar_ + gap_), h / 2, bar_,
        std::max(bar_, value * h), color_);
  }
}

BEGIN_METADATA(MediaWaveform)
END_METADATA

}  // namespace starry
