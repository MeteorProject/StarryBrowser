// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/download_service.h"

#include <algorithm>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/one_shot_event.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/content_settings/cookie_settings_factory.h"
#include "chrome/browser/download/chrome_download_manager_delegate.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_request_utils.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_system.h"
#include "net/cookies/cookie_setting_override.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/referrer_policy.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "starry/browser/downloads/chrome_download_bridge.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/downloads/download_info.h"
#include "starry/browser/downloads/download_notifications.h"
#include "starry/browser/downloads/starry_download.h"
#include "starry/browser/downloads/starry_download_item.h"
#include "starry/browser/prefs/starry_prefs.h"

namespace starry {

namespace {

constexpr base::FilePath::CharType kFileName[] =
    FILE_PATH_LITERAL("Starry Downloads.json");
// 进度和速度多久刷新一次。
constexpr base::TimeDelta kTickInterval = base::Milliseconds(500);
// 下载中多久把数据刷到磁盘、存一次分段进度。
constexpr base::TimeDelta kCheckpointInterval = base::Seconds(3);
// 状态变了以后多久存盘（合并一段时间里的变化）。
constexpr base::TimeDelta kSaveDelay = base::Seconds(1);
// 设置里的连接数、同时下载数的上限。
constexpr int kMaxConnections = 16;
constexpr int kMaxConcurrent = 10;

constexpr net::NetworkTrafficAnnotationTag kStartAnnotation =
    net::DefineNetworkTrafficAnnotation("starry_start_download", R"(
        semantics {
          sender: "Starry downloads"
          description:
            "The user asked to download a file again, typed the address of "
            "a file to download on the downloads page, or a download that "
            "Starry could not fetch itself is retried the way the page "
            "started it."
          trigger: "User clicks retry, redownload or starts a new download."
          data: "None, other than the request for the file."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting: "This feature cannot be disabled in settings."
          policy_exception_justification: "User initiated."
        })");

class DownloadServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static DownloadServiceFactory* GetInstance() {
    static base::NoDestructor<DownloadServiceFactory> instance;
    return instance.get();
  }

  static DownloadService* GetForProfile(Profile* profile) {
    return static_cast<DownloadService*>(
        GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
  }

  DownloadServiceFactory()
      : ProfileKeyedServiceFactory(
            "StarryDownloadService",
            ProfileSelections::Builder()
                .WithRegular(ProfileSelection::kOwnInstance)
                .WithGuest(ProfileSelection::kOwnInstance)
                .Build()) {
    DependsOn(DownloadCoreServiceFactory::GetInstance());
  }
  DownloadServiceFactory(const DownloadServiceFactory&) = delete;
  DownloadServiceFactory& operator=(const DownloadServiceFactory&) = delete;
  ~DownloadServiceFactory() override = default;

 private:
  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override {
    return std::make_unique<DownloadService>(
        Profile::FromBrowserContext(context));
  }
  // 交接要在第一个下载之前就在看着，上次没下完的也要启动时就读回来。
  bool ServiceIsCreatedWithBrowserContext() const override { return true; }
};

// 开始了、还没关闭的（GetIfRunning）。
std::vector<DownloadService*>& RunningServices() {
  static base::NoDestructor<std::vector<DownloadService*>> services;
  return *services;
}

// 可能有危险的（危险的文件类型、网址……）留给 Chrome，由它问用户。只是「下完要把内容送去
// Safe Browsing 检查」的（MAYBE_DANGEROUS_CONTENT，dmg、zip 这些几乎都是）照样交给 Starry：
// Starry 的构建没有 Google 的 API 密钥（starry/build/args/common.gni），这个检查本来就做不了。
bool IsSafeDangerType(download::DownloadDangerType type) {
  switch (type) {
    case download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS:
    case download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT:
    case download::DOWNLOAD_DANGER_TYPE_USER_VALIDATED:
    case download::DOWNLOAD_DANGER_TYPE_ALLOWLISTED_BY_POLICY:
      return true;
    default:
      return false;
  }
}

}  // namespace

// 读回来的一条记录，顺便看了临时文件和下好的文件还在不在。
struct DownloadService::Loaded {
  base::DictValue record;
  bool partial_exists = false;
  bool target_exists = false;
};

// static
DownloadService* DownloadService::Get(Profile* profile) {
  return DownloadServiceFactory::GetForProfile(profile);
}

// static
DownloadService* DownloadService::GetIfRunning(const Profile* profile) {
  for (DownloadService* service : RunningServices()) {
    if (service->profile_ == profile) {
      return service;
    }
  }
  return nullptr;
}

// static
const std::vector<DownloadService*>& DownloadService::Running() {
  return RunningServices();
}

// static
void DownloadService::EnsureFactoryBuilt() {
  DownloadServiceFactory::GetInstance();
}

DownloadService::DownloadService(Profile* profile)
    : profile_(profile),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  // 用户资料这时还没建完（DownloadManager 要的服务不一定都在）：下一轮再开始看。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&DownloadService::Init, weak_factory_.GetWeakPtr()));
  RunningServices().push_back(this);
}

