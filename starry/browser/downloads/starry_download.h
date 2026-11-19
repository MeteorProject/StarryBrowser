// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_STARRY_DOWNLOAD_H_
#define STARRY_BROWSER_DOWNLOADS_STARRY_DOWNLOAD_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "base/values.h"
#include "components/services/quarantine/public/mojom/quarantine.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/downloads/http_downloader.h"
#include "starry/browser/downloads/starry_download_item.h"
#include "url/origin.h"

namespace content {
class WebContents;
}

namespace starry {

class DownloadService;

// Starry 自己下载的一项：Chrome 交过来的能分段的下载，或者上次退出时存下来的。下载器
// （HttpDownloader）在这一项自己的序列上跑，这个对象在界面线程上，记着要存盘的状态，定时取进度。
class StarryDownload : public Download {
 public:
  enum class Status {
    kQueued,
    kRunning,
    kPaused,
    kFinished,
    kFailed,
    kCancelled,
  };

  // 从 Chrome 的下载接过来时知道的。
  struct Params {
    Params();
    Params(const Params&);
    Params& operator=(const Params&);
    ~Params();

    std::string guid;
    GURL url;
    GURL final_url;
    GURL referrer;
    std::optional<url::Origin> initiator;
    base::FilePath target;
    std::string mime_type;
    int64_t total_bytes = -1;
    std::string etag;
    std::string last_modified;
    base::Time start_time;
    base::WeakPtr<content::WebContents> web_contents;
  };

  // 临时文件的后缀（同 Chrome 的 .crdownload）。
  static constexpr char kPartialSuffix[] = ".starrydownload";

  StarryDownload(DownloadService* service, const Params& params);
  // 存盘的一条记录（DownloadService 读回列表时）；读不懂时是空的。`partial_exists`、
  // `target_exists` 是读文件时顺便看的。
  static std::unique_ptr<StarryDownload> FromRecord(
      DownloadService* service,
      const base::DictValue& record,
      bool partial_exists,
      bool target_exists);
  StarryDownload(const StarryDownload&) = delete;
  StarryDownload& operator=(const StarryDownload&) = delete;
  ~StarryDownload() override;

  base::DictValue ToRecord() const;
  static base::FilePath PartialPathFor(const base::FilePath& target);

  // Download:
  const std::string& Guid() const override;
  DownloadState State() const override;
  std::u16string Filename() const override;
  GURL Url() const override;
  GURL ReferrerUrl() const override;
  int64_t ReceivedBytes() const override;
  int64_t TotalBytes() const override;
  int64_t Speed() const override;
  base::Time StartTime() const override;
  base::Time EndTime() const override;
  base::FilePath TargetPath() const override;
  base::FilePath RevealPath() const override;
  bool FileMissing() const override;
  bool IsDangerous() const override;
  bool CanResume() const override;
  std::u16string ErrorText() const override;
  bool IsStarry() const override;
  RangeSupport GetRangeSupport() const override;
  const std::vector<DownloadSegment>& Segments() const override;
  int Connections() const override;
  int MaxConnections() const override;
  std::optional<int> RetryAttempt() const override;
  bool IsConnecting() const override;
  content::WebContents* GetWebContents() const override;
  bool IsNew() const override;
  void Pause() override;
  void Resume() override;
  void Cancel() override;
  void Remove() override;
  void Open() override;

  DownloadService* service() const { return service_; }
  // 给 Chrome 的代码（扩展、程序坞图标）看的样子。
  StarryDownloadItem* item() const { return item_.get(); }
  Status status() const { return status_; }
  // 上次退出时还在下载：启动时可以自动继续。
  bool interrupted() const { return interrupted_; }
  // 数据下完了，正在改回原名、标隔离属性。
  bool finishing() const { return finishing_; }
  const GURL& url() const { return url_; }
  const GURL& final_url() const { return final_url_; }
  const base::FilePath& target() const { return target_; }
  base::FilePath partial_path() const { return PartialPathFor(target_); }
  const GURL& referrer() const { return referrer_; }
  const std::optional<url::Origin>& initiator() const { return initiator_; }
  const std::string& mime_type() const { return mime_type_; }
  const std::string& etag() const { return etag_; }
  const std::string& last_modified() const { return last_modified_; }
  const DownloadError& error() const { return error_; }

