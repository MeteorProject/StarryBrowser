// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/starry_download.h"

#include <algorithm>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/public/common/download_file_utils.h"
#include "components/services/quarantine/public/mojom/quarantine.mojom.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/downloads/download_info.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/l10n/l10n.h"

namespace starry {

namespace {

const char* StatusName(StarryDownload::Status status) {
  switch (status) {
    case StarryDownload::Status::kQueued:
      return "queued";
    case StarryDownload::Status::kRunning:
      return "running";
    case StarryDownload::Status::kPaused:
      return "paused";
    case StarryDownload::Status::kFinished:
      return "finished";
    case StarryDownload::Status::kFailed:
      return "failed";
    case StarryDownload::Status::kCancelled:
      return "cancelled";
  }
}

std::optional<StarryDownload::Status> StatusFromName(std::string_view name) {
  for (StarryDownload::Status status :
       {StarryDownload::Status::kQueued, StarryDownload::Status::kRunning,
        StarryDownload::Status::kPaused, StarryDownload::Status::kFinished,
        StarryDownload::Status::kFailed, StarryDownload::Status::kCancelled}) {
    if (name == StatusName(status)) {
      return status;
    }
  }
  return std::nullopt;
}

double TimeValue(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

base::Time TimeFrom(std::optional<double> value) {
  return value && *value > 0
             ? base::Time::FromMillisecondsSinceUnixEpoch(*value)
             : base::Time();
}

int64_t Int64From(const base::DictValue& dict,
                  std::string_view key,
                  int64_t fallback) {
  const std::optional<double> value = dict.FindDouble(key);
  return value ? static_cast<int64_t>(*value) : fallback;
}

std::string StringFrom(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

// 分段存成 [start, end, received]；不知道结尾的存 -1。
base::ListValue SegmentsValue(const std::vector<DownloadSegment>& segments) {
  base::ListValue list;
  for (const DownloadSegment& segment : segments) {
    base::ListValue entry;
    entry.Append(static_cast<double>(segment.start));
    entry.Append(segment.end == DownloadSegment::kOpenEnd
                     ? -1.0
                     : static_cast<double>(segment.end));
    entry.Append(static_cast<double>(segment.received));
    list.Append(std::move(entry));
  }
  return list;
}

std::vector<DownloadSegment> SegmentsFrom(const base::ListValue* list) {
  std::vector<DownloadSegment> segments;
  if (!list) {
    return segments;
  }
  for (const base::Value& entry : *list) {
    if (!entry.is_list() || entry.GetList().size() != 3) {
      return {};
    }
    const base::ListValue& values = entry.GetList();
    if (!values[0].is_double() && !values[0].is_int()) {
      return {};
    }
    DownloadSegment segment;
    segment.start = static_cast<int64_t>(values[0].GetDouble());
    const double end = values[1].GetIfDouble().value_or(-1);
    segment.end =
        end < 0 ? DownloadSegment::kOpenEnd : static_cast<int64_t>(end);
    segment.received =
        static_cast<int64_t>(values[2].GetIfDouble().value_or(0));
    segments.push_back(segment);
  }
  return segments;
}

base::DictValue ErrorValue(const DownloadError& error) {
  base::DictValue dict;
  dict.Set("kind", static_cast<int>(error.kind));
  dict.Set("status", error.status);
  dict.Set("detail", error.detail);
  dict.Set("needed", static_cast<double>(error.needed));
  dict.Set("available", static_cast<double>(error.available));
  return dict;
}

DownloadError ErrorFrom(const base::DictValue* dict) {
  DownloadError error;
  if (!dict) {
    return error;
  }
  const int kind = dict->FindInt("kind").value_or(0);
  if (kind < 0 || kind > static_cast<int>(DownloadError::Kind::kInterrupted)) {
    return error;
  }
  error.kind = static_cast<DownloadError::Kind>(kind);
  error.status = dict->FindInt("status").value_or(0);
  error.detail = StringFrom(*dict, "detail");
  error.needed = Int64From(*dict, "needed", 0);
  error.available = Int64From(*dict, "available", 0);
  return error;
}

// 下好的临时文件改回原名；同名文件已经有了就另起名字（「名字 (1).扩展名」）。权限同 Chrome
// 下好的文件（按 umask，不是临时文件的只有自己能读）。
std::optional<base::FilePath> MoveIntoPlaceOnSequence(
    const base::FilePath& partial,
    const base::FilePath& target) {
  base::FilePath destination = target;
  if (base::PathExists(destination)) {
    destination = base::GetUniquePath(target);
    if (destination.empty()) {
      return std::nullopt;
    }
  }
  if (!download::ReplaceFileWithPermissions(partial, destination)) {
    return std::nullopt;
  }
  return destination;
}

}  // namespace

StarryDownload::Params::Params() = default;
StarryDownload::Params::Params(const Params&) = default;
StarryDownload::Params& StarryDownload::Params::operator=(const Params&) =
    default;
StarryDownload::Params::~Params() = default;

StarryDownload::Saved::Saved() = default;
StarryDownload::Saved::Saved(const Saved&) = default;
StarryDownload::Saved& StarryDownload::Saved::operator=(const Saved&) = default;
StarryDownload::Saved::~Saved() = default;

StarryDownload::StarryDownload(DownloadService* service, bool is_new)
    : service_(service),
      is_new_(is_new),
      task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
      item_(std::make_unique<StarryDownloadItem>(this)) {}

StarryDownload::StarryDownload(DownloadService* service, const Params& params)
    : StarryDownload(service, /*is_new=*/true) {
  guid_ = params.guid;
  url_ = params.url;
  final_url_ = params.final_url;
  referrer_ = params.referrer;
  initiator_ = params.initiator;
  target_ = params.target;
  mime_type_ = params.mime_type;
  total_bytes_ = params.total_bytes;
  etag_ = params.etag;
  last_modified_ = params.last_modified;
  start_time_ =
      params.start_time.is_null() ? base::Time::Now() : params.start_time;
  web_contents_ = params.web_contents;
  status_ = Status::kQueued;
  connecting_ = true;
  item_->Refresh();
}

// static
std::unique_ptr<StarryDownload> StarryDownload::FromRecord(
    DownloadService* service,
    const base::DictValue& record,
    bool partial_exists,
    bool target_exists) {
  const std::string guid = StringFrom(record, "guid");
  const GURL url(StringFrom(record, "url"));
  const std::optional<Status> status =
      StatusFromName(StringFrom(record, "status"));
  const base::FilePath target =
      base::FilePath::FromUTF8Unsafe(StringFrom(record, "target"));
  if (guid.empty() || !url.is_valid() || !status || target.empty()) {
    return nullptr;
  }
  auto download =
      base::WrapUnique(new StarryDownload(service, /*is_new=*/false));
  download->guid_ = guid;
  download->url_ = url;
  download->final_url_ = GURL(StringFrom(record, "finalUrl"));
  download->referrer_ = GURL(StringFrom(record, "referrer"));
  const GURL initiator(StringFrom(record, "initiator"));
  if (initiator.is_valid()) {
    download->initiator_ = url::Origin::Create(initiator);
  }
  download->target_ = target;
  download->mime_type_ = StringFrom(record, "mimeType");
  download->start_time_ = TimeFrom(record.FindDouble("startTime"));
  download->end_time_ = TimeFrom(record.FindDouble("endTime"));
  download->error_ = ErrorFrom(record.FindDict("error"));
  download->interrupted_ = record.FindBool("interrupted").value_or(false);

  Saved saved;
  saved.total_bytes = Int64From(record, "total", -1);
  saved.range_support =
      RangeSupportFromName(StringFrom(record, "rangeSupport"));
  saved.etag = StringFrom(record, "etag");
  saved.last_modified = StringFrom(record, "lastModified");
  saved.segments = SegmentsFrom(record.FindList("segments"));
  const int64_t received = Int64From(record, "received", 0);

  switch (*status) {
    case Status::kQueued:
    case Status::kRunning:
    case Status::kPaused:
      // 上次退出时在下载的、排队的变成暂停，记下「退出时还在下载」。
      if (*status != Status::kPaused) {
        download->interrupted_ = true;
      }
      download->status_ = Status::kPaused;
      break;
    case Status::kFinished:
      download->file_missing_ = !target_exists;
      download->status_ = *status;
      break;
    case Status::kFailed:
    case Status::kCancelled:
      download->status_ = *status;
      break;
  }
  // 没下完的：分段记录对不上、临时文件没了就从头下。
  if (download->status_ == Status::kPaused ||
      download->status_ == Status::kFailed) {
    if (!(saved.range_support == RangeSupport::kSupported &&
          SegmentsConsistent(saved.segments, saved.total_bytes) &&
          partial_exists)) {
      saved.segments.clear();
      if (saved.range_support != RangeSupport::kSupported) {
        saved.total_bytes = -1;
      }
    }
  }
  download->saved_ = saved;
  download->total_bytes_ = saved.total_bytes;
  download->range_support_ = saved.range_support;
  download->etag_ = saved.etag;
  download->last_modified_ = saved.last_modified;
  download->segments_ = saved.segments;
  download->received_bytes_ =
      download->segments_.empty() && *status == Status::kFinished
          ? received
          : ::starry::ReceivedBytes(download->segments_);
  if (*status == Status::kFinished && download->total_bytes_ < 0) {
    download->total_bytes_ = received;
  }
  download->item_->Refresh();
  return download;
}

StarryDownload::~StarryDownload() = default;

base::DictValue StarryDownload::ToRecord() const {
  base::DictValue record;
  record.Set("guid", guid_);
  record.Set("url", url_.spec());
  record.Set("finalUrl", final_url_.is_valid() ? final_url_.spec() : "");
  record.Set("referrer", referrer_.is_valid() ? referrer_.spec() : "");
  record.Set("initiator", initiator_ && !initiator_->opaque()
                              ? initiator_->Serialize()
                              : "");
  record.Set("target", target_.AsUTF8Unsafe());
  record.Set("mimeType", mime_type_);
  record.Set("startTime", TimeValue(start_time_));
  record.Set("endTime", TimeValue(end_time_));
  record.Set("status", StatusName(status_));
  if (error_) {
    record.Set("error", ErrorValue(error_));
  }
  record.Set("interrupted", interrupted_);
  if (status_ == Status::kFinished) {
    record.Set("total", static_cast<double>(total_bytes_));
    record.Set("received", static_cast<double>(received_bytes_));
  } else {
    // 没下完的存最近一次刷到磁盘时的进度。
    record.Set("total", static_cast<double>(saved_.total_bytes));
    record.Set("received",
               static_cast<double>(::starry::ReceivedBytes(saved_.segments)));
    record.Set("rangeSupport", RangeSupportName(saved_.range_support));
    record.Set("etag", saved_.etag);
    record.Set("lastModified", saved_.last_modified);
    record.Set("segments", SegmentsValue(saved_.segments));
  }
  return record;
}

// static
base::FilePath StarryDownload::PartialPathFor(const base::FilePath& target) {
  // AddExtensionASCII 自己加点。
  return target.AddExtensionASCII(std::string_view(kPartialSuffix).substr(1));
}

// ---- Download ----

const std::string& StarryDownload::Guid() const {
  return guid_;
}

DownloadState StarryDownload::State() const {
  switch (status_) {
    case Status::kQueued:
      return DownloadState::kQueued;
    case Status::kRunning:
      return DownloadState::kRunning;
    case Status::kPaused:
      return DownloadState::kPaused;
    case Status::kFinished:
      return DownloadState::kComplete;
    case Status::kFailed:
      return DownloadState::kInterrupted;
    case Status::kCancelled:
      return DownloadState::kCancelled;
  }
}

std::u16string StarryDownload::Filename() const {
  const std::u16string name = target_.BaseName().LossyDisplayName();
  return name.empty() ? L("downloads.filename.fallback") : name;
}

GURL StarryDownload::Url() const {
  return url_;
}

GURL StarryDownload::ReferrerUrl() const {
  return referrer_;
}

int64_t StarryDownload::ReceivedBytes() const {
  return received_bytes_;
}

int64_t StarryDownload::TotalBytes() const {
  return total_bytes_;
}

int64_t StarryDownload::Speed() const {
  return static_cast<int64_t>(speed_);
}

base::Time StarryDownload::StartTime() const {
  return start_time_;
}

base::Time StarryDownload::EndTime() const {
  return end_time_;
}

base::FilePath StarryDownload::TargetPath() const {
  return target_;
}

base::FilePath StarryDownload::RevealPath() const {
  return status_ == Status::kFinished ? target_ : partial_path();
}

bool StarryDownload::FileMissing() const {
  return status_ == Status::kFinished && file_missing_;
}

bool StarryDownload::IsDangerous() const {
  return false;
}

bool StarryDownload::CanResume() const {
  return (status_ == Status::kPaused || status_ == Status::kFailed ||
          status_ == Status::kQueued) &&
         range_support_ != RangeSupport::kUnsupported;
}

std::u16string StarryDownload::ErrorText() const {
  return status_ == Status::kFailed ? DownloadErrorText(error_)
                                    : std::u16string();
}

bool StarryDownload::IsStarry() const {
  return true;
}

RangeSupport StarryDownload::GetRangeSupport() const {
  return range_support_;
}

const std::vector<DownloadSegment>& StarryDownload::Segments() const {
  return segments_;
}

int StarryDownload::Connections() const {
  return connections_;
}

int StarryDownload::MaxConnections() const {
  return max_connections_;
}

std::optional<int> StarryDownload::RetryAttempt() const {
  return retry_attempt_;
}

bool StarryDownload::IsConnecting() const {
  return status_ == Status::kRunning && connecting_;
}

content::WebContents* StarryDownload::GetWebContents() const {
  return web_contents_.get();
}

bool StarryDownload::IsNew() const {
  return is_new_;
}

// 暂停：排队的直接停下；下载中的停掉下载器，记下分段进度。不支持分段的继续时只能从头下。
void StarryDownload::Pause() {
  // 数据已经下完、正在改名：马上就完成了。
  if (finishing_) {
    return;
  }
  if (status_ == Status::kQueued) {
    SetStatus(Status::kPaused);
    return;
  }
  if (status_ != Status::kRunning) {
    return;
  }
  const int token = ++token_;
  if (downloader_) {
    downloader_->AsyncCall(&HttpDownloader::Pause)
        .Then(base::BindOnce(&StarryDownload::OnPaused,
                             weak_factory_.GetWeakPtr(), token));
    downloader_.reset();
  }
  ResetRuntime();
  SetStatus(Status::kPaused);
  service_->Pump();
}

void StarryDownload::OnPaused(int token,
                              const HttpDownloader::Progress& progress) {
  if (token != token_) {
    return;
  }
  ApplyProgress(progress);
  SaveProgress(progress);
  if (range_support_ != RangeSupport::kSupported) {
    task_runner_->PostTask(FROM_HERE,
                           base::GetDeleteFileCallback(partial_path()));
    ClearProgress();
  }
  Changed(/*notable=*/true);
}

// 继续：重新排队（不支持分段的从头下）。
void StarryDownload::Resume() {
  if (status_ != Status::kPaused && status_ != Status::kFailed) {
    return;
  }
  interrupted_ = false;
  error_ = DownloadError();
  end_time_ = base::Time();
  SetStatus(Status::kQueued);
  service_->Pump();
}

// 取消并删掉没下完的文件。
void StarryDownload::Cancel() {
  if (finishing_ || (!IsActive(State()) && status_ != Status::kFailed)) {
    return;
  }
  ++token_;
  if (downloader_) {
    downloader_->AsyncCall(&HttpDownloader::Cancel);
    downloader_.reset();
  } else {
    task_runner_->PostTask(FROM_HERE,
                           base::GetDeleteFileCallback(partial_path()));
  }
  ResetRuntime();
  ClearProgress();
  interrupted_ = false;
  end_time_ = base::Time::Now();
  SetStatus(Status::kCancelled);
  service_->Pump();
}

void StarryDownload::Remove() {
  if (IsActive(State()) || status_ == Status::kFailed) {
    Cancel();
  }
  service_->RemoveStarryDownload(this);
}

void StarryDownload::Open() {
  if (status_ != Status::kFinished) {
    return;
  }
  platform_util::OpenItem(
      service_->profile(), target_, platform_util::OPEN_FILE,
      base::BindOnce(
          [](base::WeakPtr<StarryDownload> download,
             platform_util::OpenOperationResult result) {
            if (download &&
                result == platform_util::OPEN_FAILED_PATH_NOT_FOUND) {
              download->file_missing_ = true;
              download->Changed(/*notable=*/false);
            }
          },
          weak_factory_.GetWeakPtr()));
}

void StarryDownload::DeleteFile(base::OnceCallback<void(bool)> callback) {
  if (status_ != Status::kFinished || file_missing_) {
    std::move(callback).Run(false);
    return;
  }
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](const base::FilePath& file) {
            return base::DeleteFile(file) && !base::PathExists(file);
          },
          target_),
      base::BindOnce(
          [](base::WeakPtr<StarryDownload> download,
             base::OnceCallback<void(bool)> callback, bool deleted) {
            if (download && deleted) {
              download->file_missing_ = true;
              download->Changed(/*notable=*/false);
            }
            std::move(callback).Run(deleted);
          },
          weak_factory_.GetWeakPtr(), std::move(callback)));
}

