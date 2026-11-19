// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_MEDIA_MEDIA_LEVELS_H_
#define STARRY_BROWSER_MEDIA_MEDIA_LEVELS_H_

#include <array>
#include <memory>

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"

namespace content {
class WebContents;
}

namespace media {
class AudioInputDevice;
}

namespace starry {

// 一个标签的实时声音（媒体指示器的波形用）：开一个回环流（同标签页录制，但不静音原声），
// 拿到这个标签混好的音频，在音频线程上算电平、频谱和一小段波形，每秒最多 30
// 次交给 UI 线程。只在有界面要看时开（MediaHub::WatchLevels），没人看就停。
//
// 拿到的是标签自己静音之后的声音：静音的标签是一片安静。
class MediaLevels {
 public:
  // 频谱：40 Hz 到 16 kHz 按对数分 32 段。
  static constexpr int kBands = 32;
  // 示波线：最近约 10 ms 的波形取 96 个点。
  static constexpr int kWavePoints = 96;
  // 记住最近这么多帧的电平（30 帧一秒，6 秒）。
  static constexpr size_t kHistory = 180;

  struct Frame {
    // 0…1：-60 dBFS 以下是 0。
    float level = 0;
    // 每段 0…1：-80 dB 以下是 0，-12 dB 以上是 1。
    std::array<float, kBands> bands{};
    // -1…1，按这一小段的峰值放大过（再乘上电平，安静时是一条平线）。
    std::array<float, kWavePoints> wave{};
    base::TimeTicks time;
  };

  // on_frame 在 UI 线程上调用（每来一帧）。
  MediaLevels(content::WebContents* contents, base::RepeatingClosure on_frame);
  MediaLevels(const MediaLevels&) = delete;
  MediaLevels& operator=(const MediaLevels&) = delete;
  ~MediaLevels();

  const Frame& frame() const { return frame_; }
  // 最近的电平，旧的在前。
  const base::circular_deque<float>& history() const { return history_; }

 private:
  class Analyzer;

  void OnFrame(const Frame& frame);

  Frame frame_;
  base::circular_deque<float> history_;
  base::RepeatingClosure on_frame_;
  std::unique_ptr<Analyzer> analyzer_;
  scoped_refptr<media::AudioInputDevice> device_;
  base::WeakPtrFactory<MediaLevels> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_MEDIA_MEDIA_LEVELS_H_
