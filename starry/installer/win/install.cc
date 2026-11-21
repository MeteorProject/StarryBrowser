// Copyright 2026 The Starry Browser Authors

#include "starry/installer/win/install.h"

#include <windows.h>

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "starry/common/platform_branding.h"
#include "starry/common/version.h"
#include "starry/installer/win/resource.h"

namespace starry::installer {

namespace {

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
    L"\\" STARRY_PRODUCT_FULLNAME_W;

std::wstring Widen(std::string_view ascii) {
  return std::wstring(ascii.begin(), ascii.end());
}

std::wstring Quote(const std::wstring& path) {
  return L"\"" + path + L"\"";
}

// setup.exe writes its uninstall entry to the 32-bit view.
std::wstring ReadRegString(HKEY root, const wchar_t* name) {
  constexpr DWORD kFlags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY;
  DWORD size = 0;
  if (RegGetValueW(root, kUninstallKey, name, kFlags, nullptr, nullptr,
                   &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t)) {
    return std::wstring();
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(root, kUninstallKey, name, kFlags, nullptr, value.data(),
                   &size) != ERROR_SUCCESS) {
    return std::wstring();
  }
  value.resize(size / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

bool FileExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) {
    result = path;
  }
  CoTaskMemFree(path);
  return result;
}

std::wstring TempDir() {
  std::wstring path(MAX_PATH + 1, L'\0');
  const DWORD length =
      GetTempPathW(static_cast<DWORD>(path.size()), path.data());
  if (length == 0 || length >= path.size()) {
    return std::wstring();
  }
  path.resize(length);
  return path;  // Ends with a backslash.
}

std::vector<unsigned> VersionParts(const std::wstring& version) {
  std::vector<unsigned> parts(1, 0);
  for (wchar_t ch : version) {
    if (ch == L'.') {
      parts.push_back(0);
    } else if (ch >= L'0' && ch <= L'9') {
      parts.back() = parts.back() * 10 + static_cast<unsigned>(ch - L'0');
    }
  }
  return parts;
}

bool WriteBytes(const std::wstring& path, const void* data, DWORD size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const bool ok =
      WriteFile(file, data, size, &written, nullptr) && written == size;
  CloseHandle(file);
  return ok;
}

bool ExtractPayload(const std::wstring& path) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource =
      FindResourceW(module, MAKEINTRESOURCEW(IDR_STARRY_PAYLOAD), RT_RCDATA);
  if (!resource) {
    return false;
  }
  HGLOBAL loaded = LoadResource(module, resource);
  const void* data = loaded ? LockResource(loaded) : nullptr;
  const DWORD size = SizeofResource(module, resource);
  return data && size && WriteBytes(path, data, size);
}

}  // namespace

InstalledInfo::InstalledInfo() = default;
InstalledInfo::InstalledInfo(const InstalledInfo&) = default;
InstalledInfo& InstalledInfo::operator=(const InstalledInfo&) = default;
InstalledInfo::~InstalledInfo() = default;

InstalledInfo FindInstalled() {
  InstalledInfo info;
  for (const bool system_level : {false, true}) {
    const HKEY root = system_level ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const std::wstring location = ReadRegString(root, L"InstallLocation");
    if (location.empty()) {
      continue;
    }
    const std::wstring exe = location + L"\\" + STARRY_WINDOWS_EXE;
    if (!FileExists(exe)) {
      continue;  // Left behind by a broken uninstall.
    }
    info.found = true;
    info.system_level = system_level;
    info.exe = exe;
    info.version = ReadRegString(root, L"Version");
    info.display_version = ReadRegString(root, L"DisplayVersion");
    if (info.display_version.empty()) {
      info.display_version = info.version;
    }
    return info;
  }
  return info;
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
  std::vector<unsigned> x = VersionParts(a);
  std::vector<unsigned> y = VersionParts(b);
  const size_t count = std::max(x.size(), y.size());
  x.resize(count, 0);
  y.resize(count, 0);
  for (size_t i = 0; i < count; ++i) {
    if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }
  return 0;
}

