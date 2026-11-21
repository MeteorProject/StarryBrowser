// Copyright 2026 The Starry Browser Authors

#include "starry/browser/updater/updater_service.h"

#include <algorithm>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/application_lifetime_desktop.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/common/version.h"

#if BUILDFLAG(ENABLE_SESSION_SERVICE)
#include "chrome/browser/sessions/session_data_service.h"
#include "chrome/browser/sessions/session_data_service_factory.h"
#endif

namespace starry::updater {

namespace {

constexpr base::FilePath::CharType kDirectoryName[] =
    FILE_PATH_LITERAL("Starry Updates");
constexpr base::FilePath::CharType kStateFile[] =
    FILE_PATH_LITERAL("State.json");
constexpr base::FilePath::CharType kStagingDirName[] =
    FILE_PATH_LITERAL("staging");
constexpr char kDisableAutoUpdateSwitch[] = "disable-auto-update";

constexpr base::TimeDelta kStartupCheckDelay = base::Seconds(45);
constexpr base::TimeDelta kCheckInterval = base::Hours(6);
constexpr base::TimeDelta kMetadataTimeout = base::Seconds(10);
constexpr base::TimeDelta kCrossCheckTimeout = base::Seconds(8);
constexpr base::TimeDelta kWatchdogInterval = base::Seconds(3);
constexpr base::TimeDelta kDownloadStallTimeout = base::Seconds(15);
constexpr base::TimeDelta kDownloadSpeedWindow = base::Seconds(15);
// GFW 限速判定阈值：若持续 15 秒平均速率低于 32 KB/s 且还有备用镜像，自动切走。
constexpr int64_t kMinAcceptableBytesPerSec = 32 * 1024;
constexpr size_t kMaxMetadataBytes = 2 * 1024 * 1024;
constexpr size_t kMaxSha256Bytes = 64 * 1024;
constexpr int64_t kMaxPackageBytes = 1024LL * 1024 * 1024;  // 1 GB 上限
constexpr base::TimeDelta kProgressNotifyInterval = base::Milliseconds(150);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("starry_browser_updater", R"(
        semantics {
          sender: "Starry Browser updater"
          description:
            "Checks GitHub Releases (and fallback release mirrors when direct "
            "access is blocked) for newer Starry Browser versions, verifies "
            "package checksums, and downloads update packages."
          trigger:
            "Automatic periodic check on startup and every 6 hours when "
            "enabled, or manually triggered from Settings."
          data: "None. Optional conditional request ETag header."
          destination: OTHER
          destination_other: "GitHub Releases or configured release mirror."
        }
        policy {
          cookies_allowed: NO
          setting: "Settings > General > Software Update."
          policy_exception_justification: "Not implemented."
        })");

network::mojom::URLLoaderFactory* SystemLoaderFactory() {
  if (!g_browser_process ||
      !g_browser_process->system_network_context_manager()) {
    return nullptr;
  }
  return g_browser_process->system_network_context_manager()
      ->GetURLLoaderFactory();
}

std::string UserAgentHeader() {
  return base::StrCat({"StarryBrowser/", starry::kVersion});
}

int ResponseCodeOf(const network::SimpleURLLoader* loader) {
  const network::mojom::URLResponseHead* head =
      loader ? loader->ResponseInfo() : nullptr;
  return head && head->headers ? head->headers->response_code() : 0;
}

std::string GetString(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

double TimeToSeconds(base::Time time) {
  return time.is_null() ? 0 : time.InSecondsFSinceUnixEpoch();
}

base::Time TimeFromSeconds(std::optional<double> seconds) {
  return seconds && *seconds > 0
             ? base::Time::FromSecondsSinceUnixEpoch(*seconds)
             : base::Time();
}

bool WriteTextAtomically(const base::FilePath& path, const std::string& text) {
  if (!base::CreateDirectory(path.DirName())) {
    return false;
  }
  return base::ImportantFileWriter::WriteFileAtomically(path, text);
}

Profile* AnyRegularProfile() {
  if (!g_browser_process || !g_browser_process->profile_manager()) {
    return nullptr;
  }
  for (Profile* profile :
       g_browser_process->profile_manager()->GetLoadedProfiles()) {
    if (!profile->IsOffTheRecord()) {
      return profile;
    }
  }
  return nullptr;
}

}  // namespace

struct UpdaterService::LoadedState {
  std::optional<base::Value> json;
  bool staged_valid = false;
  std::string staged_version;
  base::FilePath staged_path;
  base::FilePath staged_dir;
};

struct UpdaterService::PendingCandidate {
  std::string tag_name;
  SemVer version;
  std::string release_notes;
  std::string published_at;
  GURL release_url;
  std::string asset_name;
  int64_t asset_size = 0;
  std::string sha256;
  // true 表示校验和来自 GitHub 官方直连或用户自定义镜像，无需第三方交叉校验。
  bool sha256_trusted_source = false;
};

UpdaterService::StateSnapshot::StateSnapshot() = default;
UpdaterService::StateSnapshot::StateSnapshot(const StateSnapshot&) = default;
UpdaterService::StateSnapshot& UpdaterService::StateSnapshot::operator=(
    const StateSnapshot&) = default;
UpdaterService::StateSnapshot::~StateSnapshot() = default;

// static
UpdaterService* UpdaterService::Get() {
  static base::NoDestructor<UpdaterService> instance;
  return instance.get();
}

// static
const char* UpdaterService::StatusName(UpdateStatus status) {
  switch (status) {
    case UpdateStatus::kIdle:
      return "idle";
    case UpdateStatus::kChecking:
      return "checking";
    case UpdateStatus::kUpToDate:
      return "upToDate";
    case UpdateStatus::kAvailable:
      return "available";
    case UpdateStatus::kDownloading:
      return "downloading";
    case UpdateStatus::kVerifying:
      return "verifying";
    case UpdateStatus::kReady:
      return "ready";
    case UpdateStatus::kError:
      return "error";
  }
}

// static
const char* UpdaterService::ModeName(AutoUpdateMode mode) {
  switch (mode) {
    case AutoUpdateMode::kAuto:
      return "auto";
    case AutoUpdateMode::kNotify:
      return "notify";
    case AutoUpdateMode::kOff:
      return "off";
  }
}

// static
std::optional<AutoUpdateMode> UpdaterService::ModeFromName(
    std::string_view name) {
  if (name == "auto") {
    return AutoUpdateMode::kAuto;
  }
  if (name == "notify") {
    return AutoUpdateMode::kNotify;
  }
  if (name == "off") {
    return AutoUpdateMode::kOff;
  }
  return std::nullopt;
}

UpdaterService::UpdaterService()
    : current_version_(ParseSemVer(starry::kVersion).value_or(SemVer())),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

UpdaterService::~UpdaterService() = default;

base::FilePath UpdaterService::StagingDirFor(std::string_view version) const {
  return directory_.Append(kStagingDirName).AppendASCII(version);
}

void UpdaterService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  env_ = DetectInstallEnvironment();
  auto_check_disabled_by_flag_ =
      base::CommandLine::InitializedForCurrentProcess() &&
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          kDisableAutoUpdateSwitch);

