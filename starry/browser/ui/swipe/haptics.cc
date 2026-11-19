// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/swipe/haptics.h"

namespace starry {

// Windows 的精确式触控板、Linux 的 libinput 都没有给应用用的振动接口。
void PerformHaptic(Haptic haptic) {}

}  // namespace starry
