// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/mini_window/mini_window_activation.h"

// Windows、Linux：激活一个窗口不会把同一个程序的其他窗口带到前面，不用管层级。
namespace starry::mini_window {

void StartTrackingActivation() {}

void ConfigureWindow(views::Widget* widget) {}

void WindowShown(views::Widget* widget) {}

void WindowClosing(views::Widget* widget, bool moving_to_browser) {}

void DiscardWindowOrder() {}

}  // namespace starry::mini_window
