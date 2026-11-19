// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/starry_download_item.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "net/http/http_response_headers.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/downloads/starry_download.h"

namespace starry {

namespace {

// 认出 Starry 的一项（From）。
const char kStarryItemKey[] = "StarryDownloadItem";

class Marker : public base::SupportsUserData::Data {};

// 失败的原因换成 Chrome 的（扩展看到的 error）。
download::DownloadInterruptReason InterruptReasonFor(
    const DownloadError& error) {
  switch (error.kind) {
    case DownloadError::Kind::kNone:
      return download::DOWNLOAD_INTERRUPT_REASON_NONE;
    case DownloadError::Kind::kCreateFailed:
    case DownloadError::Kind::kWriteFailed:
    case DownloadError::Kind::kSaveFailed:
    case DownloadError::Kind::kNoDestination:
      return download::DOWNLOAD_INTERRUPT_REASON_FILE_FAILED;
    case DownloadError::Kind::kDiskFull:
    case DownloadError::Kind::kDiskSpace:
      return download::DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE;
    case DownloadError::Kind::kInvalidResponse:
      return download::DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT;
    case DownloadError::Kind::kConnectionClosed:
      return download::DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED;
    case DownloadError::Kind::kHttpStatus:
      switch (error.status) {
        case 401:
        case 407:
          return download::DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED;
        case 403:
          return download::DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN;
        case 404:
        case 410:
          return download::DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT;
        case 416:
          return download::DOWNLOAD_INTERRUPT_REASON_SERVER_NO_RANGE;
        default:
          return download::DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED;
      }
    case DownloadError::Kind::kFileChanged:
      return download::DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED;
    case DownloadError::Kind::kInterrupted:
      return download::DOWNLOAD_INTERRUPT_REASON_CRASH;
  }
}

const std::string& EmptyString() {
  static const base::NoDestructor<std::string> empty;
  return *empty;
}

const GURL& EmptyUrl() {
  static const base::NoDestructor<GURL> empty;
  return *empty;
}

const base::FilePath& EmptyPath() {
  static const base::NoDestructor<base::FilePath> empty;
  return *empty;
}

}  // namespace

StarryDownloadItem::StarryDownloadItem(StarryDownload* download)
    : download_(download) {
  SetUserData(kStarryItemKey, std::make_unique<Marker>());
}

StarryDownloadItem::~StarryDownloadItem() {
  for (Observer& observer : observers_) {
    observer.OnDownloadDestroyed(this);
  }
}

// static
StarryDownloadItem* StarryDownloadItem::From(
    const download::DownloadItem* item) {
  if (!item || !item->GetUserData(kStarryItemKey)) {
    return nullptr;
  }
  return static_cast<StarryDownloadItem*>(
      const_cast<download::DownloadItem*>(item));
}

void StarryDownloadItem::Refresh() {
  url_ = download_->final_url().is_valid() ? download_->final_url()
                                           : download_->Url();
  url_chain_ = {download_->Url()};
  if (url_ != download_->Url()) {
    url_chain_.push_back(url_);
  }
  target_path_ = download_->target();
  // 现在在磁盘上的文件：下完的是文件本身，没下完的是临时文件，取消了没有文件。
  switch (download_->status()) {
    case StarryDownload::Status::kFinished:
      full_path_ = target_path_;
      break;
    case StarryDownload::Status::kCancelled:
      full_path_.clear();
      break;
    default:
      full_path_ = download_->partial_path();
      break;
  }
  UpdateObservers();
}

void StarryDownloadItem::NotifyRemoved() {
  for (Observer& observer : observers_) {
    observer.OnDownloadRemoved(this);
  }
}

// ---- download::DownloadItem ----

void StarryDownloadItem::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void StarryDownloadItem::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void StarryDownloadItem::UpdateObservers() {
  for (Observer& observer : observers_) {
    observer.OnDownloadUpdated(this);
  }
}

// Starry 的下载没有危险、不安全的（可能有危险的留给了 Chrome）。
void StarryDownloadItem::ValidateDangerousDownload() {}
void StarryDownloadItem::ValidateInsecureDownload() {}
void StarryDownloadItem::ConfirmNonDangerousDownload() {}

void StarryDownloadItem::CopyDownload(AcquireFileCallback callback) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), base::FilePath()));
}

