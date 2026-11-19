// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_THUMBNAIL_CAPTURE_STATE_H_
#define STARRY_BROWSER_UI_TABS_THUMBNAIL_CAPTURE_STATE_H_

namespace starry {

struct ThumbnailCaptureState {
  bool is_capturing = false;
  bool capture_permitted = false;
};

// Capture notifications can synchronously reprioritize or remove any tab.
// Commit both scheduling decisions before calling out. Then notify only tabs
// whose current state has not been delivered, stopping captures before starting
// others. Re-scan after each callback without retaining a possibly removed
// node.
template <typename Tabs, typename TabNode>
void UpdateThumbnailCaptureState(Tabs& tabs,
                                 TabNode* descheduled_tab,
                                 TabNode* scheduled_tab) {
  if (descheduled_tab) {
    descheduled_tab->is_capturing = false;
  }
  if (scheduled_tab) {
    scheduled_tab->is_capturing = true;
  }

  for (;;) {
    auto pending = tabs.end();
    for (auto it = tabs.begin(); it != tabs.end(); ++it) {
      if (it->second.capture_permitted == it->second.is_capturing) {
        continue;
      }
      pending = it;
      if (!it->second.is_capturing) {
        break;
      }
    }
    if (pending == tabs.end()) {
      return;
    }
    auto& node = pending->second;
    const bool permitted = node.is_capturing;
    node.capture_permitted = permitted;
    node.capturer->SetCapturePermittedByScheduler(permitted);
  }
}

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_THUMBNAIL_CAPTURE_STATE_H_
