// Copyright 2026 The Starry Browser Authors

#include "starry/browser/media/media_levels.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
#include <utility>

#include "base/files/platform_file.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/unguessable_token.h"
#include "content/public/browser/audio_service.h"
#include "content/public/browser/web_contents.h"
#include "media/audio/audio_input_device.h"
#include "media/audio/audio_input_ipc.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_capturer_source.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "media/mojo/common/input_error_code_converter.h"
#include "media/mojo/mojom/audio_data_pipe.mojom.h"
#include "media/mojo/mojom/audio_input_stream.mojom.h"
#include "media/mojo/mojom/audio_stream_factory.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

namespace starry {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBufferFrames = 480;
constexpr size_t kFftSize = 1024;
// 每秒最多交这么多帧给 UI 线程。
constexpr base::TimeDelta kFrameInterval = base::Milliseconds(32);
constexpr float kMinFrequency = 40;
constexpr float kMaxFrequency = 16000;

class DummyObserver final : public media::mojom::AudioInputStreamObserver {
 public:
  void DidStartRecording() final {}
};

// 回环流的 AudioInputIPC（同
// content/browser/devtools/protocol/media_recorder.cc 的
// LoopbackAudioInputIPC）：按标签的 audio group 开回环流。不调用
// BindMuter，所以原声 照常放。
class LoopbackAudioInputIPC : public media::AudioInputIPC,
                              public media::mojom::AudioInputStreamClient {
 public:
  explicit LoopbackAudioInputIPC(const base::UnguessableToken& group_id)
      : group_id_(group_id) {
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }
  ~LoopbackAudioInputIPC() override = default;

  // media::AudioInputIPC:
  void CreateStream(media::AudioInputIPCDelegate* delegate,
                    const media::AudioParameters& params,
                    bool automatic_gain_control,
                    uint32_t total_segments) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    delegate_ = delegate;
    content::GetAudioService().BindStreamFactory(
        stream_factory_.BindNewPipeAndPassReceiver());
    auto client = stream_client_receiver_.BindNewPipeAndPassRemote();
    stream_client_receiver_.set_disconnect_handler(base::BindOnce(
        &LoopbackAudioInputIPC::OnError, weak_factory_.GetWeakPtr(),
        media::mojom::InputStreamErrorCode::kUnknown));
    // 观察者一定要接着，不然音频服务会结束这个流。
    mojo::PendingRemote<media::mojom::AudioInputStreamObserver> observer;
    mojo::MakeSelfOwnedReceiver(std::make_unique<DummyObserver>(),
                                observer.InitWithNewPipeAndPassReceiver());
    stream_factory_->CreateLoopbackStream(
        stream_.BindNewPipeAndPassReceiver(), std::move(client),
        std::move(observer), params, total_segments, group_id_,
        base::BindOnce(&LoopbackAudioInputIPC::StreamCreated,
                       weak_factory_.GetWeakPtr()));
  }

  void RecordStream() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (stream_.is_bound()) {
      stream_->Record();
    }
  }

  void SetVolume(double volume) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (stream_.is_bound()) {
      stream_->SetVolume(volume);
    }
  }

  void SetOutputDeviceForAec(const std::string& output_device_id) override {}

  void CloseStream() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    delegate_ = nullptr;
    stream_client_receiver_.reset();
    stream_.reset();
    weak_factory_.InvalidateWeakPtrs();
  }

  // media::mojom::AudioInputStreamClient:
  void OnError(media::mojom::InputStreamErrorCode code) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (delegate_) {
      delegate_->OnError(media::ConvertToCaptureCallbackCode(code));
    }
  }

  void OnMutedStateChanged(bool is_muted) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (delegate_) {
      delegate_->OnMuted(is_muted);
    }
  }

 private:
  void StreamCreated(media::mojom::ReadWriteAudioDataPipePtr data_pipe) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (data_pipe.is_null() || !delegate_) {
      OnError(media::mojom::InputStreamErrorCode::kUnknown);
      return;
    }
    base::ScopedPlatformFile socket_handle =
        data_pipe->socket.TakePlatformFile();
    delegate_->OnStreamCreated(std::move(data_pipe->shared_memory),
                               std::move(socket_handle), false);
  }

  SEQUENCE_CHECKER(sequence_checker_);
  base::UnguessableToken group_id_;
  raw_ptr<media::AudioInputIPCDelegate> delegate_ = nullptr;
  mojo::Remote<media::mojom::AudioStreamFactory> stream_factory_;
  mojo::Remote<media::mojom::AudioInputStream> stream_;
  mojo::Receiver<media::mojom::AudioInputStreamClient> stream_client_receiver_{
      this};
  base::WeakPtrFactory<LoopbackAudioInputIPC> weak_factory_{this};
};