DownloadService::~DownloadService() {
  std::erase(RunningServices(), this);
}

void DownloadService::Init() {
  if (shut_down_) {
    return;
  }
  if (content::DownloadManager* manager = profile_->GetDownloadManager()) {
    notifier_ =
        std::make_unique<download::AllDownloadItemNotifier>(manager, this);
    content::DownloadManager::DownloadVector all;
    manager->GetAllDownloads(&all);
    for (download::DownloadItem* item : all) {
      AddChromeDownload(item);
    }
  }
  prefs_.Init(prefs::PrefsFor(profile_));
  for (const char* name :
       {prefs::kDownloadMultiConnection, prefs::kDownloadConnections,
        prefs::kDownloadMaxConcurrent, prefs::kDownloadSpeedLimit}) {
    prefs_.Add(name, base::BindRepeating(&DownloadService::OnPrefsChanged,
                                         base::Unretained(this)));
  }
  effective_connections_ = EffectiveConnections();
  OnPrefsChanged();

  if (profile_->IsOffTheRecord()) {
    loaded_ = true;
    return;
  }
  notifications_ = std::make_unique<DownloadNotifications>(this);
  const base::FilePath path = profile_->GetPath().Append(kFileName);
  writer_ = std::make_unique<base::ImportantFileWriter>(path, file_task_runner_,
                                                        kSaveDelay);
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&DownloadService::ReadRecords, path),
      base::BindOnce(&DownloadService::OnLoaded, weak_factory_.GetWeakPtr()));
}

// ---- 列表 ----

std::vector<Download*> DownloadService::Downloads() const {
  std::vector<Download*> all;
  for (const auto& [guid, download] : chrome_downloads_) {
    all.push_back(download.get());
  }
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    all.push_back(download.get());
  }
  std::ranges::stable_sort(all, [](Download* a, Download* b) {
    return a->StartTime() > b->StartTime();
  });
  return all;
}

Download* DownloadService::Find(std::string_view guid) const {
  if (StarryDownload* download = FindStarry(guid)) {
    return download;
  }
  auto it = chrome_downloads_.find(guid);
  return it != chrome_downloads_.end() ? it->second.get() : nullptr;
}

StarryDownload* DownloadService::FindStarry(std::string_view guid) const {
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (download->Guid() == guid) {
      return download.get();
    }
  }
  return nullptr;
}

void DownloadService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void DownloadService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

// ---- 操作 ----

void DownloadService::StartDownload(const GURL& url,
                                    content::WebContents* contents,
                                    const GURL& referrer) {
  content::DownloadManager* manager =
      notifier_ ? notifier_->GetManager() : nullptr;
  if (!manager || !url.is_valid()) {
    return;
  }
  std::unique_ptr<download::DownloadUrlParameters> params =
      contents ? content::DownloadRequestUtils::
                     CreateDownloadForWebContentsMainFrame(contents, url,
                                                           kStartAnnotation)
               : std::make_unique<download::DownloadUrlParameters>(
                     url, kStartAnnotation);
  if (referrer.is_valid()) {
    params->set_referrer(referrer);
    params->set_referrer_policy(
        net::ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN);
  }
  manager->DownloadUrl(std::move(params));
}