void StarryDownloadItem::Pause() {
  download_->Pause();
}

void StarryDownloadItem::Resume(bool user_resume) {
  download_->Resume();
}

void StarryDownloadItem::Cancel(bool user_cancel) {
  download_->Cancel();
}

void StarryDownloadItem::Remove() {
  download_->Remove();
}

void StarryDownloadItem::OpenDownload() {
  opened_ = true;
  download_->Open();
  for (Observer& observer : observers_) {
    observer.OnDownloadOpened(this);
  }
}

void StarryDownloadItem::ShowDownloadInShell() {
  const base::FilePath path = download_->RevealPath();
  if (!path.empty()) {
    platform_util::ShowItemInFolder(download_->service()->profile(), path);
  }
}

void StarryDownloadItem::Rename(const base::FilePath& new_name,
                                RenameDownloadCallback callback) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback),
                                DownloadRenameResult::FAILURE_UNAVAILABLE));
}

uint32_t StarryDownloadItem::GetId() const {
  return id_;
}

const std::string& StarryDownloadItem::GetGuid() const {
  return download_->Guid();
}

download::DownloadItem::DownloadState StarryDownloadItem::GetState() const {
  switch (download_->status()) {
    case StarryDownload::Status::kQueued:
    case StarryDownload::Status::kRunning:
    case StarryDownload::Status::kPaused:
      return IN_PROGRESS;
    case StarryDownload::Status::kFinished:
      return COMPLETE;
    case StarryDownload::Status::kFailed:
      return INTERRUPTED;
    case StarryDownload::Status::kCancelled:
      return CANCELLED;
  }
}

download::DownloadInterruptReason StarryDownloadItem::GetLastReason() const {
  switch (download_->status()) {
    case StarryDownload::Status::kFailed:
      return InterruptReasonFor(download_->error());
    case StarryDownload::Status::kCancelled:
      return download::DOWNLOAD_INTERRUPT_REASON_USER_CANCELED;
    default:
      return download::DOWNLOAD_INTERRUPT_REASON_NONE;
  }
}

bool StarryDownloadItem::IsPaused() const {
  return download_->status() == StarryDownload::Status::kPaused;
}

bool StarryDownloadItem::AllowMetered() const {
  return true;
}

bool StarryDownloadItem::IsTemporary() const {
  return false;
}

// 同 Chrome：暂停的、能接着下的失败才算（排队、下载中的不算）。暂停了不支持分段的继续时
// 从头下。
bool StarryDownloadItem::CanResume() const {
  switch (download_->status()) {
    case StarryDownload::Status::kPaused:
      return true;
    case StarryDownload::Status::kFailed:
      return download_->CanResume();
    default:
      return false;
  }
}

bool StarryDownloadItem::IsDone() const {
  switch (GetState()) {
    case COMPLETE:
    case CANCELLED:
      return true;
    case INTERRUPTED:
      return !CanResume();
    default:
      return false;
  }
}

int64_t StarryDownloadItem::GetBytesWasted() const {
  return 0;
}

int32_t StarryDownloadItem::GetAutoResumeCount() const {
  return 0;
}

const GURL& StarryDownloadItem::GetURL() const {
  return url_;
}

const std::vector<GURL>& StarryDownloadItem::GetUrlChain() const {
  return url_chain_;
}

bool StarryDownloadItem::IsUrlTruncated() const {
  return false;
}

const GURL& StarryDownloadItem::GetOriginalUrl() const {
  return download_->url();
}

const GURL& StarryDownloadItem::GetReferrerUrl() const {
  return download_->referrer();
}

const std::string& StarryDownloadItem::GetSerializedEmbedderDownloadData()
    const {
  return EmptyString();
}

