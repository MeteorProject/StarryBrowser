// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_GOOGLE_SERVICES_H_
#define STARRY_COMMON_GOOGLE_SERVICES_H_

namespace starry {

// Chromium 157 has no build switch for page translation or Gemini. Gate their
// service entry points as well as the UI, including for existing profiles and
// when upstream experiments or command-line flags would enable them.
inline constexpr bool kGoogleTranslateEnabled = false;
inline constexpr bool kGeminiEnabled = false;
// Google image search can fall back to Lens even with LensStandalone disabled.
inline constexpr bool kGoogleLensEnabled = false;

}  // namespace starry

#endif  // STARRY_COMMON_GOOGLE_SERVICES_H_