void DownloadService::Retry(Download* download,
                            content::WebContents* contents) {
  if (!download) {
    return;
  }
  const DownloadState state = download->State();
  if (download->IsStarry() && (state == DownloadState::kPaused ||
                               state == DownloadState::kInterrupted)) {
    // Starry 自己下载的：不支持分段的也由下载器从头下。
    download->Resume();
    return;
  }
  if (!download->IsStarry() && download->CanResume() &&
      state != DownloadState::kComplete && state != DownloadState::kCancelled) {
    download->Resume();
    return;
  }
  StartDownload(download->Url(), contents, download->ReferrerUrl());
}

void DownloadService::ClearInactive() {
  std::vector<std::string> guids;
  for (Download* download : Downloads()) {
    if (!IsActive(download->State())) {
      guids.push_back(download->Guid());
    }
  }
  for (const std::string& guid : guids) {
    if (Download* download = Find(guid)) {
      download->Remove();
    }
  }
}

void DownloadService::ClearAll() {
  std::vector<std::string> guids;
  for (Download* download : Downloads()) {
    guids.push_back(download->Guid());
  }
  for (const std::string& guid : guids) {
    if (Download* download = Find(guid)) {
      download->Remove();
    }
  }
}

void DownloadService::PauseAll() {
  for (Download* download : Downloads()) {
    const DownloadState state = download->State();
    if (state == DownloadState::kRunning || state == DownloadState::kQueued) {
      download->Pause();
    }
  }
}

void DownloadService::ResumeAll() {
  // 先开始的先排上。
  std::vector<Download*> all = Downloads();
  for (auto it = all.rbegin(); it != all.rend(); ++it) {
    if ((*it)->State() == DownloadState::kPaused) {
      (*it)->Resume();
    }
  }
}

int DownloadService::LostOnShutdownCount() const {
  if (!profile_->IsOffTheRecord()) {
    return 0;
  }
  return std::ranges::count_if(
      starry_downloads_, [](const std::unique_ptr<StarryDownload>& download) {
        return IsActive(download->State());
      });
}

void DownloadService::CancelLostOnShutdown() {
  if (!profile_->IsOffTheRecord()) {
    return;
  }
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (IsActive(download->State())) {
      download->Cancel();
    }
  }
}

void DownloadService::CheckFilesExist() {
  if (notifier_ && notifier_->GetManager()) {
    notifier_->GetManager()->CheckForHistoryFilesRemoval();
  }
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    download->CheckFileExists();
  }
}

// ---- 给 Chrome 的代码看的样子（chrome_download_bridge.h） ----

StarryDownloadItem* DownloadService::FindItem(uint32_t id) const {
  if (id == download::DownloadItem::kInvalidId) {
    return nullptr;
  }
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (download->item()->GetId() == id) {
      return download->item();
    }
  }
  return nullptr;
}

void DownloadService::AppendItems(
    std::vector<raw_ptr<download::DownloadItem, VectorExperimental>>* items)
    const {
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (download->item()->has_id()) {
      items->push_back(download->item());
    }
  }
}

void DownloadService::AddProgress(int* count,
                                  int64_t* received_bytes,
                                  int64_t* total_bytes,
                                  bool* certain) const {
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (!IsActive(download->State())) {
      continue;
    }
    ++*count;
    if (download->TotalBytes() <= 0) {
      *certain = false;
    } else {
      *received_bytes += download->ReceivedBytes();
      *total_bytes += download->TotalBytes();
    }
  }
}

// ---- Chrome 的下载 ----

void DownloadService::AddChromeDownload(download::DownloadItem* item) {
  const std::string& guid = item->GetGuid();
  if (!ShouldShowDownload(*item) || handed_off_.contains(guid) ||
      chrome_downloads_.contains(guid)) {
    return;
  }
  const bool is_new = item->GetState() == download::DownloadItem::IN_PROGRESS &&
                      item->GetDownloadCreationType() ==
                          download::DownloadItem::TYPE_ACTIVE_DOWNLOAD &&
                      !handed_back_.contains(guid);
  auto download = std::make_unique<ChromeDownload>(item, is_new);
  ChromeDownload* raw = download.get();
  chrome_downloads_.emplace(guid, std::move(download));
  for (Observer& observer : observers_) {
    observer.OnDownloadAdded(raw);
  }
}

