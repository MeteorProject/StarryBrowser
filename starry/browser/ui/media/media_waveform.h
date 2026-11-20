// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_WAVEFORM_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_WAVEFORM_H_

#include <array>
#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/media/media_levels.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/compositor/compositor_animation_observer.h"
#include "ui/views/view.h"

namespace ui {
class Compositor;
}

namespace starry {

// 媒体实时波形视图。可见且正在播放时订阅对应标签的音频回环流（MediaHub::WatchLevels）
// 并重绘自身区域；不可见或暂停时停止采样与重绘（暂停时静止为一排圆点）。
//
// 刷新率按样式区分：
// - 频谱、实时波形只在展开的播放器、画中画的画面上显示：跟随屏幕刷新以 60 fps
//   补间（120 Hz 屏幕上隔帧绘制），在约 30 Hz 的电平采样间按时间平滑插值；频谱静音无变化
//   时跳过重绘，实时波形在放的时候一直往左走。
// - 滚动响度、进度与电平条作为常驻小型指示器：按 30 fps 刷新（与电平采样频率匹配，
//   避免常驻组件持续高频提交帧带来不必要的合成开销）。
class MediaWaveform : public views::View,
                      public ui::CompositorAnimationObserver {
  METADATA_HEADER(MediaWaveform, views::View)

 public:
  enum class Style {
    // 对称频谱：低频在中间，往两边是高频（画中画的画面上）。
    kSpectrum,
    // 实时波形：最近两三秒的响度，新的从右边进来、平滑地往左走，上下对称（展开的播放器）。
    // 按最近的峰值放大，鼓点、乐句、换气和安静的地方看得出来。
    kLive,
    // 滚动响度：最近几秒的电平，新的在右边（岛上的胶囊）。
    kRoll,
    // 边播边画的进度：放过的部分画出当时的响度，没放到的是一排点（侧栏的坞）。不知道总长
    // （直播）时换成滚动响度。
    kRecorded,
    // 电平条：三、四条跟着声音跳（标签行的胶囊）。
    kMeter,
  };

  MediaWaveform(MediaHub* hub, Style style);
  MediaWaveform(const MediaWaveform&) = delete;
  MediaWaveform& operator=(const MediaWaveform&) = delete;
  ~MediaWaveform() override;

  // 画哪个媒体、用什么颜色。color2 是频谱两边（高频）的颜色；rest
  // 是没放到的点。
  void Set(const MediaEntry& entry,
           SkColor color,
           SkColor color2,
           SkColor rest);
  // 只知道是哪个标签的声音（标签行的电平条：不用整个
  // MediaEntry）。会话还没出现时 一直等它出现。
  void SetSource(const base::UnguessableToken& id, bool playing, SkColor color);
  void SetBar(float width, float gap);

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;

  // ui::CompositorAnimationObserver:
  void OnAnimationStep(base::TimeTicks timestamp) override;
  void OnCompositingShuttingDown(ui::Compositor* compositor) override;

  // 上次问过之后每秒画了多少次（调试步骤 mediainfo 用）。
  double TakePaintRate();

 private:
  // 跟着屏幕刷新画（频谱、实时波形），还是按 30 帧的定时器画。
  bool Smooth() const {
    return style_ == Style::kSpectrum || style_ == Style::kLive;
  }
  bool Running() const { return compositor_ || ticker_.IsRunning(); }
  void Update();
  void Start();
  void Stop();
  void Tick();
  // 过了 elapsed 这么久：显示的值往新的电平靠，有变化就重画。
  void Advance(base::TimeDelta elapsed);
  const MediaLevels* Levels() const;
  void PaintSpectrum(gfx::Canvas* canvas, const MediaLevels* levels);
  void PaintLive(gfx::Canvas* canvas, const MediaLevels* levels);
  // 实时波形一行摆得下几条（加上从右边进来、左边出去的各一条）。
  int LiveColumns() const;
  void PaintRoll(gfx::Canvas* canvas, const MediaLevels* levels);
  void PaintRecorded(gfx::Canvas* canvas);
  void PaintMeter(gfx::Canvas* canvas, const MediaLevels* levels);

  const raw_ptr<MediaHub> hub_;
  const Style style_;
  std::optional<base::UnguessableToken> id_;
  bool playing_ = false;
  std::optional<double> progress_;
  double duration_seconds_ = 0;
  SkColor color_ = SK_ColorWHITE;
  SkColor color2_ = SK_ColorWHITE;
  SkColor rest_ = SK_ColorGRAY;
  float bar_ = 2;
  float gap_ = 1.6f;
  // 频谱、电平条显示出来的值：涨得快、落得慢。
  std::array<float, MediaLevels::kBands> shown_{};
  // 实时波形：最新的一帧电平是哪一帧、什么时候到的，两帧之间大约隔多久：两帧之间按时间
  // 往左挪，下一帧到时刚好挪满一格。
  base::TimeTicks live_frame_time_;
  base::TimeTicks live_arrival_;
  base::TimeDelta live_interval_ = base::Milliseconds(33);
  // 实时波形按它放大：看得见的那一段里最响的（涨得快、落得慢，响的移出去时不会一下跳）。
  float live_peak_ = 0;
  std::unique_ptr<MediaLevelWatch> watch_;
  bool had_levels_ = false;
  base::RepeatingTimer ticker_;
  // 跟着屏幕刷新画时挂在这个合成器上。
  raw_ptr<ui::Compositor> compositor_ = nullptr;
  base::TimeTicks last_step_;
  int paints_ = 0;
  base::TimeTicks paints_since_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_WAVEFORM_H_
