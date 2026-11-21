// Copyright 2026 The Starry Browser Authors

#include "starry/browser/updater/platform_updater.h"

#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "chrome/browser/first_run/upgrade_util.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "starry/common/branding.h"

namespace starry::updater {

namespace {

struct BundlePlistInfo {
  std::string bundle_id;
  std::string short_version;
};

std::optional<BundlePlistInfo> ReadBundlePlist(const base::FilePath& app_path) {
  const base::FilePath plist_path =
      app_path.Append("Contents").Append("Info.plist");
  std::string bytes;
  if (!base::ReadFileToString(plist_path, &bytes) || bytes.empty()) {
    return std::nullopt;
  }
  base::apple::ScopedCFTypeRef<CFDataRef> data(CFDataCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(bytes.data()),
      static_cast<CFIndex>(bytes.size())));
  if (!data) {
    return std::nullopt;
  }
  base::apple::ScopedCFTypeRef<CFPropertyListRef> plist(
      CFPropertyListCreateWithData(kCFAllocatorDefault, data.get(),
                                   kCFPropertyListImmutable, nullptr, nullptr));
  CFDictionaryRef dict = base::apple::CFCast<CFDictionaryRef>(plist.get());
  if (!dict) {
    return std::nullopt;
  }
  CFStringRef id_ref = base::apple::GetValueFromDictionary<CFStringRef>(
      dict, CFSTR("CFBundleIdentifier"));
  CFStringRef ver_ref = base::apple::GetValueFromDictionary<CFStringRef>(
      dict, CFSTR("CFBundleShortVersionString"));
  if (!id_ref || !ver_ref) {
    return std::nullopt;
  }
  BundlePlistInfo info;
  info.bundle_id = base::SysCFStringRefToUTF8(id_ref);
  info.short_version = base::SysCFStringRefToUTF8(ver_ref);
  return info;
}

base::FilePath FindExtractedApp(const base::FilePath& unpacked_dir) {
  const base::FilePath expected =
      unpacked_dir.Append(base::StrCat({starry::kProductName, ".app"}));
  if (base::DirectoryExists(expected)) {
    return expected;
  }
  base::FileEnumerator enumerator(unpacked_dir, /*recursive=*/false,
                                  base::FileEnumerator::DIRECTORIES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    if (path.MatchesExtension(".app")) {
      return path;
    }
  }
  return base::FilePath();
}

}  // namespace

InstallEnvironment DetectInstallEnvironment() {
  InstallEnvironment env;
  env.arch = CurrentArch();

  const base::FilePath bundle = base::apple::OuterBundlePath();
  env.target_path = bundle;
  env.bundle_id = base::apple::MainBundleIdentifier();

  if (bundle.empty() || !bundle.MatchesExtension(".app") ||
      !base::DirectoryExists(bundle)) {
    env.kind = InstallKind::kManual;
    return env;
  }

  // Gatekeeper App Translocation 或只读磁盘镜像（DMG）上的 App 不能就地替换。
  const bool translocated =
      bundle.value().find("/AppTranslocation/") != std::string::npos;
  const bool writable =
      !translocated && access(bundle.DirName().value().c_str(), W_OK) == 0 &&
      access(bundle.value().c_str(), W_OK) == 0;

  if (!writable) {
    env.kind = InstallKind::kMacReadOnly;
    env.can_auto_install = false;
    env.can_silent_apply_on_quit = false;
    return env;
  }

  env.kind = InstallKind::kMacApp;
  env.can_auto_install = true;
  env.can_silent_apply_on_quit = true;
  return env;
}

StageResult StageDownloadedUpdate(const InstallEnvironment& env,
                                  const base::FilePath& downloaded_file,
                                  const base::FilePath& staging_dir,
                                  std::string_view expected_version,
                                  std::string_view expected_sha256) {
  StageResult result;

  // 1) SHA-256 完整性校验。
  const std::optional<std::string> actual_sha =
      ComputeFileSha256(downloaded_file);
  if (!actual_sha || *actual_sha != expected_sha256) {
    result.error = StageError::kChecksum;
    result.error_detail =
        base::StrCat({"sha256 mismatch: expected=", expected_sha256,
                      " actual=", actual_sha.value_or("<read-error>")});
    return result;
  }

  // 2) 使用 macOS 原生 ditto 解压，完整保留符号链接、权限与代码签名。
  const base::FilePath unpacked_dir = staging_dir.Append("unpacked");
  base::DeletePathRecursively(unpacked_dir);
  if (!base::CreateDirectory(unpacked_dir)) {
    result.error = StageError::kUnpack;
    result.error_detail = "failed to create unpacked directory";
    return result;
  }

  base::CommandLine ditto(base::FilePath("/usr/bin/ditto"));
  ditto.AppendArg("-x");
  ditto.AppendArg("-k");
  ditto.AppendArgPath(downloaded_file);
  ditto.AppendArgPath(unpacked_dir);
  std::string ditto_output;
  if (!base::GetAppOutputAndError(ditto, &ditto_output)) {
    base::DeletePathRecursively(unpacked_dir);
    result.error = StageError::kUnpack;
    result.error_detail = ditto_output;
    return result;
  }

  const base::FilePath staged_app = FindExtractedApp(unpacked_dir);
  if (staged_app.empty()) {
    base::DeletePathRecursively(unpacked_dir);
    result.error = StageError::kInvalidPackage;
    result.error_detail = "no .app bundle inside archive";
    return result;
  }

  // 3) 校验 Info.plist 的 Bundle ID 与版本号。
  const std::optional<BundlePlistInfo> plist = ReadBundlePlist(staged_app);
  if (!plist || (!env.bundle_id.empty() && plist->bundle_id != env.bundle_id) ||
      plist->short_version != expected_version) {
    base::DeletePathRecursively(unpacked_dir);
    result.error = StageError::kInvalidPackage;
    result.error_detail = "Info.plist identifier or version mismatch";
    return result;
  }

  // 解压并校验通过后删除原始压缩包，节省磁盘空间。
  base::DeleteFile(downloaded_file);

  result.ok = true;
  result.staged_path = staged_app;
  return result;
}

