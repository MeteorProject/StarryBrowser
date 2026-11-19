// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/download.h"

#include "base/no_destructor.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_item_utils.h"
#include "starry/browser/downloads/download_info.h"
#include "starry/browser/l10n/l10n.h"

namespace starry {

const char* StateName(DownloadState state) {
  switch (state) {
    case DownloadState::kQueued:
      return "queued";
    case DownloadState::kRunning:
      return "running";
    case DownloadState::kPaused:
      return "paused";
    case DownloadState::kComplete:
      return "complete";
    case DownloadState::kCancelled:
      return "cancelled";
    case DownloadState::kInterrupted:
      return "interrupted";
  }
}

// ---- Download 的默认实现（Chrome 的下载没有这些） ----

bool Download::IsStarry() const {
  return false;
}

RangeSupport Download::GetRangeSupport() const {
  return RangeSupport::kUnknown;
}

const std::vector<DownloadSegment>& Download::Segments() const {
  static const base::NoDestructor<std::vector<DownloadSegment>> empty;
  return *empty;
}

int Download::Connections() const {
  return 0;
}

int Download::MaxConnections() const {
  return 0;
}

std::optional<int> Download::RetryAttempt() const {
  return std::nullopt;
}

bool Download::IsConnecting() const {
  return false;
}

content::WebContents* Download::GetWebContents() const {
  return nullptr;
}

void Download::KeepDangerous() {}

download::DownloadItem* Download::chrome_item() const {
  return nullptr;
}

// ---- ChromeDownload ----

ChromeDownload::ChromeDownload(download::DownloadItem* item, bool is_new)
    : item_(item), is_new_(is_new) {}

ChromeDownload::~ChromeDownload() = default;

const std::string& ChromeDownload::Guid() const {
  return item_->GetGuid();
}

DownloadState ChromeDownload::State() const {
  switch (item_->GetState()) {
    case download::DownloadItem::IN_PROGRESS:
      return item_->IsPaused() ? DownloadState::kPaused
                               : DownloadState::kRunning;
    case download::DownloadItem::COMPLETE:
      return DownloadState::kComplete;
    case download::DownloadItem::CANCELLED:
      return DownloadState::kCancelled;
    case download::DownloadItem::INTERRUPTED:
    case download::DownloadItem::MAX_DOWNLOAD_STATE:
      return DownloadState::kInterrupted;
  }
}

std::u16string ChromeDownload::Filename() const {
  std::u16string name = item_->GetFileNameToReportUser().LossyDisplayName();
  return name.empty() ? L("downloads.filename.fallback") : name;
}

GURL ChromeDownload::Url() const {
  return item_->GetURL();
}

GURL ChromeDownload::ReferrerUrl() const {
  return item_->GetReferrerUrl();
}

int64_t ChromeDownload::ReceivedBytes() const {
  return item_->GetReceivedBytes();
}

int64_t ChromeDownload::TotalBytes() const {
  return item_->GetTotalBytes();
}

int64_t ChromeDownload::Speed() const {
  return item_->CurrentSpeed();
}

base::Time ChromeDownload::StartTime() const {
  return item_->GetStartTime();
}

base::Time ChromeDownload::EndTime() const {
  return item_->GetEndTime();
}

base::FilePath ChromeDownload::TargetPath() const {
  return item_->GetTargetFilePath();
}

base::FilePath ChromeDownload::RevealPath() const {
  return item_->GetTargetFilePath();
}

bool ChromeDownload::FileMissing() const {
  return item_->GetFileExternallyRemoved();
}

bool ChromeDownload::IsDangerous() const {
  return item_->IsDangerous();
}

bool ChromeDownload::CanResume() const {
  return item_->CanResume();
}

std::u16string ChromeDownload::ErrorText() const {
  return ChromeErrorText(*item_);
}

content::WebContents* ChromeDownload::GetWebContents() const {
  return content::DownloadItemUtils::GetWebContents(item_);
}

bool ChromeDownload::IsNew() const {
  return is_new_;
}

void ChromeDownload::Pause() {
  item_->Pause();
}

void ChromeDownload::Resume() {
  item_->Resume(/*user_resume=*/true);
}

void ChromeDownload::Cancel() {
  item_->Cancel(/*user_cancel=*/true);
}

void ChromeDownload::Remove() {
  // Remove 之后这个对象就没了（DownloadService 收到 OnDownloadRemoved 时删掉它）。
  download::DownloadItem* item = item_;
  if (item->GetState() == download::DownloadItem::IN_PROGRESS) {
    item->Cancel(/*user_cancel=*/true);
  }
  item->Remove();
}

void ChromeDownload::Open() {
  item_->OpenDownload();
}

void ChromeDownload::KeepDangerous() {
  item_->ValidateDangerousDownload();
}

download::DownloadItem* ChromeDownload::chrome_item() const {
  return item_;
}

}  // namespace starry
