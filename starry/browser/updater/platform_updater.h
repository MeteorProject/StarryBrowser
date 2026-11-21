// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UPDATER_PLATFORM_UPDATER_H_
#define STARRY_BROWSER_UPDATER_PLATFORM_UPDATER_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

namespace starry::updater {

enum class InstallKind {
  // macOS：位于可写目录的 .app，支持下载 zip、验签后退出替换并重启。
  kMacApp,
  // macOS：位于只读卷（如 DMG）或 App Translocation，需要用户移入「应用程序」。
  kMacReadOnly,
  // Windows：通过 setup.exe / mini_installer 安装的版本。
  kWinInstaller,
  // Windows：从 portable.zip 解压运行的便携版。
  kWinPortable,
  // Linux：Debian/Ubuntu .deb 包。
  kLinuxDeb,
  // Linux：Fedora/RHEL .rpm 包。
  kLinuxRpm,
  // 无法自动安装的运行方式（引导前往 Release 页面手动更新）。
  kManual,
};

struct InstallEnvironment {
  InstallEnvironment();
  InstallEnvironment(const InstallEnvironment&);
  InstallEnvironment& operator=(const InstallEnvironment&);
  ~InstallEnvironment();

  InstallKind kind = InstallKind::kManual;
  // 与 scripts/package.sh 一致："arm64" / "x64"。
  std::string arch;
  // true 表示可在应用内自动下载、校验并一键更新（重启更新或拉起包管理器）。
  bool can_auto_install = false;
  // true 表示用户普通退出浏览器时可静默替换（无 UAC / root 弹窗）。
  bool can_silent_apply_on_quit = false;
  // Windows：是否为系统级安装（HKLM / Program Files）。
  bool system_level = false;
  // macOS：当前 .app 路径；Windows：可执行文件所在目录。
  base::FilePath target_path;
  // Windows：主程序可执行文件路径；macOS：当前 Bundle Identifier。
  base::FilePath exe_path;
  std::string bundle_id;
};

enum class StageError {
  kNone,
  kChecksum,
  kUnpack,
  kInvalidPackage,
  kUnsupported,
};

struct StageResult {
  StageResult();
  StageResult(const StageResult&);
  StageResult& operator=(const StageResult&);
  ~StageResult();

  bool ok = false;
  // 供退出时直接替换或安装的路径（macOS 为解压验签后的 .app，Windows 安装版为
  // setup.exe，便携版为解压目录，Linux 为 .deb/.rpm）。
  base::FilePath staged_path;
  // 若校验失败，对应的错误类型与日志详情。
  StageError error = StageError::kNone;
  std::string error_detail;
};

// 必须在主线程调用一次（macOS 的 OuterBundlePath 要求主线程）。
InstallEnvironment DetectInstallEnvironment();

// 对应 InstallKind 的字符串标识（用于 WebUI 和持久化）。
const char* InstallKindName(InstallKind kind);

// 当前架构字符串（"arm64" / "x64"）。
std::string CurrentArch();

// 去空格后的产品名，与 scripts/package.sh 一致（例如 "StarryBrowser"）。
std::string PackageProductName();

// 按当前平台与安装方式计算期望的 GitHub Release 资产文件名。
// 如 "StarryBrowser-1.1.0-mac-arm64.zip"、"StarryBrowser-1.1.0-win-x64-setup.exe"。
std::string ExpectedAssetName(const InstallEnvironment& env,
                              std::string_view version);

// 阻塞计算文件的 64 位小写 SHA-256（在后台文件线程调用）。
std::optional<std::string> ComputeFileSha256(const base::FilePath& path);

// 在后台文件线程对已下载的更新包执行 SHA-256 校验与平台解包。
StageResult StageDownloadedUpdate(const InstallEnvironment& env,
                                  const base::FilePath& downloaded_file,
                                  const base::FilePath& staging_dir,
                                  std::string_view expected_version,
                                  std::string_view expected_sha256);

// 校验此前已经准备好的更新（例如上次启动前下载完尚未应用）是否依然完整有效。
bool ValidateExistingStagedUpdate(const InstallEnvironment& env,
                                  const base::FilePath& staged_path,
                                  std::string_view expected_version);

// 在进程即将退出时启动独立的更新助手，等待当前进程结束后替换并按需重启。
bool LaunchApplyOnExit(const InstallEnvironment& env,
                       const base::FilePath& staged_path,
                       const base::FilePath& staging_dir,
                       bool relaunch);

// 启动时清理上一次更新可能残留的备份目录。
void CleanupStaleBackups(const InstallEnvironment& env);

}  // namespace starry::updater

#endif  // STARRY_BROWSER_UPDATER_PLATFORM_UPDATER_H_