// ---- DownloadService 调的 ----

void StarryDownload::Launch() {
  const int token = ++token_;
  ResetRuntime();
  connecting_ = true;
  end_time_ = base::Time();
  const int connections = service_->EffectiveConnections();
  max_connections_ =
      range_support_ == RangeSupport::kUnsupported ? 1 : connections;

  HttpDownloader::Request request;
  request.url = url_;
  request.referrer = referrer_;
  request.initiator = initiator_;
  request.partial_file = partial_path();
  request.max_connections = connections;
  request.retries = service_->RetriesAutomatically();
  request.send_cookies = service_->CookiesAllowed(url_);

  HttpDownloader::Progress progress;
  if (range_support_ == RangeSupport::kSupported && total_bytes_ > 0 &&
      !segments_.empty()) {
    progress.total_bytes = total_bytes_;
    progress.range_support = range_support_;
    progress.segments = segments_;
    progress.etag = etag_;
    progress.last_modified = last_modified_;
    progress.final_url = final_url_;
    progress.mime_type = mime_type_;
  }
  downloader_.emplace(
      task_runner_, service_->CreateLoaderFactory(),
      service_->CreateCookieManager(), request, progress,
      base::BindPostTaskToCurrentDefault(base::BindRepeating(
          &StarryDownload::OnOutcome, weak_factory_.GetWeakPtr(), token)));
  downloader_->AsyncCall(&HttpDownloader::Start);
  SetStatus(Status::kRunning);
}