  base::FilePath user_data;
  if (base::PathService::Get(chrome::DIR_USER_DATA, &user_data)) {
    directory_ = user_data.Append(kDirectoryName);
  }

  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath directory, InstallEnvironment env,
             SemVer current_version) {
            CleanupStaleBackups(env);
            auto loaded = std::make_unique<LoadedState>();
            std::string raw;
            if (!directory.empty() &&
                base::ReadFileToString(directory.Append(kStateFile), &raw)) {
              loaded->json = base::JSONReader::Read(raw, base::JSON_PARSE_RFC);
            }

            // 清理版本 <= 当前版本的过时暂存目录（例如刚升级重启之后）。
            const base::FilePath staging_root =
                directory.Append(kStagingDirName);
            if (!directory.empty() && base::DirectoryExists(staging_root)) {
              base::FileEnumerator enumerator(
                  staging_root, /*recursive=*/false,
                  base::FileEnumerator::DIRECTORIES);
              for (base::FilePath sub = enumerator.Next(); !sub.empty();
                   sub = enumerator.Next()) {
                const std::optional<SemVer> dir_ver =
                    ParseSemVer(sub.BaseName().AsUTF8Unsafe());
                if (!dir_ver || !IsNewerVersion(*dir_ver, current_version)) {
                  base::DeletePathRecursively(sub);
                }
              }
            }

            if (loaded->json && loaded->json->is_dict()) {
              if (const base::DictValue* staged =
                      loaded->json->GetDict().FindDict("staged")) {
                const std::string ver_str = GetString(*staged, "version");
                const std::string path_str = GetString(*staged, "stagedPath");
                const std::string kind_str = GetString(*staged, "installKind");
                const std::optional<SemVer> staged_ver = ParseSemVer(ver_str);
                if (staged_ver &&
                    IsNewerVersion(*staged_ver, current_version) &&
                    kind_str == InstallKindName(env.kind) &&
                    !path_str.empty()) {
                  const base::FilePath staged_path =
                      base::FilePath::FromUTF8Unsafe(path_str);
                  if (ValidateExistingStagedUpdate(env, staged_path, ver_str)) {
                    loaded->staged_valid = true;
                    loaded->staged_version = ver_str;
                    loaded->staged_path = staged_path;
                    loaded->staged_dir = staging_root.AppendASCII(ver_str);
                  }
                }
              }
            }
            return loaded;
          },
          directory_, env_, current_version_),
      base::BindOnce(&UpdaterService::OnStateLoaded,
                     weak_factory_.GetWeakPtr()));
}

void UpdaterService::Shutdown() {
  startup_timer_.Stop();
  periodic_timer_.Stop();
  watchdog_timer_.Stop();
  loader_.reset();
  closing_subscription_ = {};
}

void UpdaterService::OnPostDestroyThreads() {
  if (staged_path_.empty() || status_ != UpdateStatus::kReady) {
    return;
  }

  bool should_apply = pending_apply_on_exit_;
  bool relaunch = relaunch_after_apply_;

  // 若更新已经校验就绪且当前平台支持无感知退出替换，用户正常退出浏览器时顺带完成更新。
  if (!should_apply && env_.can_silent_apply_on_quit) {
    const browser_shutdown::ShutdownType type =
        browser_shutdown::GetShutdownType();
    if (type == browser_shutdown::ShutdownType::kWindowClose ||
        type == browser_shutdown::ShutdownType::kBrowserExit) {
      should_apply = true;
      relaunch = false;
    }
  }

  if (should_apply) {
    LaunchApplyOnExit(env_, staged_path_, staged_dir_, relaunch);
  }
}

void UpdaterService::OnStateLoaded(std::unique_ptr<LoadedState> loaded) {
  loaded_ = true;
  if (loaded->json && loaded->json->is_dict()) {
    const base::DictValue& dict = loaded->json->GetDict();
    if (std::optional<AutoUpdateMode> mode =
            ModeFromName(GetString(dict, "mode"))) {
      mode_ = *mode;
    }
    if (std::optional<std::string> mirror =
            SanitizeMirrorInput(GetString(dict, "customMirror"))) {
      custom_mirror_ = std::move(*mirror);
    }
    last_working_route_ = GetString(dict, "lastWorkingRoute");
    etag_ = GetString(dict, "etag");
    last_seen_tag_ = GetString(dict, "lastSeenTag");
    last_checked_ = TimeFromSeconds(dict.FindDouble("lastChecked"));
    dismissed_version_ = GetString(dict, "dismissedVersion");

    if (loaded->staged_valid) {
      if (const base::DictValue* staged = dict.FindDict("staged")) {
        status_ = UpdateStatus::kReady;
        staged_version_ = loaded->staged_version;
        staged_path_ = loaded->staged_path;
        staged_dir_ = loaded->staged_dir;
        available_version_ = staged_version_;
        release_tag_ = GetString(*staged, "tag");
        release_url_ = GURL(GetString(*staged, "releaseUrl"));
        if (!release_url_.is_valid()) {
          release_url_ = ReleasePageUrl(release_tag_);
        }
        release_notes_ = GetString(*staged, "releaseNotes");
        published_at_ = GetString(*staged, "publishedAt");
        asset_name_ = GetString(*staged, "assetName");
        expected_sha256_ = GetString(*staged, "sha256");
      }
    }
  }

  ScheduleTimers();
  NotifyChanged();
}

