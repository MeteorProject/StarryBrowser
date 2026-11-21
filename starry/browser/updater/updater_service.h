// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UPDATER_UPDATER_SERVICE_H_
#define STARRY_BROWSER_UPDATER_UPDATER_SERVICE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "starry/browser/updater/platform_updater.h"
#include "starry/browser/updater/release_sources.h"
#include "starry/browser/updater/semver.h"
#include "url/gurl.h"

namespace base {
class SequencedTaskRunner;
}

namespace net {
struct RedirectInfo;
}

namespace network {
class SimpleURLLoader;
namespace mojom {
class URLResponseHead;
}
}  // namespace network

namespace starry::updater {

enum class UpdateStatus {
  kIdle,
  kChecking,
  kUpToDate,
  kAvailable,
  kDownloading,
  kVerifying,
  kReady,
  kError,
};

enum class AutoUpdateMode {
  // 自动检查并在后台静默下载，校验就绪后提示重启更新（默认）。
  kAuto,
  // 自动检查并提醒有新版本，由用户点击后再下载安装。
  kNotify,
  // 关闭自动检查，仅响应用户手动点击「检查更新」。
  kOff,
};

// 全应用共享的自动更新服务（仅在 UI 线程访问，文件 I/O 与校验在后台序列执行）：
// - 通过 GitHub Releases 检查最新版本，自动跳过尚未上传当前平台资产的不完整发版；
// - 面向中国大陆网络环境内置多级 HTTPS 镜像自动回退、直连/镜像智能路由、防 API 限流
//   的 Web 重定向回退以及慢速/卡死连接看门狗；
// - 所有安装包均执行 SHA-256 校验（经第三方镜像检查时跨镜像二次比对校验和）；
// - 状态与设置持久化到 `<UserDataDir>/Starry Updates/State.json`。
class UpdaterService {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnUpdateStateChanged() = 0;
  };

  struct StateSnapshot {
    StateSnapshot();
    StateSnapshot(const StateSnapshot&);
    StateSnapshot& operator=(const StateSnapshot&);
    ~StateSnapshot();

    UpdateStatus status = UpdateStatus::kIdle;
    AutoUpdateMode mode = AutoUpdateMode::kAuto;
    InstallKind install_kind = InstallKind::kManual;
    bool can_auto_install = false;
    std::string current_version;
    std::string available_version;
    std::string release_tag;
    GURL release_url;
    std::string release_notes;
    std::string published_at;
    std::string asset_name;
    base::Time last_checked;
    std::string custom_mirror;
    // 当前正在使用或最近一次命中的通道主机名（如 "github.com" / "gh-proxy.org"）。
    std::string active_route_host;
    int64_t bytes_received = 0;
    int64_t total_bytes = 0;
    // 本地化后的错误文案（仅 kError 时非空）。
    std::string error_message;
    // 用户在当前版本上点击过「稍后」，不再弹出窗口提示条（设置图标红点保留）。
    bool notification_dismissed = false;
    base::FilePath staged_path;
  };

  static UpdaterService* Get();

  UpdaterService(const UpdaterService&) = delete;
  UpdaterService& operator=(const UpdaterService&) = delete;

  // 在浏览器启动完成后调用（StarryBrowserMainExtraParts::PostBrowserStart）。
  void Start();
  // 在主消息循环结束后调用（StarryBrowserMainExtraParts::PostMainMessageLoopRun）。
  void Shutdown();
  // 在线程停止后、进程即将退出前调用（StarryBrowserMainExtraParts::PostDestroyThreads），
  // 启动独立的替换与重启助手。
  void OnPostDestroyThreads();

  StateSnapshot Snapshot() const;
  bool HasAttention() const;
  bool ShouldShowToast() const;

  void CheckNow(bool user_initiated);
  void DownloadUpdate();
  void CancelDownload();
  void ApplyUpdate();
  void RevealStagedPackage();
  void DismissNotification();

  void SetAutoUpdateMode(AutoUpdateMode mode);
  // 校验并保存自定义镜像地址；空串表示清除。返回空串表示成功，否则返回本地化错误文案。
  std::string SetCustomMirror(std::string_view input);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  static const char* StatusName(UpdateStatus status);
  static const char* ModeName(AutoUpdateMode mode);
  static std::optional<AutoUpdateMode> ModeFromName(std::string_view name);

 private:
  struct LoadedState;
  struct PendingCandidate;
  friend class base::NoDestructor<UpdaterService>;

  UpdaterService();
  ~UpdaterService();