void StarryDownload::Tick(base::TimeTicks now) {
  if (!downloader_ || status_ != Status::kRunning || snapshotting_) {
    return;
  }
  snapshotting_ = true;
  downloader_->AsyncCall(&HttpDownloader::GetSnapshot)
      .Then(base::BindOnce(&StarryDownload::OnSnapshot,
                           weak_factory_.GetWeakPtr(), token_, now));
}

void StarryDownload::OnSnapshot(int token,
                                base::TimeTicks now,
                                const HttpDownloader::Snapshot& snapshot) {
  snapshotting_ = false;
  if (token != token_ || status_ != Status::kRunning) {
    return;
  }
  ApplyProgress(snapshot.progress);
  connections_ = snapshot.connections;
  retry_attempt_ = snapshot.retry_attempt;
  connecting_ = snapshot.connecting;
  meter_.Record(received_bytes_, now);
  speed_ = meter_.bytes_per_second();
  Changed(/*notable=*/false);
}

void StarryDownload::Checkpoint() {
  if (!downloader_ || status_ != Status::kRunning || checkpointing_) {
    return;
  }
  checkpointing_ = true;
  downloader_->AsyncCall(&HttpDownloader::Checkpoint)
      .Then(base::BindOnce(&StarryDownload::OnCheckpoint,
                           weak_factory_.GetWeakPtr(), token_));
}

