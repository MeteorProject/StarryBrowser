// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/design/theme.h"

#include <algorithm>
#include <cmath>

namespace starry {

namespace {

// 色相 0…1（会绕回），饱和度、亮度 0…1。
SkColor Hsb(float hue, float saturation, float brightness) {
  hue = hue - std::floor(hue);
  SkScalar hsv[3] = {hue * 360.0f, std::clamp(saturation, 0.0f, 1.0f),
                     std::clamp(brightness, 0.0f, 1.0f)};
  return SkHSVToColor(hsv);
}

struct HsbColor {
  float hue;
  float saturation;
  float brightness;
};

HsbColor ToHsb(SkColor color) {
  SkScalar hsv[3];
  SkColorToHSV(color, hsv);
  return {hsv[0] / 360.0f, hsv[1], hsv[2]};
}

}  // namespace

SkColor Theme::warning() const {
  return is_dark ? SkColorSetRGB(0xF5, 0xB9, 0x4E)
                 : SkColorSetRGB(0xB7, 0x79, 0x1F);
}

SkColor Theme::danger() const {
  return is_dark ? SkColorSetRGB(0xF0, 0x62, 0x5D)
                 : SkColorSetRGB(0xDC, 0x3F, 0x3A);
}

SkColor Theme::OnSurface(float opacity) const {
  return SkColorSetA(
      on_surface,
      static_cast<U8CPU>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255)));
}

// static
Theme Theme::DarkBase() {
  Theme theme;
  theme.is_dark = true;
  theme.primary = SkColorSetRGB(244, 244, 245);
  theme.primary_container = SkColorSetRGB(63, 63, 70);
  theme.on_primary = SkColorSetRGB(24, 24, 27);
  theme.secondary = SkColorSetRGB(161, 161, 170);
  theme.surface = SkColorSetRGB(16, 16, 20);
  theme.surface_alt = SkColorSetRGB(39, 39, 42);
  theme.surface_panel = SkColorSetRGB(24, 24, 28);
  theme.surface_bright = SkColorSetRGB(72, 72, 78);
  theme.on_surface = SkColorSetRGB(228, 228, 231);
  theme.on_surface_variant = SkColorSetRGB(161, 161, 170);
  theme.outline = SkColorSetRGB(82, 82, 91);
  theme.outline_variant = SkColorSetRGB(46, 46, 51);
  theme.accent = SkColorSetRGB(244, 244, 245);
  return theme;
}

// static
Theme Theme::LightBase() {
  Theme theme;
  theme.is_dark = false;
  theme.primary = SkColorSetRGB(24, 24, 27);
  theme.primary_container = SkColorSetRGB(228, 228, 231);
  theme.on_primary = SkColorSetRGB(255, 255, 255);
  theme.secondary = SkColorSetRGB(82, 82, 91);
  theme.surface = SkColorSetRGB(246, 246, 246);
  theme.surface_alt = SkColorSetRGB(250, 250, 251);
  theme.surface_panel = SkColorSetRGB(255, 255, 255);
  theme.surface_bright = SkColorSetRGB(255, 255, 255);
  theme.on_surface = SkColorSetRGB(24, 24, 27);
  theme.on_surface_variant = SkColorSetRGB(113, 113, 122);
  theme.outline = SkColorSetRGB(212, 212, 216);
  theme.outline_variant = SkColorSetRGB(228, 228, 231);
  theme.accent = SkColorSetRGB(24, 24, 27);
  return theme;
}

// static
Theme Theme::PrivateBrowsing() {
  return Make(SkColorSetRGB(124, 92, 255), /*dark=*/true,
              /*tint_surfaces=*/true);
}