void UpdaterService::SaveState() {
  if (!loaded_ || directory_.empty()) {
    return;
  }
  base::DictValue dict;
  dict.Set("mode", ModeName(mode_));
  dict.Set("customMirror", custom_mirror_);
  dict.Set("lastWorkingRoute", last_working_route_);
  dict.Set("etag", etag_);
  dict.Set("lastSeenTag", last_seen_tag_);
  dict.Set("lastChecked", TimeToSeconds(last_checked_));
  dict.Set("dismissedVersion", dismissed_version_);
  if (status_ == UpdateStatus::kReady && !staged_version_.empty() &&
      !staged_path_.empty()) {
    base::DictValue staged;
    staged.Set("version", staged_version_);
    staged.Set("tag", release_tag_);
    staged.Set("stagedPath", staged_path_.AsUTF8Unsafe());
    staged.Set("releaseUrl", release_url_.spec());
    staged.Set("releaseNotes", release_notes_);
    staged.Set("publishedAt", published_at_);
    staged.Set("assetName", asset_name_);
    staged.Set("sha256", expected_sha256_);
    staged.Set("installKind", InstallKindName(env_.kind));
    dict.Set("staged", std::move(staged));
  }

  std::string json;
  base::JSONWriter::WriteWithOptions(dict,
                                     base::JSONWriter::OPTIONS_PRETTY_PRINT,
                                     &json);
  file_runner_->PostTask(
      FROM_HERE, base::BindOnce(base::IgnoreResult(&WriteTextAtomically),
                                directory_.Append(kStateFile), std::move(json)));
}

void UpdaterService::NotifyChanged(bool throttled) {
  if (throttled) {
    const base::TimeTicks now = base::TimeTicks::Now();
    if (!last_progress_notify_.is_null() &&
        now - last_progress_notify_ < kProgressNotifyInterval) {
      return;
    }
    last_progress_notify_ = now;
  }
  for (Observer& observer : observers_) {
    observer.OnUpdateStateChanged();
  }
}

void UpdaterService::ScheduleTimers() {
  startup_timer_.Stop();
  periodic_timer_.Stop();
  if (auto_check_disabled_by_flag_ || mode_ == AutoUpdateMode::kOff) {
    return;
  }

  // 启动后延迟检查，避免拖慢冷启动；若距离上次检查已超间隔则检查一次。
  const base::Time now = base::Time::Now();
  const bool due = last_checked_.is_null() || now - last_checked_ >= kCheckInterval;
  if (due && status_ != UpdateStatus::kReady) {
    startup_timer_.Start(FROM_HERE, kStartupCheckDelay,
                         base::BindOnce(&UpdaterService::CheckNow,
                                        base::Unretained(this),
                                        /*user_initiated=*/false));
  }
  periodic_timer_.Start(FROM_HERE, kCheckInterval,
                        base::BindRepeating(&UpdaterService::OnPeriodicCheck,
                                            base::Unretained(this)));
}

void UpdaterService::OnPeriodicCheck() {
  if (mode_ == AutoUpdateMode::kOff) {
    return;
  }
  CheckNow(/*user_initiated=*/false);
}

UpdaterService::StateSnapshot UpdaterService::Snapshot() const {
  StateSnapshot s;
  s.status = status_;
  s.mode = mode_;
  s.install_kind = env_.kind;
  s.can_auto_install = env_.can_auto_install;
  s.current_version = starry::kVersion;
  s.available_version = available_version_;
  s.release_tag = release_tag_;
  s.release_url =
      release_url_.is_valid() ? release_url_ : ReleasePageUrl(release_tag_);
  s.release_notes = release_notes_;
  s.published_at = published_at_;
  s.asset_name = asset_name_;
  s.last_checked = last_checked_;
  s.custom_mirror = custom_mirror_;
  s.active_route_host = active_route_host_;
  s.bytes_received = bytes_received_;
  s.total_bytes = total_bytes_;
  s.error_message = error_message_;
  s.notification_dismissed =
      !available_version_.empty() && dismissed_version_ == available_version_;
  s.staged_path = staged_path_;
  return s;
}

bool UpdaterService::HasAttention() const {
  if (status_ == UpdateStatus::kReady) {
    return true;
  }
  if (status_ == UpdateStatus::kAvailable &&
      (mode_ == AutoUpdateMode::kNotify || !env_.can_auto_install)) {
    return true;
  }
  return false;
}

bool UpdaterService::ShouldShowToast() const {
  if (!HasAttention() || available_version_.empty()) {
    return false;
  }
  return dismissed_version_ != available_version_;
}

void UpdaterService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void UpdaterService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void UpdaterService::SetAutoUpdateMode(AutoUpdateMode mode) {
  if (mode_ == mode) {
    return;
  }
  mode_ = mode;
  SaveState();
  ScheduleTimers();
  if (mode_ == AutoUpdateMode::kAuto && status_ == UpdateStatus::kAvailable &&
      env_.can_auto_install) {
    DownloadUpdate();
    return;
  }
  NotifyChanged();
}