const GURL& StarryDownloadItem::GetTabUrl() const {
  return EmptyUrl();
}

const GURL& StarryDownloadItem::GetTabReferrerUrl() const {
  return EmptyUrl();
}

const std::optional<url::Origin>& StarryDownloadItem::GetRequestInitiator()
    const {
  return download_->initiator();
}

std::string StarryDownloadItem::GetSuggestedFilename() const {
  return std::string();
}

const scoped_refptr<const net::HttpResponseHeaders>&
StarryDownloadItem::GetResponseHeaders() const {
  static const base::NoDestructor<scoped_refptr<const net::HttpResponseHeaders>>
      none;
  return *none;
}

std::string StarryDownloadItem::GetContentDisposition() const {
  return std::string();
}

std::string StarryDownloadItem::GetMimeType() const {
  return download_->mime_type();
}

std::string StarryDownloadItem::GetOriginalMimeType() const {
  return download_->mime_type();
}

std::string StarryDownloadItem::GetRemoteAddress() const {
  return std::string();
}

bool StarryDownloadItem::HasUserGesture() const {
  return false;
}

ui::PageTransition StarryDownloadItem::GetTransitionType() const {
  return ui::PAGE_TRANSITION_LINK;
}

const std::string& StarryDownloadItem::GetLastModifiedTime() const {
  return download_->last_modified();
}

const std::string& StarryDownloadItem::GetETag() const {
  return download_->etag();
}

bool StarryDownloadItem::IsSavePackageDownload() const {
  return false;
}

download::DownloadSource StarryDownloadItem::GetDownloadSource() const {
  return download::DownloadSource::UNKNOWN;
}

::network::mojom::CredentialsMode StarryDownloadItem::GetCredentialsMode()
    const {
  return ::network::mojom::CredentialsMode::kInclude;
}

::network::mojom::RequestMode StarryDownloadItem::GetFetchRequestMode() const {
  return ::network::mojom::RequestMode::kNoCors;
}

const std::optional<net::IsolationInfo>& StarryDownloadItem::GetIsolationInfo()
    const {
  static const base::NoDestructor<std::optional<net::IsolationInfo>> none;
  return *none;
}

const base::FilePath& StarryDownloadItem::GetFullPath() const {
  return full_path_;
}

const base::FilePath& StarryDownloadItem::GetTargetFilePath() const {
  return target_path_;
}

const base::FilePath& StarryDownloadItem::GetForcedFilePath() const {
  return EmptyPath();
}

base::FilePath StarryDownloadItem::GetTemporaryFilePath() const {
  return download_->partial_path();
}

base::FilePath StarryDownloadItem::GetFileNameToReportUser() const {
  return target_path_.BaseName();
}

download::DownloadItem::TargetDisposition
StarryDownloadItem::GetTargetDisposition() const {
  return TARGET_DISPOSITION_OVERWRITE;
}

const std::string& StarryDownloadItem::GetHash() const {
  return EmptyString();
}

bool StarryDownloadItem::GetFileExternallyRemoved() const {
  return download_->FileMissing();
}

void StarryDownloadItem::DeleteFile(base::OnceCallback<void(bool)> callback) {
  download_->DeleteFile(std::move(callback));
}

bool StarryDownloadItem::IsDangerous() const {
  return false;
}

bool StarryDownloadItem::IsInsecure() const {
  return false;
}

bool StarryDownloadItem::IsUserConfirmed() const {
  return false;
}

download::DownloadDangerType StarryDownloadItem::GetDangerType() const {
  return download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS;
}

download::DownloadItem::InsecureDownloadStatus
StarryDownloadItem::GetInsecureDownloadStatus() const {
  return InsecureDownloadStatus::SAFE;
}

download::DownloadFile* StarryDownloadItem::GetDownloadFile() {
  return nullptr;
}

download::DownloadItemRenameHandler* StarryDownloadItem::GetRenameHandler() {
  return nullptr;
}