// static
Theme Theme::Make(std::optional<SkColor> seed, bool dark, bool tint_surfaces) {
  Theme theme = dark ? DarkBase() : LightBase();
  if (!seed) {
    return theme;
  }
  HsbColor hsb = ToHsb(*seed);
  if (hsb.saturation <= 0.05f) {
    return theme;
  }
  const float h = hsb.hue;
  const float chroma = std::min(hsb.saturation, 0.7f) / 0.7f;
  if (dark) {
    theme.primary = Hsb(h, 0.18f * chroma + 0.04f, 0.96f);
    theme.primary_container = Hsb(h, 0.38f * chroma + 0.05f, 0.36f);
    theme.on_primary = Hsb(h, 0.30f, 0.12f);
    theme.secondary = Hsb(h, 0.10f, 0.68f);
    theme.accent = Hsb(h, 0.5f * chroma + 0.12f, 0.93f);
    if (tint_surfaces) {
      theme.surface = Hsb(h, 0.16f * chroma, 0.075f);
      theme.surface_panel = Hsb(h, 0.15f * chroma, 0.105f);
      theme.surface_alt = Hsb(h, 0.12f * chroma, 0.15f);
      theme.surface_bright = Hsb(h, 0.10f * chroma, 0.29f);
      theme.on_surface = Hsb(h, 0.04f, 0.91f);
      theme.on_surface_variant = Hsb(h, 0.07f, 0.66f);
      theme.outline = Hsb(h, 0.10f, 0.36f);
      theme.outline_variant = Hsb(h, 0.10f, 0.19f);
    }
  } else {
    theme.primary = Hsb(h, 0.42f * chroma + 0.05f, 0.18f);
    theme.primary_container = Hsb(h, 0.20f * chroma + 0.02f, 0.92f);
    theme.on_primary = SK_ColorWHITE;
    theme.secondary = Hsb(h, 0.12f, 0.38f);
    theme.accent = Hsb(h, 0.6f * chroma + 0.15f, 0.78f);
    if (tint_surfaces) {
      theme.surface = Hsb(h, 0.05f * chroma, 0.965f);
      theme.surface_panel = Hsb(h, 0.02f * chroma, 0.995f);
      theme.surface_alt = Hsb(h, 0.03f * chroma, 0.985f);
      theme.on_surface = Hsb(h, 0.22f, 0.12f);
      theme.on_surface_variant = Hsb(h, 0.10f, 0.47f);
      theme.outline = Hsb(h, 0.08f, 0.84f);
      theme.outline_variant = Hsb(h, 0.05f, 0.91f);
    }
  }
  return theme;
}

// static
SkColor Theme::LerpColor(SkColor from, SkColor to, double t) {
  t = std::clamp(t, 0.0, 1.0);
  const auto mix = [t](U8CPU a, U8CPU b) {
    return static_cast<U8CPU>(
        std::lround(a + (static_cast<double>(b) - a) * t));
  };
  return SkColorSetARGB(mix(SkColorGetA(from), SkColorGetA(to)),
                        mix(SkColorGetR(from), SkColorGetR(to)),
                        mix(SkColorGetG(from), SkColorGetG(to)),
                        mix(SkColorGetB(from), SkColorGetB(to)));
}

// static
Theme Theme::Lerp(const Theme& from, const Theme& to, double t) {
  if (t <= 0) {
    return from;
  }
  if (t >= 1) {
    return to;
  }
  Theme theme;
  theme.is_dark = t < 0.5 ? from.is_dark : to.is_dark;
  theme.primary = LerpColor(from.primary, to.primary, t);
  theme.primary_container =
      LerpColor(from.primary_container, to.primary_container, t);
  theme.on_primary = LerpColor(from.on_primary, to.on_primary, t);
  theme.secondary = LerpColor(from.secondary, to.secondary, t);
  theme.surface = LerpColor(from.surface, to.surface, t);
  theme.surface_alt = LerpColor(from.surface_alt, to.surface_alt, t);
  theme.surface_panel = LerpColor(from.surface_panel, to.surface_panel, t);
  theme.surface_bright = LerpColor(from.surface_bright, to.surface_bright, t);
  theme.on_surface = LerpColor(from.on_surface, to.on_surface, t);
  theme.on_surface_variant =
      LerpColor(from.on_surface_variant, to.on_surface_variant, t);
  theme.outline = LerpColor(from.outline, to.outline, t);
  theme.outline_variant =
      LerpColor(from.outline_variant, to.outline_variant, t);
  theme.accent = LerpColor(from.accent, to.accent, t);
  return theme;
}

SkColor Theme::Wash(SkColor color) const {
  HsbColor hsb = ToHsb(color);
  return is_dark ? Hsb(hsb.hue, std::min(hsb.saturation, 0.62f), 0.46f)
                 : Hsb(hsb.hue, std::min(hsb.saturation, 0.5f) * 0.8f, 0.93f);
}

}  // namespace starry