bool ValidateExistingStagedUpdate(const InstallEnvironment& env,
                                  const base::FilePath& staged_path,
                                  std::string_view expected_version) {
  if (staged_path.empty() || !base::DirectoryExists(staged_path)) {
    return false;
  }
  const std::optional<BundlePlistInfo> plist = ReadBundlePlist(staged_path);
  return plist &&
         (env.bundle_id.empty() || plist->bundle_id == env.bundle_id) &&
         plist->short_version == expected_version;
}

bool LaunchApplyOnExit(const InstallEnvironment& env,
                       const base::FilePath& staged_path,
                       const base::FilePath& staging_dir,
                       bool relaunch) {
  if (env.kind != InstallKind::kMacApp || env.target_path.empty() ||
      staged_path.empty()) {
    return false;
  }

  // 独立 shell 进程：等待当前浏览器 PID 完全退出 → 原子替换 .app（失败时回滚）→
  // 清理暂存目录 → 按需重新打开新版本并恢复会话。
  static constexpr char kSwapScript[] = R"(
pid="$1"
staged="$2"
target="$3"
staging_dir="$4"
relaunch="$5"
shift 5

i=0
while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 300 ]; do
  sleep 0.1
  i=$((i + 1))
done
if kill -0 "$pid" 2>/dev/null; then
  exit 1
fi

backup="${target}.starry-old-$$"
rm -rf "$backup"
if mv "$target" "$backup"; then
  if mv "$staged" "$target" 2>/dev/null || /usr/bin/ditto "$staged" "$target"; then
    /usr/bin/xattr -dr com.apple.quarantine "$target" 2>/dev/null || true
    rm -rf "$backup" "$staging_dir"
  else
    rm -rf "$target"
    mv "$backup" "$target"
    exit 1
  fi
else
  exit 1
fi

if [ "$relaunch" = "1" ]; then
  if [ "$#" -gt 0 ]; then
    /usr/bin/open -n -a "$target" --args "$@"
  else
    /usr/bin/open -n -a "$target"
  fi
fi
)";

  base::CommandLine cmd(base::FilePath("/bin/sh"));
  cmd.AppendArg("-c");
  cmd.AppendArg(kSwapScript);
  cmd.AppendArg("starry-updater");
  cmd.AppendArg(base::NumberToString(base::GetCurrentProcId()));
  cmd.AppendArgPath(staged_path);
  cmd.AppendArgPath(env.target_path);
  cmd.AppendArgPath(staging_dir);
  cmd.AppendArg(relaunch ? "1" : "0");

  if (relaunch && base::CommandLine::InitializedForCurrentProcess()) {
    const base::CommandLine relaunch_cl = upgrade_util::GetRelaunchCommandLine(
        *base::CommandLine::ForCurrentProcess(),
        browser_shutdown::RestartMode::kRestartLastSession);
    const std::vector<std::string>& argv = relaunch_cl.argv();
    for (size_t i = 1; i < argv.size(); ++i) {
      cmd.AppendArg(argv[i]);
    }
  }

  base::LaunchOptions options;
  options.new_process_group = true;
  base::Process proc = base::LaunchProcess(cmd, options);
  if (!proc.IsValid()) {
    LOG(ERROR) << "[starry] updater failed to launch macOS swap helper";
    return false;
  }
  return true;
}

void CleanupStaleBackups(const InstallEnvironment& env) {
  if (env.target_path.empty()) {
    return;
  }
  const std::string prefix =
      base::StrCat({env.target_path.BaseName().value(), ".starry-old-"});
  base::FileEnumerator enumerator(env.target_path.DirName(),
                                  /*recursive=*/false,
                                  base::FileEnumerator::DIRECTORIES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    if (path.BaseName().value().starts_with(prefix)) {
      base::DeletePathRecursively(path);
    }
  }
}

}  // namespace starry::updater
