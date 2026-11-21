// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_INSTALLER_WIN_SETUP_WINDOW_H_
#define STARRY_INSTALLER_WIN_SETUP_WINDOW_H_

#include <windows.h>

#include "starry/installer/win/install.h"

namespace starry::installer {

inline constexpr wchar_t kWindowClass[] = L"StarrySetupWindow";

int RunSetupWindow(HINSTANCE instance, const InstallOptions& defaults);

}  // namespace starry::installer

#endif  // STARRY_INSTALLER_WIN_SETUP_WINDOW_H_