bool StarryDownloadItem::TimeRemaining(base::TimeDelta* remaining) const {
  const int64_t speed = download_->Speed();
  const int64_t total = download_->TotalBytes();
  if (download_->status() != StarryDownload::Status::kRunning || speed <= 0 ||
      total <= 0) {
    return false;
  }
  *remaining = base::Seconds(static_cast<double>(std::max<int64_t>(
                                 0, total - download_->ReceivedBytes())) /
                             static_cast<double>(speed));
  return true;
}

int64_t StarryDownloadItem::CurrentSpeed() const {
  return download_->Speed();
}

int StarryDownloadItem::PercentComplete() const {
  const int64_t total = download_->TotalBytes();
  if (total <= 0) {
    return -1;
  }
  return static_cast<int>(download_->ReceivedBytes() * 100 / total);
}

// 数据下完了（正在改回原名也算）：访达里的进度这时就要撤掉（见 DownloadStatusUpdater）。
bool StarryDownloadItem::AllDataSaved() const {
  return download_->status() == StarryDownload::Status::kFinished ||
         download_->finishing();
}

int64_t StarryDownloadItem::GetTotalBytes() const {
  return std::max<int64_t>(0, download_->TotalBytes());
}

int64_t StarryDownloadItem::GetReceivedBytes() const {
  return download_->ReceivedBytes();
}

const std::vector<download::DownloadItem::ReceivedSlice>&
StarryDownloadItem::GetReceivedSlices() const {
  static const base::NoDestructor<std::vector<ReceivedSlice>> none;
  return *none;
}

int64_t StarryDownloadItem::GetUploadedBytes() const {
  return 0;
}

base::Time StarryDownloadItem::GetStartTime() const {
  return download_->StartTime();
}

base::Time StarryDownloadItem::GetEndTime() const {
  return download_->EndTime();
}

bool StarryDownloadItem::CanShowInFolder() {
  return !download_->RevealPath().empty();
}

bool StarryDownloadItem::CanOpenDownload() {
  return true;
}

bool StarryDownloadItem::ShouldOpenFileBasedOnExtension() {
  return false;
}

bool StarryDownloadItem::ShouldOpenFileByPolicyBasedOnExtension() {
  return false;
}

bool StarryDownloadItem::GetOpenWhenComplete() const {
  return false;
}

bool StarryDownloadItem::GetAutoOpened() {
  return false;
}

bool StarryDownloadItem::GetOpened() const {
  return opened_;
}

base::Time StarryDownloadItem::GetLastAccessTime() const {
  return last_access_time_;
}

bool StarryDownloadItem::IsTransient() const {
  return false;
}

bool StarryDownloadItem::RequireSafetyChecks() const {
  return false;
}

bool StarryDownloadItem::IsParallelDownload() const {
  return false;
}

download::DownloadItem::DownloadCreationType
StarryDownloadItem::GetDownloadCreationType() const {
  return download_->IsNew() ? TYPE_ACTIVE_DOWNLOAD : TYPE_HISTORY_IMPORT;
}

void StarryDownloadItem::OnContentCheckCompleted(
    download::DownloadDangerType danger_type,
    download::DownloadInterruptReason reason) {}

void StarryDownloadItem::OnAsyncScanningCompleted(
    download::DownloadDangerType danger_type) {}

void StarryDownloadItem::SetOpenWhenComplete(bool open) {}

void StarryDownloadItem::SetOpened(bool opened) {
  opened_ = opened;
}

void StarryDownloadItem::SetLastAccessTime(base::Time last_access_time) {
  last_access_time_ = last_access_time;
}

void StarryDownloadItem::SetDisplayName(const base::FilePath& name) {}

std::string StarryDownloadItem::DebugString(bool verbose) const {
  return base::StringPrintf("{ starry id = %u guid = %s state = %d }", id_,
                            download_->Guid().c_str(),
                            static_cast<int>(GetState()));
}

void StarryDownloadItem::SimulateErrorForTesting(
    download::DownloadInterruptReason reason) {}

}  // namespace starry