void DownloadService::OnManagerGoingDown(content::DownloadManager* manager) {
  chrome_downloads_.clear();
  notifier_.reset();
}

void DownloadService::OnDownloadCreated(content::DownloadManager* manager,
                                        download::DownloadItem* item) {
  AddChromeDownload(item);
  MaybeHandOff(item);
}

void DownloadService::OnDownloadUpdated(content::DownloadManager* manager,
                                        download::DownloadItem* item) {
  auto it = chrome_downloads_.find(item->GetGuid());
  if (it == chrome_downloads_.end()) {
    return;
  }
  for (Observer& observer : observers_) {
    observer.OnDownloadUpdated(it->second.get());
  }
  MaybeHandOff(item);
}

void DownloadService::OnDownloadRemoved(content::DownloadManager* manager,
                                        download::DownloadItem* item) {
  auto it = chrome_downloads_.find(item->GetGuid());
  if (it == chrome_downloads_.end()) {
    return;
  }
  const std::string guid = it->first;
  std::unique_ptr<ChromeDownload> removed = std::move(it->second);
  chrome_downloads_.erase(it);
  for (Observer& observer : observers_) {
    observer.OnDownloadRemoved(guid);
  }
}

bool DownloadService::ShouldHandOff(const download::DownloadItem& item) const {
  if (handed_back_.contains(item.GetGuid()) ||
      item.GetDownloadCreationType() !=
          download::DownloadItem::TYPE_ACTIVE_DOWNLOAD ||
      item.IsTransient() || item.IsTemporary() ||
      item.IsSavePackageDownload() || item.IsDangerous() || item.IsPaused() ||
      !IsSafeDangerType(item.GetDangerType())) {
    return false;
  }
  // 扩展发起的要在 chrome.downloads 里看得到，留给 Chrome。
  switch (item.GetDownloadSource()) {
    case download::DownloadSource::EXTENSION_API:
    case download::DownloadSource::EXTENSION_INSTALLER:
    case download::DownloadSource::INTERNAL_API:
    case download::DownloadSource::OFFLINE_PAGE:
      return false;
    default:
      break;
  }
  // 已经下完了（文件很小），不用再下一遍。
  if (!item.GetOriginalUrl().SchemeIsHTTPOrHTTPS() ||
      item.GetReceivedBytes() >= item.GetTotalBytes()) {
    return false;
  }
  return CanHandOff(item.GetURL(), item.GetResponseHeaders().get(),
                    item.GetTotalBytes());
}

