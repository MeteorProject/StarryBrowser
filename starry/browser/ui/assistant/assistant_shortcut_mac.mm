// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/assistant_shortcut_mac.h"

#import <AppKit/AppKit.h>

namespace starry {

namespace {

void Release(NSMenu* menu) {
  for (NSMenuItem* item in menu.itemArray) {
    if (item.hasSubmenu) {
      Release(item.submenu);
      continue;
    }
    if ([item.keyEquivalent isEqualToString:@"e"] &&
        item.keyEquivalentModifierMask == NSEventModifierFlagCommand) {
      item.keyEquivalent = @"";
    }
  }
}

}  // namespace

void ReleaseCommandEShortcut() {
  if (NSMenu* menu = NSApp.mainMenu) {
    Release(menu);
  }
}

}  // namespace starry
