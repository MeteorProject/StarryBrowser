// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DESIGN_TYPOGRAPHY_H_
#define STARRY_BROWSER_UI_DESIGN_TYPOGRAPHY_H_

#include "ui/gfx/font.h"
#include "ui/gfx/font_list.h"

namespace starry {

// 系统界面字体，按指定大小（DIP）和字重从平台默认字体派生并缓存。
// 各平台的系统字体不同（macOS 是苹方 / SF，Windows 是微软雅黑 / Segoe UI），尺寸一致。
const gfx::FontList& UiFont(int size,
                            gfx::Font::Weight weight = gfx::Font::Weight::NORMAL);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DESIGN_TYPOGRAPHY_H_