std::string UpdaterService::SetCustomMirror(std::string_view input) {
  const std::optional<std::string> clean = SanitizeMirrorInput(input);
  if (!clean) {
    return base::UTF16ToUTF8(L("updates.mirror.invalid"));
  }
  if (custom_mirror_ == *clean) {
    return {};
  }
  custom_mirror_ = *clean;
  SaveState();
  NotifyChanged();
  return {};
}

void UpdaterService::DismissNotification() {
  if (available_version_.empty() || dismissed_version_ == available_version_) {
    return;
  }
  dismissed_version_ = available_version_;
  SaveState();
  NotifyChanged();
}

void UpdaterService::ResetNetworkState() {
  watchdog_timer_.Stop();
  loader_.reset();
  check_routes_.clear();
  check_route_index_ = 0;
  download_routes_.clear();
  download_route_index_ = 0;
  download_response_received_ = false;
}

// ---- 检查更新 ----

void UpdaterService::CheckNow(bool user_initiated) {
  if (status_ == UpdateStatus::kChecking ||
      status_ == UpdateStatus::kDownloading ||
      status_ == UpdateStatus::kVerifying) {
    return;
  }
  if (!user_initiated && status_ == UpdateStatus::kReady) {
    return;
  }
  if (!SystemLoaderFactory()) {
    return;
  }

  ResetNetworkState();
  check_user_initiated_ = user_initiated;
  error_message_.clear();
  status_ = UpdateStatus::kChecking;
  check_routes_ = BuildCheckRoutes(custom_mirror_, last_working_route_);
  check_route_index_ = 0;
  NotifyChanged();
  TryNextCheckRoute();
}

void UpdaterService::TryNextCheckRoute() {
  if (check_route_index_ >= check_routes_.size()) {
    FinishCheckFailed();
    return;
  }
  const UpdateRoute route = check_routes_[check_route_index_++];
  active_route_host_ = route.display_host;
  VLOG(1) << "[starry] updater checking via " << route.id;
  if (route.kind == UpdateRoute::Kind::kDirect) {
    FetchApiDirect(route);
  } else {
    FetchWebLatest(route);
  }
}

void UpdaterService::FetchApiDirect(const UpdateRoute& route) {
  network::mojom::URLLoaderFactory* factory = SystemLoaderFactory();
  if (!factory) {
    FinishCheckFailed();
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = DirectApiLatestUrl();
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  request->headers.SetHeader("Accept", "application/vnd.github+json");
  request->headers.SetHeader("X-GitHub-Api-Version", "2022-11-28");
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             UserAgentHeader());
  // 后台定时检查附带 If-None-Match：304 不计入 GitHub 60 次/小时限额。
  if (!check_user_initiated_ && !etag_.empty() && !last_seen_tag_.empty()) {
    request->headers.SetHeader("If-None-Match", etag_);
  }

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetAllowHttpErrorResults(true);
  loader_->SetTimeoutDuration(kMetadataTimeout);
  loader_->DownloadToString(
      factory,
      base::BindOnce(&UpdaterService::OnApiDirectComplete,
                     weak_factory_.GetWeakPtr(), route),
      kMaxMetadataBytes);
}

void UpdaterService::OnApiDirectComplete(UpdateRoute route,
                                         std::optional<std::string> body) {
  const int code = ResponseCodeOf(loader_.get());
  const net::HttpResponseHeaders* headers =
      loader_ && loader_->ResponseInfo() ? loader_->ResponseInfo()->headers.get()
                                         : nullptr;

  if (code == net::HTTP_NOT_MODIFIED) {
    // Release 没有变化；若此前看到的 tag 不比当前新，直接视为已是最新。
    if (std::optional<SemVer> seen = ParseSemVer(last_seen_tag_);
        !seen || !IsNewerVersion(*seen, current_version_)) {
      FinishUpToDate(route);
      return;
    }
    // 若此前看到过新版本，退回到网页端确认最新资产。
    FetchWebLatest(route);
    return;
  }

  if (code == net::HTTP_OK && body) {
    if (std::optional<ReleaseInfo> info = ParseGitHubReleaseApiJson(*body)) {
      if (headers) {
        if (std::optional<std::string> etag =
                headers->GetNormalizedHeader("ETag")) {
          etag_ = *etag;
        }
      }
      last_seen_tag_ = info->tag_name;

      if (info->draft ||
          (info->prerelease && !current_version_.IsPrerelease()) ||
          !IsNewerVersion(info->version, current_version_)) {
        FinishUpToDate(route);
        return;
      }

      const std::string version_str = info->version.Format();
      const std::string asset_name = ExpectedAssetName(env_, version_str);
      if (asset_name.empty()) {
        // 无法自动安装的平台环境：仍告知用户有新版本，可点击跳转 Release 页面。
        PendingCandidate candidate;
        candidate.tag_name = info->tag_name;
        candidate.version = info->version;
        candidate.release_notes = info->body;
        candidate.published_at = info->published_at;
        candidate.release_url = info->html_url;
        candidate.sha256_trusted_source = true;
        FinalizeCandidate(route, std::move(candidate));
        return;
      }

      const ReleaseAsset* asset = info->FindAsset(asset_name);
      if (!asset) {
        // 发版流程中 Linux/Windows 与 macOS 分批上传；当前平台包尚未就绪时不误报。
        VLOG(1) << "[starry] updater release " << info->tag_name
                << " does not yet contain " << asset_name;
        FinishUpToDate(route);
        return;
      }

      PendingCandidate candidate;
      candidate.tag_name = info->tag_name;
      candidate.version = info->version;
      candidate.release_notes = info->body;
      candidate.published_at = info->published_at;
      candidate.release_url = info->html_url;
      candidate.asset_name = asset->name;
      candidate.asset_size = asset->size;
      candidate.sha256 = asset->sha256;
      candidate.sha256_trusted_source = true;

      if (!candidate.sha256.empty()) {
        FinalizeCandidate(route, std::move(candidate));
        return;
      }
      // GitHub API 未返回 digest 时，读取随包上传的 `<asset>.sha256`。
      FetchSha256ForCandidate(route, std::move(candidate));
      return;
    }
  }

  // API 失败（如 CGNAT 触发 403 限流或 api.github.com 受阻）：
  // 先尝试直连 github.com/releases/latest（无 API 限额），再切国内镜像。
  VLOG(1) << "[starry] updater direct API failed (code=" << code << ")";
  FetchWebLatest(route);
}