// 定下了文件名（问过存到哪里）：能分段的交给 Starry。
void DownloadService::MaybeHandOff(download::DownloadItem* item) {
  if (shut_down_ || item->GetState() != download::DownloadItem::IN_PROGRESS ||
      item->GetTargetFilePath().empty()) {
    return;
  }
  const std::string guid = item->GetGuid();
  if (!considered_.insert(guid).second) {
    return;
  }
  if (!ShouldHandOff(*item)) {
    VLOG(1) << "[starry] download stays with Chrome " << item->GetURL().spec()
            << " danger=" << item->GetDangerType()
            << " source=" << static_cast<int>(item->GetDownloadSource())
            << " total=" << item->GetTotalBytes()
            << " received=" << item->GetReceivedBytes() << " status="
            << (item->GetResponseHeaders()
                    ? item->GetResponseHeaders()->response_code()
                    : 0)
            << " ranges="
            << (item->GetResponseHeaders()
                    ? item->GetResponseHeaders()
                          ->GetNormalizedHeader("Accept-Ranges")
                          .value_or("")
                    : "");
    return;
  }
  StarryDownload::Params params;
  params.guid = guid;
  params.url = item->GetOriginalUrl();
  params.final_url = item->GetURL();
  params.referrer = item->GetReferrerUrl();
  params.initiator = item->GetRequestInitiator();
  params.target = UniqueTarget(item->GetTargetFilePath());
  params.mime_type = item->GetMimeType();
  params.total_bytes = item->GetTotalBytes();
  params.etag = StrongETag(item->GetETag());
  params.last_modified = item->GetLastModifiedTime();
  params.start_time = item->GetStartTime();
  if (content::WebContents* contents =
          content::DownloadItemUtils::GetWebContents(item)) {
    params.web_contents = contents->GetWeakPtr();
  }
  VLOG(1) << "[starry] download handed off " << params.url.spec() << " -> "
          << params.target;
  auto download = std::make_unique<StarryDownload>(this, params);
  StarryDownload* raw = download.get();
  handed_off_.insert(guid);
  chrome_downloads_.erase(guid);
  starry_downloads_.push_back(std::move(download));
  // 扩展看到的还是同一个下载：沿用 Chrome 那一项的编号，并迁移扩展事件的状态。
  raw->item()->set_id(item->GetId());
  ReplaceForExtensions(item, raw->item());
  ItemStarted(profile_, raw->item(), /*announce=*/false);

  // Chrome 那一项取消（删掉它写了一半的文件）、从记录里去掉。不在它自己的通知里做。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<DownloadService> service, const std::string& guid) {
            content::DownloadManager* manager =
                service && service->notifier_ ? service->notifier_->GetManager()
                                              : nullptr;
            download::DownloadItem* item =
                manager ? manager->GetDownloadByGuid(guid) : nullptr;
            if (item) {
              item->Cancel(/*user_cancel=*/true);
              item->Remove();
            }
          },
          weak_factory_.GetWeakPtr(), guid));
  NotifyUpdated(raw, /*notable=*/true);
  Pump();
}

// 别的 Starry 下载占着同一个文件名时另起名字（「名字 (1).扩展名」）。Chrome 的下载和磁盘
// 上后来有了的同名文件，完成时改名再看（StarryDownload::MoveIntoPlace）。
base::FilePath DownloadService::UniqueTarget(const base::FilePath& path) const {
  const auto taken = [this](const base::FilePath& candidate) {
    return std::ranges::any_of(
        starry_downloads_, [&](const std::unique_ptr<StarryDownload>& other) {
          return other->target() == candidate &&
                 other->status() != StarryDownload::Status::kFinished &&
                 other->status() != StarryDownload::Status::kCancelled;
        });
  };
  base::FilePath candidate = path;
  for (int i = 1; taken(candidate) && i < 100; ++i) {
    candidate = path.InsertBeforeExtensionASCII(base::StringPrintf(" (%d)", i));
  }
  return candidate;
}

std::unique_ptr<StarryDownload> DownloadService::TakeStarry(
    StarryDownload* download) {
  auto it = std::ranges::find_if(
      starry_downloads_, [download](const std::unique_ptr<StarryDownload>& d) {
        return d.get() == download;
      });
  if (it == starry_downloads_.end()) {
    return nullptr;
  }
  std::unique_ptr<StarryDownload> owned = std::move(*it);
  starry_downloads_.erase(it);
  return owned;
}

// ---- StarryDownload 调的 ----

void DownloadService::Pump() {
  if (shut_down_) {
    return;
  }
  const int limit = std::clamp(
      prefs::PrefsFor(profile_)->GetInteger(prefs::kDownloadMaxConcurrent), 1,
      kMaxConcurrent);
  int running = 0;
  std::vector<StarryDownload*> queued;
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (download->status() == StarryDownload::Status::kRunning) {
      ++running;
    } else if (download->status() == StarryDownload::Status::kQueued) {
      queued.push_back(download.get());
    }
  }
  std::ranges::stable_sort(queued, [](StarryDownload* a, StarryDownload* b) {
    return a->StartTime() < b->StartTime();
  });
  for (StarryDownload* download : queued) {
    if (running >= limit) {
      break;
    }
    ++running;
    download->Launch();
  }
  UpdateTicker();
}