// 原地的基 2 FFT。
void Fft(std::array<std::complex<float>, kFftSize>& data) {
  for (size_t i = 1, j = 0; i < kFftSize; ++i) {
    size_t bit = kFftSize >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(data[i], data[j]);
    }
  }
  for (size_t length = 2; length <= kFftSize; length <<= 1) {
    const float angle = -2 * std::numbers::pi_v<float> / length;
    const std::complex<float> step(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < kFftSize; i += length) {
      std::complex<float> w(1, 0);
      for (size_t k = 0; k < length / 2; ++k) {
        const std::complex<float> u = data[i + k];
        const std::complex<float> v = data[i + k + length / 2] * w;
        data[i + k] = u + v;
        data[i + k + length / 2] = u - v;
        w *= step;
      }
    }
  }
}

}  // namespace

// 音频线程上的那一半：攒样本、算一帧、交给 UI 线程。
class MediaLevels::Analyzer
    : public media::AudioCapturerSource::CaptureCallback {
 public:
  Analyzer(scoped_refptr<base::SequencedTaskRunner> ui,
           base::WeakPtr<MediaLevels> owner)
      : ui_(std::move(ui)), owner_(std::move(owner)) {
    for (size_t i = 0; i < kFftSize; ++i) {
      window_[i] = 0.5f - 0.5f * std::cos(2 * std::numbers::pi_v<float> * i /
                                          (kFftSize - 1));
    }
    // 频段的边界（FFT 的第几格），每段至少一格。
    const float resolution = static_cast<float>(kSampleRate) / kFftSize;
    size_t previous = 0;
    for (int b = 0; b <= kBands; ++b) {
      const float frequency =
          kMinFrequency * std::pow(kMaxFrequency / kMinFrequency,
                                   static_cast<float>(b) / kBands);
      size_t bin = static_cast<size_t>(std::lround(frequency / resolution));
      bin = std::clamp<size_t>(bin, b == 0 ? 1 : previous + 1, kFftSize / 2);
      edges_[b] = bin;
      previous = bin;
    }
  }

  // media::AudioCapturerSource::CaptureCallback:
  void Capture(const media::AudioBus* bus,
               base::TimeTicks time,
               const media::AudioGlitchInfo& glitch_info,
               double volume) override {
    const int frames = bus->frames();
    const int channels = std::max(1, bus->channels());
    for (int i = 0; i < frames; ++i) {
      float sample = 0;
      for (int c = 0; c < bus->channels(); ++c) {
        sample += bus->channel(c)[i];
      }
      sample /= channels;
      ring_[write_] = sample;
      write_ = (write_ + 1) % kFftSize;
      block_squares_ += sample * sample;
    }
    block_frames_ += frames;
    if (last_frame_.is_null() || time - last_frame_ >= kFrameInterval) {
      last_frame_ = time;
      Analyze(time);
    }
  }

  void OnCaptureError(media::AudioCapturerSource::ErrorCode code,
                      const std::string& message) override {}
  void OnCaptureMuted(bool is_muted) override {}

 private:
  void Analyze(base::TimeTicks time) {
    Frame frame;
    frame.time = time;
    const float rms =
        block_frames_ > 0 ? std::sqrt(block_squares_ / block_frames_) : 0;
    block_squares_ = 0;
    block_frames_ = 0;
    const float level_db = 20 * std::log10(std::max(rms, 1e-6f));
    frame.level = std::clamp((level_db + 60) / 60, 0.0f, 1.0f);

    // 频谱：最近 1024 个样本加汉宁窗。满幅的正弦在窗后的峰值是 N / 4。
    for (size_t i = 0; i < kFftSize; ++i) {
      data_[i] =
          std::complex<float>(ring_[(write_ + i) % kFftSize] * window_[i], 0);
    }
    Fft(data_);
    for (int b = 0; b < kBands; ++b) {
      float peak = 0;
      for (size_t k = edges_[b]; k < std::max(edges_[b] + 1, edges_[b + 1]);
           ++k) {
        peak = std::max(peak, std::abs(data_[std::min(k, kFftSize / 2)]));
      }
      const float db =
          20 * std::log10(std::max(peak / (kFftSize / 4.0f), 1e-6f));
      frame.bands[b] = std::clamp((db + 80) / 68, 0.0f, 1.0f);
    }

    // 示波线：最近 480 个样本取 96 个点，按峰值放大。
    constexpr size_t kWaveSamples = 480;
    float peak = 0;
    for (size_t i = 0; i < kWaveSamples; ++i) {
      peak = std::max(
          peak,
          std::abs(ring_[(write_ + kFftSize - kWaveSamples + i) % kFftSize]));
    }
    const float gain = peak > 1e-4f ? std::min(1.0f, frame.level * 1.4f) /
                                          std::max(peak, 0.02f)
                                    : 0;
    for (int p = 0; p < kWavePoints; ++p) {
      const size_t i = static_cast<size_t>(p) * kWaveSamples / kWavePoints;
      frame.wave[p] = std::clamp(
          ring_[(write_ + kFftSize - kWaveSamples + i) % kFftSize] * gain,
          -1.0f, 1.0f);
    }
    ui_->PostTask(FROM_HERE,
                  base::BindOnce(&MediaLevels::OnFrame, owner_, frame));
  }

  const scoped_refptr<base::SequencedTaskRunner> ui_;
  const base::WeakPtr<MediaLevels> owner_;
  std::array<float, kFftSize> ring_{};
  std::array<float, kFftSize> window_{};
  std::array<std::complex<float>, kFftSize> data_{};
  std::array<size_t, kBands + 1> edges_{};
  size_t write_ = 0;
  float block_squares_ = 0;
  int block_frames_ = 0;
  base::TimeTicks last_frame_;
};

MediaLevels::MediaLevels(content::WebContents* contents,
                         base::RepeatingClosure on_frame)
    : on_frame_(std::move(on_frame)) {
  analyzer_ =
      std::make_unique<Analyzer>(base::SequencedTaskRunner::GetCurrentDefault(),
                                 weak_factory_.GetWeakPtr());
  device_ = base::MakeRefCounted<media::AudioInputDevice>(
      std::make_unique<LoopbackAudioInputIPC>(contents->GetAudioGroupId()),
      media::AudioInputDevice::Purpose::kLoopback,
      media::AudioInputDevice::DeadStreamDetection::kDisabled);
  device_->Initialize(
      media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                             media::ChannelLayoutConfig::Stereo(), kSampleRate,
                             kBufferFrames),
      analyzer_.get());
  device_->Start();
}

MediaLevels::~MediaLevels() {
  // Stop 等音频线程停下，之后 Analyzer 不会再被调用。
  device_->Stop();
}

void MediaLevels::OnFrame(const Frame& frame) {
  frame_ = frame;
  history_.push_back(frame.level);
  while (history_.size() > kHistory) {
    history_.pop_front();
  }
  if (on_frame_) {
    on_frame_.Run();
  }
}

}  // namespace starry