void UpdaterService::FetchWebLatest(const UpdateRoute& route) {
  network::mojom::URLLoaderFactory* factory = SystemLoaderFactory();
  if (!factory) {
    FinishCheckFailed();
    return;
  }
  const GURL target = route.kind == UpdateRoute::Kind::kDirect
                          ? DirectWebLatestUrl()
                          : ApplyMirror(route.prefix, DirectWebLatestUrl());
  if (!target.is_valid()) {
    TryNextCheckRoute();
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = target;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  request->headers.SetHeader("Accept", "application/json, text/html");
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             UserAgentHeader());

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetAllowHttpErrorResults(true);
  loader_->SetTimeoutDuration(kMetadataTimeout);
  loader_->DownloadToString(
      factory,
      base::BindOnce(&UpdaterService::OnWebLatestComplete,
                     weak_factory_.GetWeakPtr(), route),
      kMaxMetadataBytes);
}

void UpdaterService::OnWebLatestComplete(UpdateRoute route,
                                         std::optional<std::string> body) {
  const int code = ResponseCodeOf(loader_.get());
  const GURL final_url = loader_ ? loader_->GetFinalURL() : GURL();
  if (code != net::HTTP_OK || !body ||
      (final_url.is_valid() && !final_url.SchemeIs(url::kHttpsScheme))) {
    VLOG(1) << "[starry] updater web latest failed via " << route.id
            << " (code=" << code << ")";
    TryNextCheckRoute();
    return;
  }

  const std::optional<std::string> tag =
      ExtractTagFromWebLatest(final_url, *body);
  if (!tag) {
    VLOG(1) << "[starry] updater could not parse tag via " << route.id;
    TryNextCheckRoute();
    return;
  }

  const std::optional<SemVer> version = ParseSemVer(*tag);
  if (!version) {
    TryNextCheckRoute();
    return;
  }

  last_seen_tag_ = *tag;
  if (!IsNewerVersion(*version, current_version_)) {
    FinishUpToDate(route);
    return;
  }

  PendingCandidate candidate;
  candidate.tag_name = *tag;
  candidate.version = *version;
  candidate.release_url = ReleasePageUrl(*tag);
  candidate.asset_name = ExpectedAssetName(env_, version->Format());
  candidate.sha256_trusted_source = route.kind != UpdateRoute::Kind::kMirror;

  if (candidate.asset_name.empty()) {
    FinalizeCandidate(route, std::move(candidate));
    return;
  }
  // 读取 `<asset>.sha256`：同时确认当前平台包已上传，并获取 SHA-256 校验和。
  FetchSha256ForCandidate(route, std::move(candidate));
}

void UpdaterService::FetchSha256ForCandidate(const UpdateRoute& route,
                                             PendingCandidate candidate) {
  network::mojom::URLLoaderFactory* factory = SystemLoaderFactory();
  if (!factory) {
    FinishCheckFailed();
    return;
  }
  const GURL raw_url =
      DirectAssetUrl(candidate.tag_name, candidate.asset_name + ".sha256");
  const GURL target = route.kind == UpdateRoute::Kind::kDirect
                          ? raw_url
                          : ApplyMirror(route.prefix, raw_url);
  if (!target.is_valid()) {
    TryNextCheckRoute();
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = target;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             UserAgentHeader());

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetAllowHttpErrorResults(true);
  loader_->SetTimeoutDuration(kMetadataTimeout);
  loader_->DownloadToString(
      factory,
      base::BindOnce(&UpdaterService::OnSha256Complete,
                     weak_factory_.GetWeakPtr(), route, std::move(candidate)),
      kMaxSha256Bytes);
}

void UpdaterService::OnSha256Complete(UpdateRoute route,
                                      PendingCandidate candidate,
                                      std::optional<std::string> body) {
  const int code = ResponseCodeOf(loader_.get());
  if (code == net::HTTP_NOT_FOUND) {
    // 当前平台资产尚未上传到该 Release，按已是最新处理，避免误报。
    VLOG(1) << "[starry] updater asset checksum 404 for "
            << candidate.asset_name << " via " << route.id;
    FinishUpToDate(route);
    return;
  }
  if (code != net::HTTP_OK || !body) {
    TryNextCheckRoute();
    return;
  }
  const std::optional<std::string> sha =
      ParseSha256File(*body, candidate.asset_name);
  if (!sha) {
    VLOG(1) << "[starry] updater invalid sha256 payload via " << route.id;
    TryNextCheckRoute();
    return;
  }
  candidate.sha256 = *sha;

  // 若元数据与校验和均来自第三方公共镜像，尝试从另一个独立镜像交叉核对 .sha256，
  // 防止单一公共镜像被劫持后同时伪造安装包与校验和。
  if (!candidate.sha256_trusted_source) {
    CrossCheckMirrorSha256(route, std::move(candidate), check_route_index_);
    return;
  }
  FinalizeCandidate(route, std::move(candidate));
}