void DownloadService::NotifyUpdated(Download* download, bool notable) {
  for (Observer& observer : observers_) {
    observer.OnDownloadUpdated(download);
  }
  if (download->IsStarry()) {
    StarryDownloadItem* item = static_cast<StarryDownload*>(download)->item();
    if (item->has_id()) {
      // 用户资料关闭时（无痕的取消掉）只更新程序坞图标，不再发扩展事件。
      ItemUpdated(profile_, item, /*extensions=*/!shut_down_);
    }
  }
  if (notable) {
    ScheduleSave();
    UpdateTicker();
    UpdateKeepAlive();
  }
}

void DownloadService::ItemGone(StarryDownload* download) {
  download->item()->NotifyRemoved();
  if (download->item()->has_id() && !shut_down_) {
    ItemRemoved(profile_, download->item());
  }
}

void DownloadService::RemoveStarryDownload(StarryDownload* download) {
  std::unique_ptr<StarryDownload> owned = TakeStarry(download);
  if (!owned) {
    return;
  }
  const std::string guid = owned->Guid();
  ItemGone(owned.get());
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             std::move(owned));
  for (Observer& observer : observers_) {
    observer.OnDownloadRemoved(guid);
  }
  ScheduleSave();
  Pump();
  UpdateKeepAlive();
}

void DownloadService::HandBack(StarryDownload* download) {
  std::unique_ptr<StarryDownload> owned = TakeStarry(download);
  if (!owned) {
    return;
  }
  const std::string guid = owned->Guid();
  const GURL url = owned->Url();
  const GURL referrer = owned->referrer();
  const std::optional<url::Origin> initiator = owned->initiator();
  content::WebContents* contents = owned->GetWebContents();
  VLOG(1) << "[starry] download handed back to Chrome " << url.spec();
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::GetDeleteFileCallback(owned->partial_path()));
  // 扩展看到的是去掉了一项，Chrome 重新开始的是新的一项。
  ItemGone(owned.get());
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             std::move(owned));
  handed_off_.erase(guid);
  handed_back_.insert(guid);
  for (Observer& observer : observers_) {
    observer.OnDownloadRemoved(guid);
  }
  ScheduleSave();
  Pump();
  UpdateKeepAlive();

  content::DownloadManager* manager =
      notifier_ ? notifier_->GetManager() : nullptr;
  if (!manager) {
    return;
  }
  std::unique_ptr<download::DownloadUrlParameters> params =
      contents ? content::DownloadRequestUtils::
                     CreateDownloadForWebContentsMainFrame(contents, url,
                                                           kStartAnnotation)
               : std::make_unique<download::DownloadUrlParameters>(
                     url, kStartAnnotation);
  params->set_guid(guid);
  if (referrer.is_valid()) {
    params->set_referrer(referrer);
    params->set_referrer_policy(
        net::ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN);
  }
  if (initiator) {
    params->set_initiator(initiator);
  }
  manager->DownloadUrl(std::move(params));
}

int DownloadService::EffectiveConnections() const {
  PrefService* prefs = prefs::PrefsFor(profile_);
  if (!prefs->GetBoolean(prefs::kDownloadMultiConnection)) {
    return 1;
  }
  int connections = std::clamp(prefs->GetInteger(prefs::kDownloadConnections),
                               1, kMaxConnections);
  const int limit_kb = prefs->GetInteger(prefs::kDownloadSpeedLimit);
  if (limit_kb > 0) {
    connections = std::min(connections, std::max(1, limit_kb / 512));
  }
  return connections;
}

bool DownloadService::RetriesAutomatically() const {
  return prefs::PrefsFor(profile_)->GetBoolean(prefs::kDownloadAutoRetry);
}

std::unique_ptr<network::PendingSharedURLLoaderFactory>
DownloadService::CreateLoaderFactory() {
  return profile_->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess()
      ->Clone();
}

