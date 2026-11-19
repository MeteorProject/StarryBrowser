// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SWIPE_HAPTICS_H_
#define STARRY_BROWSER_UI_SWIPE_HAPTICS_H_

namespace starry {

// 触控板的振动反馈：macOS 上是 Force Touch 触控板（NSHapticFeedbackManager），只在手指
// 放在触控板上时有感觉。没有这种触控板、系统设置里关了触感反馈时系统自己忽略；Windows、
// Linux 没有给应用用的触控板振动接口，什么都不做。
enum class Haptic {
  // 对齐到一个位置：拖过了翻页的距离、又退了回去。
  kAlignment,
  // 换了一档：上下选翻几页时换了一行。
  kLevelChange,
};

void PerformHaptic(Haptic haptic);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SWIPE_HAPTICS_H_
