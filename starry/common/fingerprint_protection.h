// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_FINGERPRINT_PROTECTION_H_
#define STARRY_COMMON_FINGERPRINT_PROTECTION_H_

#include <atomic>
#include <cstdint>

namespace starry {

// Marks renderers whose browser provides the Starry policy interface. Blink
// unit tests and other embedders must not request an unregistered Mojo service.
inline constexpr char kFingerprintServiceSwitch[] =
    "starry-fingerprint-service";

// The browser owns the writable mapping; renderers only receive read access.
// A read is enough to notice a preference change, even inside a busy worker.
// Only a new generation requires another IPC. No URLs or secret keys are
// stored in this shared region.
//
// A renderer belongs to one profile, so all of its contexts share the region.
// `enabled` is zero while the profile's mode protects nothing (off, or private
// browsing only in a regular window): then no context needs to ask at all. The
// browser stores it before advancing `generation`.
struct FingerprintPolicyVersion {
  std::atomic<uint32_t> generation{0};
  std::atomic<uint32_t> enabled{0};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

}  // namespace starry

#endif  // STARRY_COMMON_FINGERPRINT_PROTECTION_H_