mojo::PendingRemote<network::mojom::CookieManager>
DownloadService::CreateCookieManager() {
  mojo::PendingRemote<network::mojom::CookieManager> remote;
  profile_->GetDefaultStoragePartition()->GetNetworkContext()->GetCookieManager(
      remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

bool DownloadService::CookiesAllowed(const GURL& url) const {
  scoped_refptr<content_settings::CookieSettings> settings =
      CookieSettingsFactory::GetForProfile(profile_);
  if (!settings) {
    return true;
  }
  const url::Origin origin = url::Origin::Create(url);
  return settings->IsFullCookieAccessAllowed(
      url, net::SiteForCookies::FromUrl(url), origin,
      net::CookieSettingOverrides(), std::nullopt);
}

download::QuarantineConnectionCallback DownloadService::QuarantineConnection() {
  DownloadCoreService* core =
      DownloadCoreServiceFactory::GetForBrowserContext(profile_);
  ChromeDownloadManagerDelegate* delegate =
      core ? core->GetDownloadManagerDelegate() : nullptr;
  return delegate ? delegate->GetQuarantineConnectionCallback()
                  : download::QuarantineConnectionCallback();
}

void DownloadService::ScheduleSave() {
  if (writer_ && loaded_ && !shut_down_) {
    writer_->ScheduleWrite(this);
  }
}

// ---- 设置、进度刷新 ----

void DownloadService::OnPrefsChanged() {
  DownloadThrottle::Get().SetLimit(
      static_cast<int64_t>(std::max(0, prefs::PrefsFor(profile_)->GetInteger(
                                           prefs::kDownloadSpeedLimit))) *
      1024);
  // 连接数：多了马上补上，少了等连接下完手上的段再收。
  const int connections = EffectiveConnections();
  if (connections != effective_connections_) {
    effective_connections_ = connections;
    for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
      if (download->status() == StarryDownload::Status::kRunning) {
        download->SetMaxConnections(connections);
      }
    }
  }
  Pump();
}

void DownloadService::UpdateTicker() {
  const bool running = std::ranges::any_of(
      starry_downloads_, [](const std::unique_ptr<StarryDownload>& download) {
        return download->status() == StarryDownload::Status::kRunning;
      });
  if (running && !ticker_.IsRunning() && !shut_down_) {
    ticker_.Start(
        FROM_HERE, kTickInterval,
        base::BindRepeating(&DownloadService::Tick, base::Unretained(this)));
  } else if (!running && ticker_.IsRunning()) {
    ticker_.Stop();
  }
}

void DownloadService::Tick() {
  const base::TimeTicks now = base::TimeTicks::Now();
  const bool checkpoint =
      writer_ && now - last_checkpoint_ >= kCheckpointInterval;
  if (checkpoint) {
    last_checkpoint_ = now;
  }
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (download->status() != StarryDownload::Status::kRunning) {
      continue;
    }
    download->Tick(now);
    if (checkpoint) {
      download->Checkpoint();
    }
  }
}

// ---- 存盘 ----

std::optional<std::string> DownloadService::SerializeData() {
  base::ListValue items;
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    items.Append(download->ToRecord());
  }
  base::DictValue root;
  root.Set("version", 1);
  root.Set("items", std::move(items));
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          root, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json)) {
    return std::nullopt;
  }
  return json;
}

// static
std::vector<DownloadService::Loaded> DownloadService::ReadRecords(
    const base::FilePath& path) {
  std::vector<Loaded> loaded;
  std::string text;
  if (!base::ReadFileToString(path, &text)) {
    return loaded;
  }
  std::optional<base::Value> root =
      base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  const base::ListValue* items =
      root && root->is_dict() ? root->GetDict().FindList("items") : nullptr;
  if (!items) {
    return loaded;
  }
  for (const base::Value& item : *items) {
    if (!item.is_dict()) {
      continue;
    }
    Loaded entry;
    entry.record = item.GetDict().Clone();
    if (const std::string* target = entry.record.FindString("target")) {
      const base::FilePath file = base::FilePath::FromUTF8Unsafe(*target);
      entry.partial_exists =
          !file.empty() &&
          base::PathExists(StarryDownload::PartialPathFor(file));
      entry.target_exists = !file.empty() && base::PathExists(file);
    }
    loaded.push_back(std::move(entry));
  }
  return loaded;
}