void UpdaterService::CrossCheckMirrorSha256(const UpdateRoute& primary_route,
                                            PendingCandidate candidate,
                                            size_t next_mirror_index) {
  network::mojom::URLLoaderFactory* factory = SystemLoaderFactory();
  if (!factory) {
    FinalizeCandidate(primary_route, std::move(candidate));
    return;
  }
  while (next_mirror_index < check_routes_.size() &&
         (check_routes_[next_mirror_index].kind != UpdateRoute::Kind::kMirror ||
          check_routes_[next_mirror_index].id == primary_route.id)) {
    ++next_mirror_index;
  }
  if (next_mirror_index >= check_routes_.size()) {
    // 其余备用镜像均不可达，继续使用当前校验和。
    FinalizeCandidate(primary_route, std::move(candidate));
    return;
  }

  const UpdateRoute& secondary = check_routes_[next_mirror_index];
  const GURL target = ApplyMirror(
      secondary.prefix,
      DirectAssetUrl(candidate.tag_name, candidate.asset_name + ".sha256"));
  if (!target.is_valid()) {
    CrossCheckMirrorSha256(primary_route, std::move(candidate),
                           next_mirror_index + 1);
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = target;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             UserAgentHeader());

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetAllowHttpErrorResults(true);
  loader_->SetTimeoutDuration(kCrossCheckTimeout);
  loader_->DownloadToString(
      factory,
      base::BindOnce(&UpdaterService::OnCrossCheckComplete,
                     weak_factory_.GetWeakPtr(), primary_route,
                     std::move(candidate), next_mirror_index),
      kMaxSha256Bytes);
}

void UpdaterService::OnCrossCheckComplete(UpdateRoute primary_route,
                                          PendingCandidate candidate,
                                          size_t checked_mirror_index,
                                          std::optional<std::string> body) {
  const int code = ResponseCodeOf(loader_.get());
  if (code == net::HTTP_OK && body) {
    if (const std::optional<std::string> second_sha =
            ParseSha256File(*body, candidate.asset_name)) {
      if (*second_sha != candidate.sha256) {
        LOG(WARNING) << "[starry] updater sha256 mismatch across mirrors ("
                     << primary_route.display_host << " vs "
                     << check_routes_[checked_mirror_index].display_host
                     << "); rejecting primary mirror";
        TryNextCheckRoute();
        return;
      }
      FinalizeCandidate(primary_route, std::move(candidate));
      return;
    }
  }
  // 第二个镜像不可用或返回异常：尝试下一个镜像交叉核对。
  CrossCheckMirrorSha256(primary_route, std::move(candidate),
                         checked_mirror_index + 1);
}

void UpdaterService::FinalizeCandidate(const UpdateRoute& winning_route,
                                       PendingCandidate candidate) {
  loader_.reset();
  last_checked_ = base::Time::Now();
  winning_check_route_ = winning_route.id;
  last_working_route_ = winning_route.id;
  active_route_host_ = winning_route.display_host;
  error_message_.clear();

  const std::string ver_str = candidate.version.Format();
  available_version_ = ver_str;
  release_tag_ = candidate.tag_name;
  release_url_ = candidate.release_url.is_valid()
                     ? candidate.release_url
                     : ReleasePageUrl(candidate.tag_name);
  if (!candidate.release_notes.empty()) {
    release_notes_ = candidate.release_notes;
  }
  if (!candidate.published_at.empty()) {
    published_at_ = candidate.published_at;
  }
  asset_name_ = candidate.asset_name;
  expected_size_ = candidate.asset_size;
  expected_sha256_ = candidate.sha256;

  // 若该版本此前已下载并校验完毕，直接保持 kReady。
  if (!staged_path_.empty() && staged_version_ == ver_str) {
    status_ = UpdateStatus::kReady;
    SaveState();
    NotifyChanged();
    return;
  }

  SaveState();
  if (mode_ == AutoUpdateMode::kAuto && env_.can_auto_install &&
      !asset_name_.empty() && !expected_sha256_.empty()) {
    PrepareAndStartDownload();
    return;
  }

  status_ = UpdateStatus::kAvailable;
  NotifyChanged();
}

void UpdaterService::FinishUpToDate(const UpdateRoute& winning_route) {
  loader_.reset();
  last_checked_ = base::Time::Now();
  last_working_route_ = winning_route.id;
  active_route_host_ = winning_route.display_host;
  error_message_.clear();
  status_ = UpdateStatus::kUpToDate;
  SaveState();
  NotifyChanged();
}

void UpdaterService::FinishCheckFailed() {
  loader_.reset();
  if (!staged_path_.empty() && !staged_version_.empty()) {
    status_ = UpdateStatus::kReady;
    NotifyChanged();
    return;
  }
  if (check_user_initiated_) {
    status_ = UpdateStatus::kError;
    error_message_ = base::UTF16ToUTF8(L("updates.error.checkFailed"));
  } else {
    status_ = UpdateStatus::kIdle;
  }
  NotifyChanged();
}

// ---- 下载与校验 ----

void UpdaterService::DownloadUpdate() {
  if (status_ == UpdateStatus::kDownloading ||
      status_ == UpdateStatus::kVerifying ||
      available_version_.empty() || asset_name_.empty() ||
      expected_sha256_.empty() || !env_.can_auto_install) {
    return;
  }
  PrepareAndStartDownload();
}

void UpdaterService::CancelDownload() {
  if (status_ != UpdateStatus::kDownloading) {
    return;
  }
  watchdog_timer_.Stop();
  loader_.reset();
  if (!active_download_file_.empty()) {
    file_runner_->PostTask(
        FROM_HERE, base::BindOnce(base::IgnoreResult(&base::DeleteFile),
                                  active_download_file_));
    active_download_file_.clear();
  }
  bytes_received_ = 0;
  total_bytes_ = expected_size_;
  status_ = available_version_.empty() ? UpdateStatus::kIdle
                                       : UpdateStatus::kAvailable;
  NotifyChanged();
}