void StarryDownload::OnCheckpoint(int token,
                                  const HttpDownloader::Progress& progress) {
  checkpointing_ = false;
  if (token != token_) {
    return;
  }
  SaveProgress(progress);
  service_->ScheduleSave();
}

void StarryDownload::SetMaxConnections(int count) {
  if (!downloader_) {
    return;
  }
  downloader_->AsyncCall(&HttpDownloader::SetMaxConnections).WithArgs(count);
  if (range_support_ == RangeSupport::kSupported) {
    max_connections_ = count;
  }
  Changed(/*notable=*/false);
}

void StarryDownload::MarkInterrupted() {
  if (status_ == Status::kRunning || status_ == Status::kQueued) {
    interrupted_ = true;
  }
}

void StarryDownload::CheckFileExists() {
  if (status_ != Status::kFinished) {
    return;
  }
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&base::PathExists, target_),
      base::BindOnce(
          [](base::WeakPtr<StarryDownload> download, bool exists) {
            if (download && download->file_missing_ == exists) {
              download->file_missing_ = !exists;
              download->Changed(/*notable=*/false);
            }
          },
          weak_factory_.GetWeakPtr()));
}

// ---- 内部 ----

void StarryDownload::SetStatus(Status status) {
  status_ = status;
  Changed(/*notable=*/true);
}