void DownloadService::OnLoaded(std::vector<Loaded> loaded) {
  if (shut_down_) {
    return;
  }
  for (const Loaded& entry : loaded) {
    std::unique_ptr<StarryDownload> download = StarryDownload::FromRecord(
        this, entry.record, entry.partial_exists, entry.target_exists);
    if (!download || Find(download->Guid())) {
      continue;
    }
    StarryDownload* raw = download.get();
    handed_off_.insert(raw->Guid());
    considered_.insert(raw->Guid());
    starry_downloads_.push_back(std::move(download));
    for (Observer& observer : observers_) {
      observer.OnDownloadAdded(raw);
    }
    // 给扩展看的编号：向 DownloadManager 要（和 Chrome 的不重复）。
    if (content::DownloadManager* manager =
            notifier_ ? notifier_->GetManager() : nullptr) {
      manager->GetNextId(base::BindOnce(
          &DownloadService::OnItemId, weak_factory_.GetWeakPtr(), raw->Guid()));
    }
  }
  loaded_ = true;
  UpdateKeepAlive();
  if (prefs::PrefsFor(profile_)->GetBoolean(prefs::kDownloadResumeOnLaunch)) {
    // 等扩展载入（扩展设的代理生效）再接着下。
    extensions::ExtensionSystem::Get(profile_)->ready().Post(
        FROM_HERE, base::BindOnce(&DownloadService::ResumeInterrupted,
                                  weak_factory_.GetWeakPtr()));
  }
}

void DownloadService::OnItemId(const std::string& guid, uint32_t id) {
  StarryDownload* download = FindStarry(guid);
  if (shut_down_ || !download || download->item()->has_id() ||
      id == download::DownloadItem::kInvalidId) {
    return;
  }
  download->item()->set_id(id);
  // 同 Chrome 启动时读回下载记录：给扩展发 onCreated。
  ItemStarted(profile_, download->item(), /*announce=*/true);
}

// 只算在下载、排队的：暂停的（含启动时读回来还没继续的）进度都存着，用户资料关了
// 也不丢。
void DownloadService::UpdateKeepAlive() {
  const bool active =
      !shut_down_ && !profile_->IsOffTheRecord() &&
      std::ranges::any_of(
          starry_downloads_,
          [](const std::unique_ptr<StarryDownload>& download) {
            return download->status() == StarryDownload::Status::kRunning ||
                   download->status() == StarryDownload::Status::kQueued;
          });
  if (active && !keep_alive_) {
    keep_alive_ = std::make_unique<ScopedProfileKeepAlive>(
        profile_, ProfileKeepAliveOrigin::kDownloadInProgress);
  } else if (!active) {
    keep_alive_.reset();
  }
}

// 接着下上次退出时还在进行的下载。用户自己暂停的不动。
void DownloadService::ResumeInterrupted() {
  std::vector<StarryDownload*> interrupted;
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (download->status() == StarryDownload::Status::kPaused &&
        download->interrupted()) {
      interrupted.push_back(download.get());
    }
  }
  std::ranges::stable_sort(interrupted,
                           [](StarryDownload* a, StarryDownload* b) {
                             return a->StartTime() < b->StartTime();
                           });
  for (StarryDownload* download : interrupted) {
    download->Resume();
  }
}

void DownloadService::Shutdown() {
  if (shut_down_) {
    return;
  }
  for (Observer& observer : observers_) {
    observer.OnDownloadServiceShutdown();
  }
  shut_down_ = true;
  notifications_.reset();
  keep_alive_.reset();
  ticker_.Stop();
  // 退出时还在下载的记成「退出时还在下载」，下次启动接着下。存的是最近一次刷到磁盘时的
  // 进度，之后写的那一点下次重新下。无痕的不存：没下完的取消、删掉临时文件。
  for (const std::unique_ptr<StarryDownload>& download : starry_downloads_) {
    if (profile_->IsOffTheRecord()) {
      download->Cancel();
    } else {
      download->MarkInterrupted();
    }
  }
  if (writer_ && loaded_) {
    if (std::optional<std::string> data = SerializeData()) {
      writer_->WriteNow(std::move(*data));
    }
  }
  prefs_.RemoveAll();
  notifier_.reset();
  chrome_downloads_.clear();
  starry_downloads_.clear();
  std::erase(RunningServices(), this);
}

}  // namespace starry