void UpdaterService::PrepareAndStartDownload() {
  if (directory_.empty() || !SystemLoaderFactory()) {
    return;
  }
  ResetNetworkState();
  status_ = UpdateStatus::kDownloading;
  error_message_.clear();
  bytes_received_ = 0;
  total_bytes_ = expected_size_;
  download_routes_ = BuildDownloadRoutes(custom_mirror_, winning_check_route_,
                                         last_working_route_);
  download_route_index_ = 0;
  NotifyChanged();

  const base::FilePath staging_dir = StagingDirFor(available_version_);
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath dir) { return base::CreateDirectory(dir); },
          staging_dir),
      base::BindOnce(&UpdaterService::OnStagingDirReady,
                     weak_factory_.GetWeakPtr()));
}

void UpdaterService::OnStagingDirReady(bool ok) {
  if (status_ != UpdateStatus::kDownloading) {
    return;
  }
  if (!ok) {
    status_ = UpdateStatus::kError;
    error_message_ = base::UTF16ToUTF8(L("updates.error.downloadFailed"));
    NotifyChanged();
    return;
  }
  TryNextDownloadRoute();
}

void UpdaterService::TryNextDownloadRoute() {
  watchdog_timer_.Stop();
  loader_.reset();
  if (download_route_index_ >= download_routes_.size()) {
    status_ = UpdateStatus::kError;
    error_message_ = base::UTF16ToUTF8(L("updates.error.downloadFailed"));
    NotifyChanged();
    return;
  }

  network::mojom::URLLoaderFactory* factory = SystemLoaderFactory();
  if (!factory) {
    status_ = UpdateStatus::kError;
    error_message_ = base::UTF16ToUTF8(L("updates.error.downloadFailed"));
    NotifyChanged();
    return;
  }

  const UpdateRoute route = download_routes_[download_route_index_++];
  const GURL raw_url = DirectAssetUrl(release_tag_, asset_name_);
  const GURL url = route.kind == UpdateRoute::Kind::kDirect
                       ? raw_url
                       : ApplyMirror(route.prefix, raw_url);
  if (!url.is_valid()) {
    TryNextDownloadRoute();
    return;
  }

  active_route_host_ = route.display_host;
  bytes_received_ = 0;
  total_bytes_ = expected_size_;
  download_response_received_ = false;
  download_route_started_ = base::TimeTicks::Now();
  download_response_started_ = base::TimeTicks();
  last_progress_time_ = download_route_started_;
  active_download_file_ =
      StagingDirFor(available_version_)
          .Append(FILE_PATH_LITERAL("package.download"));
  NotifyChanged();

  VLOG(1) << "[starry] updater downloading " << asset_name_ << " via "
          << route.id;

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             UserAgentHeader());

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetOnRedirectCallback(base::BindRepeating(
      &UpdaterService::OnDownloadRedirect, weak_factory_.GetWeakPtr()));
  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &UpdaterService::OnDownloadResponseStarted, weak_factory_.GetWeakPtr()));
  loader_->SetOnDownloadProgressCallback(base::BindRepeating(
      &UpdaterService::OnDownloadProgress, weak_factory_.GetWeakPtr()));
  loader_->DownloadToFile(
      factory,
      base::BindOnce(&UpdaterService::OnDownloadFileComplete,
                     weak_factory_.GetWeakPtr()),
      active_download_file_, kMaxPackageBytes);

  watchdog_timer_.Start(FROM_HERE, kWatchdogInterval,
                        base::BindRepeating(
                            &UpdaterService::OnDownloadWatchdogTick,
                            base::Unretained(this)));
}

void UpdaterService::OnDownloadRedirect(
    const GURL& url_before_redirect,
    const net::RedirectInfo& redirect_info,
    const network::mojom::URLResponseHead& head,
    std::vector<std::string>* removed_headers) {
  if (!redirect_info.new_url.SchemeIs(url::kHttpsScheme)) {
    AbortCurrentDownloadRoute("redirected to non-HTTPS URL");
  }
}

void UpdaterService::OnDownloadResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  const int code = head.headers ? head.headers->response_code() : 0;
  if (code != net::HTTP_OK ||
      (final_url.is_valid() && !final_url.SchemeIs(url::kHttpsScheme))) {
    AbortCurrentDownloadRoute("non-200 or non-HTTPS download response");
    return;
  }
  // 拦截镜像站或网络劫持返回的 HTML 错误页 / 验证码页。
  if (head.mime_type == "text/html" ||
      head.mime_type == "application/xhtml+xml") {
    AbortCurrentDownloadRoute("unexpected HTML mime type");
    return;
  }
  if (head.content_length > 0) {
    total_bytes_ = head.content_length;
  }
  download_response_received_ = true;
  download_response_started_ = base::TimeTicks::Now();
  last_progress_time_ = download_response_started_;
  NotifyChanged();
}

void UpdaterService::OnDownloadProgress(uint64_t current) {
  bytes_received_ = static_cast<int64_t>(current);
  last_progress_time_ = base::TimeTicks::Now();
  NotifyChanged(/*throttled=*/true);
}

