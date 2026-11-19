// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/audio_fingerprint_protection.h"

#include <bit>
#include <cmath>

#include "starry/common/fingerprint_values.h"
#include "starry/renderer/fingerprint_protection.h"
#include "third_party/blink/renderer/modules/webaudio/audio_buffer.h"
#include "third_party/blink/renderer/modules/webaudio/audio_param_handler.h"
#include "third_party/blink/renderer/platform/audio/audio_bus.h"
#include "third_party/blink/renderer/platform/audio/audio_channel.h"

namespace starry {

namespace {

// An integer avalanche mixer, not a source of secrets. The browser already
// derives an independent site key with HMAC; this distributes that key over
// sample buckets without allocating or hashing on the audio-processing thread.
uint64_t Mix(uint64_t value) {
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

float ProtectSample(float value, uint64_t seed) {
  // Keep silence, exact unity and invalid/out-of-range response sentinels.
  if (!std::isfinite(value) || value == 0.0f || std::abs(value) == 1.0f) {
    return value;
  }
  uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint32_t bucket = bits & ~uint32_t{15};
  // At most fifteen ULPs. Basing the choice on the bucket makes the transform
  // idempotent and independent of read order, array length and slice offsets.
  bits =
      bucket | static_cast<uint32_t>(Mix(seed ^ bucket ^ 0x617564696fULL) & 15);
  return std::bit_cast<float>(bits);
}

}  // namespace

void ProtectOfflineAudio(blink::ExecutionContext* context,
                         blink::AudioBuffer* buffer) {
  const auto seed = FingerprintSeed(context);
  if (!seed || !buffer) {
    return;
  }
  // Called once after rendering stops, before resolving the promise or firing
  // oncomplete. Both getChannelData and copyFromChannel then see the same data.
  for (unsigned channel = 0; channel < buffer->numberOfChannels(); ++channel) {
    for (float& value : buffer->getChannelData(channel)->AsSpan()) {
      value = ProtectSample(value, *seed);
    }
  }
}

void ProtectAudioSamples(blink::ExecutionContext* context,
                         base::span<float> samples) {
  const auto seed = FingerprintSeed(context);
  if (!seed) {
    return;
  }
  for (float& value : samples) {
    value = ProtectSample(value, *seed);
  }
}

void ProtectAudioBytes(blink::ExecutionContext* context,
                       base::span<uint8_t> samples,
                       bool time_domain) {
  const auto seed = FingerprintSeed(context);
  if (!seed) {
    return;
  }
  for (uint8_t& value : samples) {
    if (value == 0 || value == 255 || (time_domain && value == 128)) {
      continue;
    }
    const uint8_t bucket = value & ~uint8_t{1};
    value = bucket |
            static_cast<uint8_t>(Mix(*seed ^ bucket ^ 0x617564696fULL) & 1);
  }
}

float ProtectAudioValue(blink::ExecutionContext* context, float value) {
  const auto seed = FingerprintSeed(context);
  return seed ? ProtectSample(value, *seed) : value;
}

AudioWorkletProtection::AudioWorkletProtection() = default;
AudioWorkletProtection::~AudioWorkletProtection() = default;

void AudioWorkletProtection::Initialize(blink::ExecutionContext* context) {
  seed_ = FingerprintSeed(context);
}

void AudioWorkletProtection::ProtectInputs(
    blink::Vector<scoped_refptr<blink::AudioBus>>& inputs) {
  if (!seed_) {
    return;
  }
  if (copies_.size() < inputs.size()) {
    copies_.resize(inputs.size());
  }
  for (blink::wtf_size_t i = 0; i < inputs.size(); ++i) {
    const scoped_refptr<blink::AudioBus>& input = inputs[i];
    if (!input || !input->NumberOfChannels()) {
      continue;
    }
    // The input bus can be an upstream node's output (in-place processing),
    // shared with other destinations; never write to it.
    scoped_refptr<blink::AudioBus>& copy = copies_[i];
    if (!copy || !copy->TopologyMatches(*input)) {
      copy = blink::AudioBus::TryCreate(input->NumberOfChannels(),
                                        input->length());
    }
    if (!copy) {
      inputs[i] = nullptr;
      continue;
    }
    for (unsigned channel = 0; channel < input->NumberOfChannels(); ++channel) {
      blink::AudioChannel* target = copy->Channel(channel);
      target->CopyFrom(input->Channel(channel));
      if (!target->IsSilent()) {
        for (float& value : target->MutableSpan()) {
          value = ProtectSample(value, *seed_);
        }
      }
    }
    inputs[i] = copy;
  }
}

void AudioWorkletProtection::ProtectParam(const blink::AudioParamHandler& param,
                                          base::span<float> values) const {
  if (!seed_ || !param.NumberOfRenderingConnections()) {
    return;
  }
  for (float& value : values) {
    value = ProtectSample(value, *seed_);
  }
}

std::optional<double> ProtectedOutputLatency(blink::ExecutionContext* context) {
  if (!ShouldProtectFingerprint(context)) {
    return std::nullopt;
  }
  return kProtectedOutputLatency;
}

double ProtectPlaybackLatency(blink::ExecutionContext* context,
                              double seconds) {
  if (seconds == 0) {
    return seconds;
  }
  return ProtectedOutputLatency(context).value_or(seconds);
}

}  // namespace starry