void StarryDownload::Changed(bool notable) {
  item_->Refresh();
  service_->NotifyUpdated(this, notable);
}

void StarryDownload::ApplyProgress(const HttpDownloader::Progress& progress) {
  segments_ = progress.segments;
  received_bytes_ = progress.received();
  if (progress.total_bytes >= 0 ||
      progress.range_support != RangeSupport::kUnknown) {
    total_bytes_ = progress.total_bytes;
  }
  if (progress.range_support != RangeSupport::kUnknown) {
    range_support_ = progress.range_support;
  }
  etag_ = progress.etag;
  last_modified_ = progress.last_modified;
  if (progress.final_url.is_valid()) {
    final_url_ = progress.final_url;
  }
  if (!progress.mime_type.empty()) {
    mime_type_ = progress.mime_type;
  }
}

void StarryDownload::SaveProgress(const HttpDownloader::Progress& progress) {
  saved_.total_bytes = progress.total_bytes;
  saved_.range_support = progress.range_support;
  saved_.etag = progress.etag;
  saved_.last_modified = progress.last_modified;
  saved_.segments = progress.segments;
}

void StarryDownload::ResetRuntime() {
  connections_ = 0;
  retry_attempt_.reset();
  connecting_ = false;
  checkpointing_ = false;
  snapshotting_ = false;
  speed_ = 0;
  meter_.Reset();
}