void UpdaterService::OnDownloadWatchdogTick() {
  if (status_ != UpdateStatus::kDownloading || !loader_) {
    watchdog_timer_.Stop();
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  const bool has_fallback = download_route_index_ < download_routes_.size();

  if (!download_response_received_ &&
      now - download_route_started_ >= kDownloadStallTimeout) {
    AbortCurrentDownloadRoute("response header timeout");
    return;
  }
  if (now - last_progress_time_ >= kDownloadStallTimeout) {
    AbortCurrentDownloadRoute("download stalled");
    return;
  }
  if (download_response_received_ && has_fallback &&
      now - download_response_started_ >= kDownloadSpeedWindow) {
    const double seconds = (now - download_response_started_).InSecondsF();
    const double rate = seconds > 0 ? bytes_received_ / seconds : 0;
    if (rate < kMinAcceptableBytesPerSec) {
      AbortCurrentDownloadRoute("slow route failover");
      return;
    }
  }
}

void UpdaterService::AbortCurrentDownloadRoute(std::string_view reason) {
  VLOG(1) << "[starry] updater switching download route from "
          << active_route_host_ << " (" << reason << ")";
  watchdog_timer_.Stop();
  loader_.reset();
  TryNextDownloadRoute();
}

void UpdaterService::OnDownloadFileComplete(base::FilePath path) {
  watchdog_timer_.Stop();
  const int net_error = loader_ ? loader_->NetError() : net::ERR_FAILED;
  loader_.reset();
  if (status_ != UpdateStatus::kDownloading) {
    return;
  }
  if (path.empty() || net_error != net::OK) {
    VLOG(1) << "[starry] updater download failed via " << active_route_host_
            << " (net_error=" << net_error << ")";
    TryNextDownloadRoute();
    return;
  }

  // 记录这次成功完成下载的通道。
  if (download_route_index_ > 0 &&
      download_route_index_ <= download_routes_.size()) {
    last_working_route_ = download_routes_[download_route_index_ - 1].id;
  }

  status_ = UpdateStatus::kVerifying;
  NotifyChanged();

  const base::FilePath staging_dir = StagingDirFor(available_version_);
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&StageDownloadedUpdate, env_, path, staging_dir,
                     available_version_, expected_sha256_),
      base::BindOnce(&UpdaterService::OnStageComplete,
                     weak_factory_.GetWeakPtr()));
}

const std::u16string& StageErrorText(StageError error) {
  switch (error) {
    case StageError::kChecksum:
      return L("updates.error.checksum");
    case StageError::kUnpack:
      return L("updates.error.unpack");
    case StageError::kInvalidPackage:
      return L("updates.error.invalidPackage");
    case StageError::kUnsupported:
      return L("updates.error.unsupported");
    case StageError::kNone:
      return L("updates.error.verifyFailed");
  }
}

void UpdaterService::OnStageComplete(StageResult result) {
  if (status_ != UpdateStatus::kVerifying) {
    return;
  }
  if (!result.ok) {
    LOG(WARNING) << "[starry] updater stage failed: " << result.error_detail;
    if (!active_download_file_.empty()) {
      file_runner_->PostTask(
          FROM_HERE, base::BindOnce(base::IgnoreResult(&base::DeleteFile),
                                    active_download_file_));
      active_download_file_.clear();
    }
    // 当前通道下的包校验/验签不通过：尝试下一个备用通道。
    if (result.error != StageError::kUnsupported &&
        download_route_index_ < download_routes_.size()) {
      status_ = UpdateStatus::kDownloading;
      TryNextDownloadRoute();
      return;
    }
    status_ = UpdateStatus::kError;
    error_message_ = base::UTF16ToUTF8(StageErrorText(result.error));
    NotifyChanged();
    return;
  }

  active_download_file_.clear();
  staged_version_ = available_version_;
  staged_path_ = result.staged_path;
  staged_dir_ = StagingDirFor(available_version_);
  status_ = UpdateStatus::kReady;
  error_message_.clear();
  SaveState();
  NotifyChanged();
}

// ---- 应用更新 ----

void UpdaterService::ApplyUpdate() {
  if (status_ != UpdateStatus::kReady || staged_path_.empty()) {
    return;
  }

  if (env_.kind == InstallKind::kLinuxDeb ||
      env_.kind == InstallKind::kLinuxRpm) {
    Profile* profile = AnyRegularProfile();
    platform_util::OpenItem(
        profile, staged_path_, platform_util::OPEN_FILE,
        base::BindOnce(
            [](base::FilePath path, platform_util::OpenOperationResult result) {
              if (result != platform_util::OPEN_SUCCEEDED) {
                platform_util::ShowItemInFolder(AnyRegularProfile(), path);
              }
            },
            staged_path_));
    return;
  }

  // macOS & Windows：保存所有窗口会话后发起干净退出，在 PostDestroyThreads 里
  // 启动独立替换助手并带 --restart 重启恢复会话（不置 kRestartLastSessionOnShutdown，
  // 避免与 Chromium 自带重启器争抢替换时机）。
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [](BrowserWindowInterface* browser) {
        browser->GetProfile()->SaveSessionState();
#if BUILDFLAG(ENABLE_SESSION_SERVICE)
        if (auto* session_data_service =
                SessionDataServiceFactory::GetForProfile(
                    browser->GetProfile())) {
          session_data_service->SetForceKeepSessionState();
        }
#endif
        return true;
      });

  if (PrefService* local_state = g_browser_process->local_state()) {
    local_state->SetBoolean(::prefs::kWasRestarted, true);
    local_state->CommitPendingWrite();
  }

  pending_apply_on_exit_ = true;
  relaunch_after_apply_ = true;
  closing_subscription_ = chrome::AddClosingAllBrowsersCallback(
      base::BindRepeating(&UpdaterService::OnClosingAllBrowsers,
                          weak_factory_.GetWeakPtr()));
  chrome::AttemptExit();
}

void UpdaterService::RevealStagedPackage() {
  if (staged_path_.empty()) {
    return;
  }
  platform_util::ShowItemInFolder(AnyRegularProfile(), staged_path_);
}

void UpdaterService::OnClosingAllBrowsers(bool closing) {
  if (!closing) {
    // 用户在 beforeunload 或未完成下载确认框里取消了关闭：撤销待应用的重启标记。
    CancelPendingApply();
  }
}

void UpdaterService::CancelPendingApply() {
  pending_apply_on_exit_ = false;
  relaunch_after_apply_ = false;
  closing_subscription_ = {};
  if (PrefService* local_state =
          g_browser_process ? g_browser_process->local_state() : nullptr) {
    local_state->ClearPref(::prefs::kWasRestarted);
  }
}

}  // namespace starry::updater
