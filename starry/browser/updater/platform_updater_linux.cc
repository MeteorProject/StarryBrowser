// Copyright 2026 The Starry Browser Authors

#include "starry/browser/updater/platform_updater.h"

#include <array>
#include <cstdint>
#include <string>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"

namespace starry::updater {

namespace {

bool HasDebMagic(const base::FilePath& path) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return false;
  }
  std::array<uint8_t, 8> magic{};
  static constexpr std::array<uint8_t, 8> kExpected = {'!', '<', 'a', 'r',
                                                       'c', 'h', '>', '\n'};
  return file.Read(0, magic) == magic.size() && magic == kExpected;
}

bool HasRpmMagic(const base::FilePath& path) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return false;
  }
  std::array<uint8_t, 4> magic{};
  static constexpr std::array<uint8_t, 4> kExpected = {0xED, 0xAB, 0xEE, 0xDB};
  return file.Read(0, magic) == magic.size() && magic == kExpected;
}

}  // namespace

InstallEnvironment DetectInstallEnvironment() {
  InstallEnvironment env;
  env.arch = CurrentArch();

  // 按系统包管理器判断优先下载 .deb 还是 .rpm。
  if (base::PathExists(base::FilePath("/etc/debian_version")) ||
      base::PathExists(base::FilePath("/usr/bin/dpkg"))) {
    env.kind = InstallKind::kLinuxDeb;
    env.can_auto_install = true;
    env.can_silent_apply_on_quit = false;
    return env;
  }
  if (base::PathExists(base::FilePath("/usr/bin/rpm"))) {
    env.kind = InstallKind::kLinuxRpm;
    env.can_auto_install = true;
    env.can_silent_apply_on_quit = false;
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

  const bool valid_magic = env.kind == InstallKind::kLinuxDeb
                               ? HasDebMagic(downloaded_file)
                               : HasRpmMagic(downloaded_file);
  if (!valid_magic) {
    result.error = StageError::kInvalidPackage;
    result.error_detail = "downloaded Linux package has invalid magic bytes";
    return result;
  }

  const std::string asset_name = ExpectedAssetName(env, expected_version);
  const base::FilePath target = staging_dir.AppendASCII(asset_name);
  base::DeleteFile(target);
  if (!base::Move(downloaded_file, target)) {
    result.error = StageError::kUnpack;
    result.error_detail = "failed to move package into staging directory";
    return result;
  }

  result.ok = true;
  result.staged_path = target;
  return result;
}

bool ValidateExistingStagedUpdate(const InstallEnvironment& env,
                                  const base::FilePath& staged_path,
                                  std::string_view expected_version) {
  if (staged_path.empty() || !base::PathExists(staged_path)) {
    return false;
  }
  return env.kind == InstallKind::kLinuxDeb ? HasDebMagic(staged_path)
                                            : HasRpmMagic(staged_path);
}

bool LaunchApplyOnExit(const InstallEnvironment& env,
                       const base::FilePath& staged_path,
                       const base::FilePath& staging_dir,
                       bool relaunch) {
  // Linux 通过桌面软件中心直接打开 .deb / .rpm，不在进程退出时替换文件。
  return false;
}

void CleanupStaleBackups(const InstallEnvironment& env) {}

}  // namespace starry::updater
