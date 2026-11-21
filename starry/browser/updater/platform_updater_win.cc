// Copyright 2026 The Starry Browser Authors

#include "starry/browser/updater/platform_updater.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/registry.h"
#include "chrome/browser/first_run/upgrade_util.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "starry/common/branding.h"
#include "starry/common/platform_branding.h"
#include "starry/common/version.h"
#include "third_party/zlib/google/zip.h"

namespace starry::updater {

namespace {

#include "starry/browser/updater/apply_portable_update.inc"

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
    L"\\" STARRY_PRODUCT_FULLNAME_W;

bool PathsEqualNoTrailingSlash(const base::FilePath& a,
                               const base::FilePath& b) {
  return base::FilePath::CompareEqualIgnoreCase(
      a.StripTrailingSeparators().value(), b.StripTrailingSeparators().value());
}

// 转义 PowerShell 单引号字符串里的单引号。
std::string PsQuote(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('\'');
  for (char ch : text) {
    if (ch == '\'') {
      out.append("''");
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

bool HasPeHeader(const base::FilePath& path) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return false;
  }
  std::array<uint8_t, 2> magic{};
  return file.Read(0, magic) == magic.size() && magic[0] == 'M' &&
         magic[1] == 'Z';
}

base::FilePath FindPortableRoot(const base::FilePath& unpacked_dir) {
  const std::wstring exe_name =
      base::UTF8ToWide(base::StrCat({starry::kProductId, ".exe"}));
  const base::FilePath nested =
      unpacked_dir.AppendASCII(PackageProductName());
  if (base::PathExists(nested.Append(exe_name))) {
    return nested;
  }
  if (base::PathExists(unpacked_dir.Append(exe_name))) {
    return unpacked_dir;
  }
  return base::FilePath();
}

bool CanPublishPortableRuntime(const InstallEnvironment& env,
                               const base::FilePath& staged_root) {
  // Prereleases with the same numeric Windows version share the live runtime
  // directory. Switching them requires a filesystem that supports transactions.
  if (!base::DirectoryExists(staged_root.AppendASCII(starry::kWindowsVersion))) {
    return true;
  }
  base::File exe(env.exe_path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  DWORD flags = 0;
  return exe.IsValid() &&
         ::GetVolumeInformationByHandleW(exe.GetPlatformFile(), nullptr, 0,
                                         nullptr, nullptr, &flags, nullptr, 0) &&
         (flags & FILE_SUPPORTS_TRANSACTIONS) != 0;
}

}  // namespace

InstallEnvironment DetectInstallEnvironment() {
  InstallEnvironment env;
  env.arch = CurrentArch();

  base::FilePath exe_path;
  if (!base::PathService::Get(base::FILE_EXE, &exe_path)) {
    env.kind = InstallKind::kManual;
    return env;
  }
  env.exe_path = exe_path;
  env.target_path = exe_path.DirName();

  // 检查当前运行目录是否与注册表中安装版路径一致（同时兼容用户级和系统级）。
  for (const bool system_level : {false, true}) {
    const HKEY root = system_level ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    base::win::RegKey key;
    if (key.Open(root, kUninstallKey, KEY_READ | KEY_WOW64_32KEY) !=
        ERROR_SUCCESS) {
      continue;
    }
    std::wstring location;
    if (key.ReadValue(L"InstallLocation", &location) != ERROR_SUCCESS ||
        location.empty()) {
      continue;
    }
    if (PathsEqualNoTrailingSlash(env.target_path, base::FilePath(location))) {
      env.kind = InstallKind::kWinInstaller;
      env.system_level = system_level;
      env.can_auto_install = true;
      // 系统级安装需要 UAC 提权，不在用户普通退出时静默拉起，仅在用户主动点「重启并更新」时执行。
      env.can_silent_apply_on_quit = !system_level;
      return env;
    }
  }

  // 非安装版：若当前程序目录可写，按便携版（portable.zip）处理。
  if (base::PathIsWritable(env.target_path)) {
    env.kind = InstallKind::kWinPortable;
    env.can_auto_install = true;
    env.can_silent_apply_on_quit = true;
    return env;
  }

  env.kind = InstallKind::kManual;
  return env;
}

StageResult StageDownloadedUpdate(const InstallEnvironment& env,
                                  const base::FilePath& downloaded_file,
                                  const base::FilePath& staging_dir,
                                  std::string_view expected_version,
                                  std::string_view expected_sha256) {
  StageResult result;

  const std::optional<std::string> actual_sha =
      ComputeFileSha256(downloaded_file);
  if (!actual_sha || *actual_sha != expected_sha256) {
    result.error = StageError::kChecksum;
    result.error_detail =
        base::StrCat({"sha256 mismatch: expected=", expected_sha256,
                      " actual=", actual_sha.value_or("<read-error>")});
    return result;
  }

  if (env.kind == InstallKind::kWinInstaller) {
    if (!HasPeHeader(downloaded_file)) {
      result.error = StageError::kInvalidPackage;
      result.error_detail = "installer does not have a valid PE header";
      return result;
    }
    const base::FilePath setup_exe =
        staging_dir.Append(FILE_PATH_LITERAL("starry_setup.exe"));
    base::DeleteFile(setup_exe);
    if (!base::Move(downloaded_file, setup_exe)) {
      result.error = StageError::kUnpack;
      result.error_detail = "failed to move setup executable into staging";
      return result;
    }
    result.ok = true;
    result.staged_path = setup_exe;
    return result;
  }

  if (env.kind == InstallKind::kWinPortable) {
    const base::FilePath unpacked_dir =
        staging_dir.Append(FILE_PATH_LITERAL("unpacked"));
    base::DeletePathRecursively(unpacked_dir);
    if (!base::CreateDirectory(unpacked_dir) ||
        !zip::Unzip(downloaded_file, unpacked_dir)) {
      base::DeletePathRecursively(unpacked_dir);
      result.error = StageError::kUnpack;
      result.error_detail = "failed to extract portable archive";
      return result;
    }
    const base::FilePath root = FindPortableRoot(unpacked_dir);
    if (root.empty()) {
      base::DeletePathRecursively(unpacked_dir);
      result.error = StageError::kInvalidPackage;
      result.error_detail = "browser executable not found in portable archive";
      return result;
    }
    if (!CanPublishPortableRuntime(env, root)) {
      base::DeletePathRecursively(unpacked_dir);
      result.error = StageError::kUnsupported;
      result.error_detail = "cannot transactionally replace the active runtime";
      return result;
    }
    base::DeleteFile(downloaded_file);
    result.ok = true;
    result.staged_path = root;
    return result;
  }

  result.error = StageError::kUnsupported;
  return result;
}

bool ValidateExistingStagedUpdate(const InstallEnvironment& env,
                                  const base::FilePath& staged_path,
                                  std::string_view expected_version) {
  if (staged_path.empty()) {
    return false;
  }
  if (env.kind == InstallKind::kWinInstaller) {
    return HasPeHeader(staged_path);
  }
  if (env.kind == InstallKind::kWinPortable) {
    const std::wstring exe_name =
        base::UTF8ToWide(base::StrCat({starry::kProductId, ".exe"}));
    return base::PathExists(staged_path.Append(exe_name)) &&
           CanPublishPortableRuntime(env, staged_path);
  }
  return false;
}

bool LaunchApplyOnExit(const InstallEnvironment& env,
                       const base::FilePath& staged_path,
                       const base::FilePath& staging_dir,
                       bool relaunch) {
  if ((env.kind != InstallKind::kWinInstaller &&
       env.kind != InstallKind::kWinPortable) ||
      staged_path.empty() || env.exe_path.empty()) {
    return false;
  }

  base::FilePath temp_dir;
  if (!base::GetTempDir(&temp_dir)) {
    return false;
  }
  const base::FilePath script_path = temp_dir.Append(base::UTF8ToWide(
      base::StrCat({"starry-update-",
                    base::NumberToString(base::GetCurrentProcId()), ".ps1"})));

  std::vector<std::string> relaunch_args;
  if (relaunch && base::CommandLine::InitializedForCurrentProcess()) {
    const base::CommandLine relaunch_cl = upgrade_util::GetRelaunchCommandLine(
        *base::CommandLine::ForCurrentProcess(),
        browser_shutdown::RestartMode::kRestartLastSession);
    const base::CommandLine::StringVector& argv = relaunch_cl.argv();
    for (size_t i = 1; i < argv.size(); ++i) {
      relaunch_args.push_back(base::WideToUTF8(argv[i]));
    }
  }

  std::string ps_relaunch_array = "@(";
  for (size_t i = 0; i < relaunch_args.size(); ++i) {
    if (i > 0) {
      ps_relaunch_array.append(", ");
    }
    ps_relaunch_array.append(PsQuote(relaunch_args[i]));
  }
  ps_relaunch_array.push_back(')');

  // UTF-8 BOM 确保 Windows PowerShell 5.1 正确读取含非 ASCII 字符的路径。
  std::string script = "\xEF\xBB\xBF";
  base::StrAppend(
      &script,
      {"$ErrorActionPreference = 'SilentlyContinue'\n",
       "$parentPid = ", base::NumberToString(base::GetCurrentProcId()), "\n",
       "$staged = ", PsQuote(staged_path.AsUTF8Unsafe()), "\n",
       "$stagingDir = ", PsQuote(staging_dir.AsUTF8Unsafe()), "\n",
       "$targetDir = ", PsQuote(env.target_path.AsUTF8Unsafe()), "\n",
       "$targetExe = ", PsQuote(env.exe_path.AsUTF8Unsafe()), "\n",
       "$relaunch = ", relaunch ? "$true" : "$false", "\n",
       "$relaunchArgs = ", ps_relaunch_array, "\n",
       "try { Wait-Process -Id $parentPid -Timeout 30 } catch {}\n",
       "Start-Sleep -Milliseconds 400\n"});

  if (env.kind == InstallKind::kWinInstaller) {
    const std::string install_args =
        env.system_level
            ? "@('--silent', '--system-level', '--no-desktop-shortcut')"
            : "@('--silent', '--no-desktop-shortcut')";
    base::StrAppend(
        &script,
        {"$installArgs = ", install_args, "\n",
         "$proc = Start-Process -FilePath $staged -ArgumentList $installArgs "
         "-Wait -PassThru -WindowStyle Hidden\n",
         "if ($proc -and $proc.ExitCode -eq 0) {\n",
         "  Remove-Item -LiteralPath $stagingDir -Recurse -Force\n",
         "}\n"});
  } else {
    script.append(kApplyPortableUpdate);
  }

  base::StrAppend(
      &script,
      {"if ($relaunch -and (Test-Path -LiteralPath $targetExe)) {\n",
       "  if ($relaunchArgs.Count -gt 0) {\n",
       "    Start-Process -FilePath $targetExe -ArgumentList $relaunchArgs\n",
       "  } else {\n",
       "    Start-Process -FilePath $targetExe\n",
       "  }\n",
       "}\n",
       "Remove-Item -LiteralPath $MyInvocation.MyCommand.Path -Force\n"});

  if (!base::WriteFile(script_path, script)) {
    LOG(ERROR) << "[starry] updater failed to write Windows helper script";
    return false;
  }

  base::CommandLine cmd(base::FilePath(FILE_PATH_LITERAL("powershell.exe")));
  cmd.AppendArg("-NoProfile");
  cmd.AppendArg("-NonInteractive");
  cmd.AppendArg("-ExecutionPolicy");
  cmd.AppendArg("Bypass");
  cmd.AppendArg("-WindowStyle");
  cmd.AppendArg("Hidden");
  cmd.AppendArg("-File");
  cmd.AppendArgPath(script_path);

  base::LaunchOptions options;
  options.start_hidden = true;
  options.force_breakaway_from_job_ = true;
  base::Process proc = base::LaunchProcess(cmd, options);
  if (!proc.IsValid()) {
    LOG(ERROR) << "[starry] updater failed to launch Windows helper";
    base::DeleteFile(script_path);
    return false;
  }
  return true;
}

void CleanupStaleBackups(const InstallEnvironment& env) {}

}  // namespace starry::updater
