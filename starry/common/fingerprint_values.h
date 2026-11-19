// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_FINGERPRINT_VALUES_H_
#define STARRY_COMMON_FINGERPRINT_VALUES_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"

// Values reported to protected pages. Shared by Blink (JavaScript and
// subresource client hints) and the browser (navigation client hints), so a
// page reads the same value from every source.
namespace starry {

// Bucket logical processor count into 4 or 8 cores.
constexpr unsigned ProtectedHardwareConcurrency(unsigned cores) {
  return cores < 8 ? 4 : 8;
}

// navigator.deviceMemory and Device-Memory. Chromium reports powers of two
// from 2 to 32 GB; keep the same two buckets as the processor count.
constexpr float ProtectedDeviceMemory(float gigabytes) {
  return gigabytes < 8 ? 4 : 8;
}

// devicePixelRatio and DPR: the display scale in two buckets, without page
// zoom (a site's zoom level, or a raised default zoom, is identifying too).
// Mac displays are already 1 or 2.
constexpr double ProtectedDevicePixelRatio(double scale) {
  return scale < 1.5 ? 1 : 2;
}

// screen.colorDepth / pixelDepth and the `color` media feature of an
// ordinary sRGB, standard dynamic range display.
inline constexpr unsigned kProtectedColorDepth = 24;
inline constexpr int kProtectedColorBitsPerComponent = 8;

// AudioContext.outputLatency (and what getOutputTimestamp() and playbackStats
// imply): built-in Mac output after Chromium's 8 ms rounding. Wired, USB and
// Bluetooth outputs are told apart by this.
inline constexpr double kProtectedOutputLatency = 0.016;

// A common broadband reading after Chromium's own rounding (50 ms RTT
// buckets, downlink capped at 10 Mbps). Change events are not dispatched.
inline constexpr char kProtectedEffectiveType[] = "4g";
inline constexpr uint32_t kProtectedRttMs = 50;
inline constexpr double kProtectedDownlinkMbps = 10;

// Keep the major part and the number of parts: "157.0.8103.51" → "157.0.0.0",
// "26.6.1" → "26.0.0". The reduced User-Agent string already exposes the major
// browser version; nothing below it is revealed.
inline std::string MajorOnlyVersion(std::string_view version) {
  const size_t dot = version.find('.');
  if (dot == std::string_view::npos) {
    return std::string(version);
  }
  std::string result(version.substr(0, dot));
  for (size_t i = dot; i < version.size(); ++i) {
    if (version[i] == '.') {
      result += ".0";
    }
  }
  return result;
}

// High-entropy client hints: full browser versions and the OS point release.
// Architecture, bitness, model and form factors stay as they are (coarse, and
// observable through other means).
inline void ReduceUserAgentMetadata(blink::UserAgentMetadata& metadata) {
  metadata.full_version = MajorOnlyVersion(metadata.full_version);
  for (auto& brand : metadata.brand_full_version_list) {
    brand.version = MajorOnlyVersion(brand.version);
  }
  metadata.platform_version = MajorOnlyVersion(metadata.platform_version);
}

}  // namespace starry

#endif  // STARRY_COMMON_FINGERPRINT_VALUES_H_
