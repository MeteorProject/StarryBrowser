// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_DOWNLOAD_H_
#define STARRY_BROWSER_DOWNLOADS_DOWNLOAD_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "starry/browser/downloads/download_model.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}

namespace download {
class DownloadItem;
}

namespace starry {

// 下载项的状态，下载页和弹出框共用。
enum class DownloadState {
  // Starry 自己下载的，超过「同时下载的文件数」时排队。
  kQueued,
  kRunning,
  kPaused,
  kComplete,
  kCancelled,
  kInterrupted,
};

// queued、running、paused、complete、cancelled、interrupted（下载页用的名字）。
const char* StateName(DownloadState state);
inline bool IsActive(DownloadState state) {
  return state == DownloadState::kQueued || state == DownloadState::kRunning ||
         state == DownloadState::kPaused;
}

// 下载列表里的一项。网页里的下载都先由 Chrome 开始；Chrome 收到回应、定下文件名后，能
// 分段的交给 Starry 自己下载（StarryDownload，多线程、限速、排队），其余的还是 Chrome
// 的（ChromeDownload，包着 download::DownloadItem）。交过去时 GUID 不变，界面上还是
// 同一行。界面只用这个接口，列表见 DownloadService。
class Download {
 public:
  virtual ~Download() = default;

  virtual const std::string& Guid() const = 0;
  virtual DownloadState State() const = 0;
  virtual std::u16string Filename() const = 0;
  // 下载的网址（重定向之前的）。
  virtual GURL Url() const = 0;
  virtual GURL ReferrerUrl() const = 0;
  virtual int64_t ReceivedBytes() const = 0;
  // 不知道时是 0 或 -1。
  virtual int64_t TotalBytes() const = 0;
  // 字节每秒。
  virtual int64_t Speed() const = 0;
  virtual base::Time StartTime() const = 0;
  virtual base::Time EndTime() const = 0;
  // 下载好的文件放在哪里（文件名还没定时是空的）。
  virtual base::FilePath TargetPath() const = 0;
  // 「在访达中显示」选中的：下完的是文件本身，没下完的是临时文件。
  virtual base::FilePath RevealPath() const = 0;
  virtual bool FileMissing() const = 0;
  virtual bool IsDangerous() const = 0;
  // 暂停、失败后能不能接着下（不能的要从头下载）。
  virtual bool CanResume() const = 0;
  // 失败的原因；没失败时是空的。
  virtual std::u16string ErrorText() const = 0;

  // Starry 自己下载的才有这些（Chrome 的不知道）。
  virtual bool IsStarry() const;
  virtual RangeSupport GetRangeSupport() const;
  virtual const std::vector<DownloadSegment>& Segments() const;
  // 正在传数据的连接、最多用几个连接。
  virtual int Connections() const;
  virtual int MaxConnections() const;
  // 所有连接都在等着重试时：第几次。
  virtual std::optional<int> RetryAttempt() const;
  virtual bool IsConnecting() const;

  // 发起下载的网页（开始下载的动画放在它的窗口里）。
  virtual content::WebContents* GetWebContents() const;
  // 这一次启动里新开始的（不是从历史记录、存盘的列表里读回来的）。
  virtual bool IsNew() const = 0;

  virtual void Pause() = 0;
  // 继续暂停的下载，或者接着下失败的（CanResume 时）。
  virtual void Resume() = 0;
  virtual void Cancel() = 0;
  // 从列表里去掉（不删已经下好的文件）；没下完的先取消。
  virtual void Remove() = 0;
  virtual void Open() = 0;
  // 可能有危险的文件：保留。
  virtual void KeepDangerous();

  // Chrome 的下载才有。
  virtual download::DownloadItem* chrome_item() const;
};

// Chrome 自己下载的一项。
class ChromeDownload : public Download {
 public:
  explicit ChromeDownload(download::DownloadItem* item, bool is_new);
  ChromeDownload(const ChromeDownload&) = delete;
  ChromeDownload& operator=(const ChromeDownload&) = delete;
  ~ChromeDownload() override;

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
  content::WebContents* GetWebContents() const override;
  bool IsNew() const override;
  void Pause() override;
  void Resume() override;
  void Cancel() override;
  void Remove() override;
  void Open() override;
  void KeepDangerous() override;
  download::DownloadItem* chrome_item() const override;

 private:
  const raw_ptr<download::DownloadItem> item_;
  const bool is_new_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_DOWNLOAD_H_
