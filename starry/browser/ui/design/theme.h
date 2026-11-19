// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DESIGN_THEME_H_
#define STARRY_BROWSER_UI_DESIGN_THEME_H_

#include <optional>

#include "third_party/skia/include/core/SkColor.h"

namespace starry {

// 界面主题：以 Zinc 灰阶为底色，支持按种子色（如网页 theme-color）为整套界面着色。
struct Theme {
  bool is_dark = true;
  SkColor primary = SK_ColorTRANSPARENT;
  SkColor primary_container = SK_ColorTRANSPARENT;
  SkColor on_primary = SK_ColorTRANSPARENT;
  SkColor secondary = SK_ColorTRANSPARENT;
  // 窗口底色。
  SkColor surface = SK_ColorTRANSPARENT;
  SkColor surface_alt = SK_ColorTRANSPARENT;
  // 内容卡片、面板的底色。
  SkColor surface_panel = SK_ColorTRANSPARENT;
  SkColor surface_bright = SK_ColorTRANSPARENT;
  SkColor on_surface = SK_ColorTRANSPARENT;
  SkColor on_surface_variant = SK_ColorTRANSPARENT;
  SkColor outline = SK_ColorTRANSPARENT;
  SkColor outline_variant = SK_ColorTRANSPARENT;
  SkColor accent = SK_ColorTRANSPARENT;

  SkColor warning() const;
  SkColor danger() const;

  // on_surface 按不透明度叠在别的颜色上：悬停、选中这些浅底。
  SkColor OnSurface(float opacity) const;

  static Theme DarkBase();
  static Theme LightBase();
  // 无痕窗口的固定底色：偏紫的深色。
  static Theme PrivateBrowsing();
  // 用种子色给主题着色；种子色为空或几乎没有饱和度时就是底色。tint_surfaces 为 false
  // 时只给强调色着色，底色不变。
  static Theme Make(std::optional<SkColor> seed, bool dark, bool tint_surfaces);

  // 页面色带的颜色。
  SkColor Wash(SkColor color) const;

  // 两套主题之间的过渡（跟着网页变色时逐帧插值）。深浅取更接近的那一套。
  static Theme Lerp(const Theme& from, const Theme& to, double t);
  static SkColor LerpColor(SkColor from, SkColor to, double t);

  bool operator==(const Theme&) const = default;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DESIGN_THEME_H_
