// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_INSTALLER_WIN_INSTALL_H_
#define STARRY_INSTALLER_WIN_INSTALL_H_

#include <windows.h>

#include <string>

namespace starry::installer {

struct InstalledInfo {
  InstalledInfo();
  InstalledInfo(const InstalledInfo&);
  InstalledInfo& operator=(const InstalledInfo&);
  ~InstalledInfo();

  bool found = false;
  bool system_level = false;
  std::wstring version;          // 4-part Windows version, e.g. "1.0.0.0".
  std::wstring display_version;  // Starry SemVer, e.g. "1.0.0".
  std::wstring exe;
};

struct InstallOptions {
  bool system_level = false;
  bool desktop_shortcut = true;
};

struct InstallResult {
  bool started = false;    // The embedded installer ran to completion.
  bool cancelled = false;  // The user declined the UAC prompt.
  DWORD exit_code = 0;     // mini_installer / setup.exe exit code.
  DWORD error = 0;         // Win32 error when it could not run.
};

// Returns the per-user install, falling back to the all-users install.
InstalledInfo FindInstalled();

int CompareVersions(const std::wstring& a, const std::wstring& b);

std::wstring PayloadVersion();         // "1.0.0.0"
std::wstring PayloadDisplayVersion();  // "1.0.0"

// Base install folder (without the trailing "Application" directory).
std::wstring InstallDir(bool system_level);

// Returns whether the target volume has `bytes` free and writes its drive
// root (e.g. "C:") to `drive`.
bool HasFreeSpace(bool system_level, ULONGLONG bytes, std::wstring* drive);

bool IsElevated();

// Extracts and runs the embedded mini_installer.exe, elevating via UAC for
// all-users installs when not already elevated.
InstallResult RunInstaller(const InstallOptions& options);

// Launches the browser as the signed-in user (never elevated).
void LaunchBrowser(const std::wstring& exe);

// Opens setup.exe's log, or the temp folder when none exists.
void OpenInstallerLog();

}  // namespace starry::installer

#endif  // STARRY_INSTALLER_WIN_INSTALL_H_
