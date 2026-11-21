// Copyright 2026 The Starry Browser Authors

// Usage: starry_setup.exe [--silent] [--system-level] [--no-desktop-shortcut]
//                         [--lang=zh-Hans|zh-Hant|en|ja|ko]

#include <objbase.h>

#include <windows.h>

#include <string>
#include <vector>

#include "starry/common/platform_branding.h"
#include "starry/installer/win/install.h"
#include "starry/installer/win/setup_window.h"
#include "starry/installer/win/strings.h"

namespace {

struct Args {
  bool silent = false;
  starry::installer::InstallOptions options;
  std::wstring lang;
};

std::vector<std::wstring> SplitCommandLine(const std::wstring& line) {
  std::vector<std::wstring> parts;
  std::wstring current;
  bool quoted = false;
  bool pending = false;
  for (wchar_t ch : line) {
    if (ch == L'"') {
      quoted = !quoted;
      pending = true;
    } else if (!quoted && (ch == L' ' || ch == L'\t')) {
      if (pending) {
        parts.push_back(current);
        current.clear();
        pending = false;
      }
    } else {
      current.push_back(ch);
      pending = true;
    }
  }
  if (pending) {
    parts.push_back(current);
  }
  if (!parts.empty()) {
    parts.erase(parts.begin());
  }
  return parts;
}

Args ParseArgs() {
  Args args;
  for (const std::wstring& part : SplitCommandLine(GetCommandLineW())) {
    if (part == L"--silent") {
      args.silent = true;
    } else if (part == L"--system-level") {
      args.options.system_level = true;
    } else if (part == L"--no-desktop-shortcut") {
      args.options.desktop_shortcut = false;
    } else if (part.starts_with(L"--lang=")) {
      args.lang = part.substr(7);
    }
  }
  return args;
}

int RunSilent(const starry::installer::InstallOptions& options) {
  const starry::installer::InstallResult result =
      starry::installer::RunInstaller(options);
  if (result.started) {
    return static_cast<int>(result.exit_code);
  }
  return result.error ? static_cast<int>(result.error) : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const Args args = ParseArgs();
  starry::installer::InitLanguage(args.lang);
  const HRESULT com = CoInitializeEx(
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  int exit_code = 0;
  if (args.silent) {
    exit_code = RunSilent(args.options);
  } else {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // One installer window at a time: bring the open one forward.
    HANDLE mutex = CreateMutexW(
        nullptr, FALSE, L"Local\\StarrySetup-" STARRY_PRODUCT_FULLNAME_W);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND existing = FindWindowW(starry::installer::kWindowClass, nullptr);
      if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
      }
    } else {
      exit_code = starry::installer::RunSetupWindow(instance, args.options);
    }
    if (mutex) {
      CloseHandle(mutex);
    }
  }

  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return exit_code;
}