void StarryDownload::ClearProgress() {
  segments_.clear();
  received_bytes_ = 0;
  saved_.segments.clear();
}

void StarryDownload::OnOutcome(int token,
                               const HttpDownloader::Outcome& outcome) {
  if (token != token_) {
    return;
  }
  ApplyProgress(outcome.progress);
  switch (outcome.kind) {
    case HttpDownloader::Outcome::Kind::kResponded:
      // 从头探测过了：之前存的分段作废。
      SaveProgress(outcome.progress);
      connecting_ = false;
      max_connections_ = range_support_ == RangeSupport::kSupported
                             ? service_->EffectiveConnections()
                             : 1;
      Changed(/*notable=*/true);
      return;
    case HttpDownloader::Outcome::Kind::kFinished:
      downloader_.reset();
      ++token_;
      ResetRuntime();
      finishing_ = true;
      MoveIntoPlace();
      return;
    case HttpDownloader::Outcome::Kind::kFailed:
      downloader_.reset();
      ++token_;
      ResetRuntime();
      SaveProgress(outcome.progress);
      // 服务器拒绝了 Starry 的请求：交回 Chrome 用网页自己的方式再试一次。
      if (outcome.rejected) {
        service_->HandBack(this);
        return;
      }
      error_ = outcome.error;
      end_time_ = base::Time::Now();
      SetStatus(Status::kFailed);
      service_->Pump();
      return;
  }
}

