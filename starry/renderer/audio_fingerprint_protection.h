// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_AUDIO_FINGERPRINT_PROTECTION_H_
#define STARRY_RENDERER_AUDIO_FINGERPRINT_PROTECTION_H_

#include <cstdint>
#include <optional>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {
class AudioBuffer;
class AudioBus;
class AudioParamHandler;
class ExecutionContext;
}  // namespace blink

namespace starry {

// These run on the JS/context thread, never the real-time audio thread. Only
// computed outputs are protected; user-created PCM and the live graph are not.
MODULES_EXPORT void ProtectOfflineAudio(blink::ExecutionContext* context,
                                        blink::AudioBuffer* buffer);
MODULES_EXPORT void ProtectAudioSamples(blink::ExecutionContext* context,
                                        base::span<float> samples);
MODULES_EXPORT void ProtectAudioBytes(blink::ExecutionContext* context,
                                      base::span<uint8_t> samples,
                                      bool time_domain);
MODULES_EXPORT float ProtectAudioValue(blink::ExecutionContext* context,
                                       float value);

// AudioWorkletProcessor.process() receives the node's inputs, and parameters
// fed by audio-rate connections, as raw samples on the rendering thread. That
// thread must not ask the browser, so the node keeps the policy it was created
// with. The graph keeps the original samples; process() gets protected copies.
class MODULES_EXPORT AudioWorkletProtection {
 public:
  AudioWorkletProtection();
  AudioWorkletProtection(const AudioWorkletProtection&) = delete;
  AudioWorkletProtection& operator=(const AudioWorkletProtection&) = delete;
  ~AudioWorkletProtection();

  // Main thread: the node's BaseAudioContext execution context.
  void Initialize(blink::ExecutionContext* context);

  // Rendering thread, before process(). Replaces each connected input with a
  // protected copy, or with null (no channels) if a copy cannot be allocated.
  void ProtectInputs(blink::Vector<scoped_refptr<blink::AudioBus>>& inputs);
  // Rendering thread: sample-accurate values the handler computed for
  // `param`. Only parameters with audio-rate inputs are changed; automation
  // and plain values stay exact.
  void ProtectParam(const blink::AudioParamHandler& param,
                    base::span<float> values) const;

 private:
  std::optional<uint64_t> seed_;
  blink::Vector<scoped_refptr<blink::AudioBus>> copies_;
};

// AudioContext.outputLatency for a protected context (null otherwise). The
// output timestamp and playback statistics follow it.
MODULES_EXPORT std::optional<double> ProtectedOutputLatency(
    blink::ExecutionContext* context);
// AudioPlaybackStats latencies. Zero (nothing measured yet) stays zero.
MODULES_EXPORT double ProtectPlaybackLatency(blink::ExecutionContext* context,
                                             double seconds);

}  // namespace starry

#endif  // STARRY_RENDERER_AUDIO_FINGERPRINT_PROTECTION_H_