  base::FilePath StagingDirFor(std::string_view version) const;
  void OnStateLoaded(std::unique_ptr<LoadedState> loaded);
  void SaveState();
  void NotifyChanged(bool throttled = false);
  void ScheduleTimers();
  void OnPeriodicCheck();

  // ---- 多通道检查流程 ----
  void TryNextCheckRoute();
  void FetchApiDirect(const UpdateRoute& route);
  void OnApiDirectComplete(UpdateRoute route, std::optional<std::string> body);
  void FetchWebLatest(const UpdateRoute& route);
  void OnWebLatestComplete(UpdateRoute route, std::optional<std::string> body);
  void FetchSha256ForCandidate(const UpdateRoute& route,
                               PendingCandidate candidate);
  void OnSha256Complete(UpdateRoute route,
                        PendingCandidate candidate,
                        std::optional<std::string> body);
  void CrossCheckMirrorSha256(const UpdateRoute& primary_route,
                              PendingCandidate candidate,
                              size_t next_mirror_index);
  void OnCrossCheckComplete(UpdateRoute primary_route,
                            PendingCandidate candidate,
                            size_t checked_mirror_index,
                            std::optional<std::string> body);
  void FinalizeCandidate(const UpdateRoute& winning_route,
                         PendingCandidate candidate);
  void FinishUpToDate(const UpdateRoute& winning_route);
  void FinishCheckFailed();

  // ---- 多通道下载与看门狗 ----
  void PrepareAndStartDownload();
  void OnStagingDirReady(bool ok);
  void TryNextDownloadRoute();
  void OnDownloadRedirect(const GURL& url_before_redirect,
                          const net::RedirectInfo& redirect_info,
                          const network::mojom::URLResponseHead& head,
                          std::vector<std::string>* removed_headers);
  void OnDownloadResponseStarted(const GURL& final_url,
                                 const network::mojom::URLResponseHead& head);
  void OnDownloadProgress(uint64_t current);
  void OnDownloadWatchdogTick();
  void AbortCurrentDownloadRoute(std::string_view reason);
  void OnDownloadFileComplete(base::FilePath path);
  void OnStageComplete(StageResult result);

  void OnClosingAllBrowsers(bool closing);
  void CancelPendingApply();
  void ResetNetworkState();

  base::FilePath directory_;
  InstallEnvironment env_;
  SemVer current_version_;
  scoped_refptr<base::SequencedTaskRunner> file_runner_;

  bool started_ = false;
  bool loaded_ = false;
  bool auto_check_disabled_by_flag_ = false;

  UpdateStatus status_ = UpdateStatus::kIdle;
  AutoUpdateMode mode_ = AutoUpdateMode::kAuto;
  std::string custom_mirror_;
  std::string last_working_route_;
  std::string winning_check_route_;
  std::string active_route_host_;
  std::string etag_;
  std::string last_seen_tag_;
  base::Time last_checked_;
  std::string dismissed_version_;

  // 检测到的目标版本信息。
  std::string available_version_;
  std::string release_tag_;
  GURL release_url_;
  std::string release_notes_;
  std::string published_at_;
  std::string asset_name_;
  std::string expected_sha256_;
  int64_t expected_size_ = 0;

  // 已下载校验完毕的更新。
  std::string staged_version_;
  base::FilePath staged_path_;
  base::FilePath staged_dir_;

  // 下载进度与看门狗状态。
  int64_t bytes_received_ = 0;
  int64_t total_bytes_ = 0;
  base::TimeTicks download_route_started_;
  base::TimeTicks download_response_started_;
  base::TimeTicks last_progress_time_;
  base::TimeTicks last_progress_notify_;
  bool download_response_received_ = false;
  base::FilePath active_download_file_;

  // 检查 / 下载通道队列。
  bool check_user_initiated_ = false;
  std::vector<UpdateRoute> check_routes_;
  size_t check_route_index_ = 0;
  std::vector<UpdateRoute> download_routes_;
  size_t download_route_index_ = 0;

  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::OneShotTimer startup_timer_;
  base::RepeatingTimer periodic_timer_;
  base::RepeatingTimer watchdog_timer_;

  std::string error_message_;
  bool pending_apply_on_exit_ = false;
  bool relaunch_after_apply_ = false;
  base::CallbackListSubscription closing_subscription_;

  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<UpdaterService> weak_factory_{this};
};

}  // namespace starry::updater

#endif  // STARRY_BROWSER_UPDATER_UPDATER_SERVICE_H_
