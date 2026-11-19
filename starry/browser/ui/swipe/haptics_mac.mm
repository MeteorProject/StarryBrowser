// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/swipe/haptics.h"

#import <AppKit/AppKit.h>

namespace starry {

void PerformHaptic(Haptic haptic) {
  const NSHapticFeedbackPattern pattern =
      haptic == Haptic::kLevelChange ? NSHapticFeedbackPatternLevelChange
                                     : NSHapticFeedbackPatternAlignment;
  [NSHapticFeedbackManager.defaultPerformer
      performFeedbackPattern:pattern
             performanceTime:NSHapticFeedbackPerformanceTimeNow];
}

}  // namespace starry