  // 删掉下好的文件（扩展的 chrome.downloads.removeFile），记录留着。
  void DeleteFile(base::OnceCallback<void(bool)> callback);

  // DownloadService 调的。
  // 轮到了：开始下载（状态已经是 kRunning）。
  void Launch();
  // 定时取一次进度、算速度。
  void Tick(base::TimeTicks now);
  // 把数据刷到磁盘，记下可以存盘的进度。
  void Checkpoint();
  void SetMaxConnections(int count);
  // 退出前：在下载的记成「退出时还在下载」。
  void MarkInterrupted();
  // 下载页打开时：看看下好的文件还在不在。
  void CheckFileExists();

 private:
  // 存盘的进度：最近一次刷到磁盘时的（不会比文件里的数据多）。
  struct Saved {
    Saved();
    Saved(const Saved&);
    Saved& operator=(const Saved&);
    ~Saved();

    int64_t total_bytes = -1;
    RangeSupport range_support = RangeSupport::kUnknown;
    std::string etag;
    std::string last_modified;
    std::vector<DownloadSegment> segments;
  };

  StarryDownload(DownloadService* service, bool is_new);

  void SetStatus(Status status);
  void Changed(bool notable);
  void ApplyProgress(const HttpDownloader::Progress& progress);
  void SaveProgress(const HttpDownloader::Progress& progress);
  void ResetRuntime();
  void ClearProgress();
  void OnOutcome(int token, const HttpDownloader::Outcome& outcome);
  void OnSnapshot(int token,
                  base::TimeTicks now,
                  const HttpDownloader::Snapshot& snapshot);
  void OnCheckpoint(int token, const HttpDownloader::Progress& progress);
  void OnPaused(int token, const HttpDownloader::Progress& progress);
  void MoveIntoPlace();
  void OnMoved(std::optional<base::FilePath> target);
  void OnQuarantined(quarantine::mojom::QuarantineFileResult result);
  void Completed();

  const raw_ptr<DownloadService> service_;
  const bool is_new_;
  const scoped_refptr<base::SequencedTaskRunner> task_runner_;

  // 存盘的。
  std::string guid_;
  GURL url_;
  GURL final_url_;
  GURL referrer_;
  std::optional<url::Origin> initiator_;
  base::FilePath target_;
  std::string mime_type_;
  base::Time start_time_;
  base::Time end_time_;
  Status status_ = Status::kQueued;
  DownloadError error_;
  bool interrupted_ = false;
  Saved saved_;

  // 现在的进度（下载中每次取进度时更新）。
  int64_t total_bytes_ = -1;
  RangeSupport range_support_ = RangeSupport::kUnknown;
  std::string etag_;
  std::string last_modified_;
  std::vector<DownloadSegment> segments_;
  int64_t received_bytes_ = 0;

  // 运行时的。
  bool file_missing_ = false;
  base::WeakPtr<content::WebContents> web_contents_;
  std::optional<base::SequenceBound<HttpDownloader>> downloader_;
  mojo::Remote<quarantine::mojom::Quarantine> quarantine_;
  // 每次开始、停下都加一：旧的下载器晚到的回报不算。
  int token_ = 0;
  int connections_ = 0;
  int max_connections_ = 0;
  std::optional<int> retry_attempt_;
  bool connecting_ = false;
  bool checkpointing_ = false;
  bool snapshotting_ = false;
  // 数据下完了，正在改回原名、标隔离属性：这时不再暂停、取消。
  bool finishing_ = false;
  TransferRateMeter meter_;
  double speed_ = 0;

  // 放在最后：先于别的成员销毁（销毁时它还会告诉观察者）。
  std::unique_ptr<StarryDownloadItem> item_;

  base::WeakPtrFactory<StarryDownload> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_STARRY_DOWNLOAD_H_