std::wstring PayloadVersion() {
  return Widen(starry::kWindowsVersion);
}

std::wstring PayloadDisplayVersion() {
  return Widen(starry::kVersion);
}

std::wstring InstallDir(bool system_level) {
  return KnownFolder(system_level ? FOLDERID_ProgramFiles
                                  : FOLDERID_LocalAppData) +
         L"\\" STARRY_PRODUCT_FULLNAME_W;
}

bool HasFreeSpace(bool system_level, ULONGLONG bytes, std::wstring* drive) {
  const std::wstring parent =
      KnownFolder(system_level ? FOLDERID_ProgramFiles : FOLDERID_LocalAppData);
  *drive = parent.substr(0, 2);
  ULARGE_INTEGER available = {};
  if (parent.empty() ||
      !GetDiskFreeSpaceExW(parent.c_str(), &available, nullptr, nullptr)) {
    return true;  // Let setup.exe find out.
  }
  return available.QuadPart >= bytes;
}

bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elevation = {};
  DWORD size = 0;
  const bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                            sizeof(elevation), &size) &&
                        elevation.TokenIsElevated;
  CloseHandle(token);
  return elevated;
}

InstallResult RunInstaller(const InstallOptions& options) {
  InstallResult result;
  const std::wstring temp = TempDir();
  const std::wstring dir = temp + L"starry-setup-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64());
  if (temp.empty() || !CreateDirectoryW(dir.c_str(), nullptr)) {
    result.error = GetLastError();
    return result;
  }
  const std::wstring exe = dir + L"\\mini_installer.exe";
  const std::wstring prefs = dir + L"\\initial_preferences.json";

  // The UI launches the browser on completion.
  std::wstring args = L"--do-not-launch-chrome";
  if (options.system_level) {
    args += L" --system-level";
  }
  bool ok = ExtractPayload(exe);
  if (ok && !options.desktop_shortcut) {
    constexpr std::string_view kPrefs =
        "{\"distribution\":{\"do_not_create_desktop_shortcut\":true}}";
    ok = WriteBytes(prefs, kPrefs.data(), static_cast<DWORD>(kPrefs.size()));
    args += L" --installerdata=" + Quote(prefs);
  }

  HANDLE process = nullptr;
  if (!ok) {
    result.error = GetLastError();
  } else if (options.system_level && !IsElevated()) {
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask =
        SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = args.c_str();
    info.lpDirectory = dir.c_str();
    info.nShow = SW_HIDE;
    if (ShellExecuteExW(&info) && info.hProcess) {
      process = info.hProcess;
    } else {
      result.error = GetLastError();
      result.cancelled = result.error == ERROR_CANCELLED;
    }
  } else {
    std::wstring command = Quote(exe) + L" " + args;
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, dir.c_str(), &startup, &child)) {
      CloseHandle(child.hThread);
      process = child.hProcess;
    } else {
      result.error = GetLastError();
    }
  }

  if (process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    result.started = true;
    result.exit_code = code;
  }
  DeleteFileW(prefs.c_str());
  DeleteFileW(exe.c_str());
  RemoveDirectoryW(dir.c_str());
  return result;
}

void LaunchBrowser(const std::wstring& exe) {
  if (exe.empty()) {
    return;
  }
  if (IsElevated()) {
    // Explorer starts it with the user's normal rights.
    ShellExecuteW(nullptr, L"open", L"explorer.exe", Quote(exe).c_str(),
                  nullptr, SW_SHOWNORMAL);
    return;
  }
  const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
  std::wstring command = Quote(exe);
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child = {};
  if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, dir.c_str(), &startup, &child)) {
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
  }
}

void OpenInstallerLog() {
  const std::wstring temp = TempDir();
  const std::wstring log = temp + STARRY_WINDOWS_INSTALLER_LOG;
  const std::wstring& target = FileExists(log) ? log : temp;
  ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

}  // namespace starry::installer