void StarryDownload::MoveIntoPlace() {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&MoveIntoPlaceOnSequence, partial_path(), target_),
      base::BindOnce(&StarryDownload::OnMoved, weak_factory_.GetWeakPtr()));
}

void StarryDownload::OnMoved(std::optional<base::FilePath> target) {
  if (status_ != Status::kRunning) {
    return;
  }
  if (!target) {
    finishing_ = false;
    error_.kind = DownloadError::Kind::kSaveFailed;
    end_time_ = base::Time::Now();
    SetStatus(Status::kFailed);
    service_->Pump();
    return;
  }
  target_ = *target;
  // 标上隔离属性和来源（同 Chrome：macOS 的门禁、Windows 的网络标记），无痕的不记来源。
  // 隔离服务连不上、断开了也算完成。
  download::QuarantineConnectionCallback connect =
      service_->QuarantineConnection();
  if (!connect) {
    Completed();
    return;
  }
  connect.Run(quarantine_.BindNewPipeAndPassReceiver());
  quarantine_.set_disconnect_handler(
      base::BindOnce(&StarryDownload::OnQuarantined, weak_factory_.GetWeakPtr(),
                     quarantine::mojom::QuarantineFileResult::OK));
  const bool off_the_record = service_->profile()->IsOffTheRecord();
  const GURL source = final_url_.is_valid() ? final_url_ : url_;
  quarantine_->QuarantineFile(target_, off_the_record ? GURL() : source,
                              off_the_record ? GURL() : referrer_, initiator_,
                              std::string(),
                              base::BindOnce(&StarryDownload::OnQuarantined,
                                             weak_factory_.GetWeakPtr()));
}

void StarryDownload::OnQuarantined(
    quarantine::mojom::QuarantineFileResult result) {
  quarantine_.reset();
  if (status_ == Status::kRunning) {
    Completed();
  }
}

// 下载完成：进度填满，按设置的同时下载数开始下一个。
void StarryDownload::Completed() {
  finishing_ = false;
  if (total_bytes_ < 0 || received_bytes_ > total_bytes_) {
    total_bytes_ = received_bytes_;
  }
  received_bytes_ = total_bytes_;
  end_time_ = base::Time::Now();
  interrupted_ = false;
  file_missing_ = false;
  error_ = DownloadError();
  segments_.clear();
  saved_ = Saved();
  saved_.total_bytes = total_bytes_;
  SetStatus(Status::kFinished);
  service_->Pump();
}

}  // namespace starry
