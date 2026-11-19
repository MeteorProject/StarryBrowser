// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_STARRY_DOWNLOAD_ITEM_H_
#define STARRY_BROWSER_DOWNLOADS_STARRY_DOWNLOAD_ITEM_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/time/time.h"
#include "components/download/public/common/download_item.h"
#include "url/gurl.h"

namespace starry {

class StarryDownload;

// StarryDownload 在 Chrome 的下载接口（download::DownloadItem）下的样子，给只认这个接口的
// Chrome 代码用：扩展的 chrome.downloads（downloads_api.cc 的钩子）、程序坞图标上的下载个数
// 和进度、访达里文件上的进度（DownloadStatusUpdater）。见 chrome_download_bridge.h。
// 不在 DownloadManager 里：Chrome 的下载记录（History）看不到它。
//
// 编号：从 Chrome 交过来的用 Chrome 那一项的编号（扩展看到的还是同一个下载）；读回来的
// 记录向 DownloadManager 要一个新的（同一次启动里不会和 Chrome 的重复，不存盘，下次启动
// 换一个）。拿到编号之前（kInvalidId）扩展看不到。
//
// 状态对应：排队、下载中是 IN_PROGRESS，暂停是暂停了的 IN_PROGRESS（同 Chrome），失败是
// INTERRUPTED（原因按 DownloadError 换算），取消是 CANCELLED，下完是 COMPLETE。
class StarryDownloadItem : public download::DownloadItem {
 public:
  explicit StarryDownloadItem(StarryDownload* download);
  StarryDownloadItem(const StarryDownloadItem&) = delete;
  StarryDownloadItem& operator=(const StarryDownloadItem&) = delete;
  ~StarryDownloadItem() override;

  // 是 Starry 的一项时返回它自己。
  static StarryDownloadItem* From(const download::DownloadItem* item);

  StarryDownload* download() const { return download_; }
  void set_id(uint32_t id) { id_ = id; }
  bool has_id() const { return id_ != kInvalidId; }

  // StarryDownload 变了：更新返回引用的那几个值，再告诉观察者。
  void Refresh();
  // 从列表里去掉了。
  void NotifyRemoved();

  // download::DownloadItem:
  void AddObserver(Observer* observer) override;
  void RemoveObserver(Observer* observer) override;
  void UpdateObservers() override;
  void ValidateDangerousDownload() override;
  void ValidateInsecureDownload() override;
  void ConfirmNonDangerousDownload() override;
  void CopyDownload(AcquireFileCallback callback) override;
  void Pause() override;
  void Resume(bool user_resume) override;
  void Cancel(bool user_cancel) override;
  void Remove() override;
  void OpenDownload() override;
  void ShowDownloadInShell() override;
  void Rename(const base::FilePath& new_name,
              RenameDownloadCallback callback) override;
  uint32_t GetId() const override;
  const std::string& GetGuid() const override;
  DownloadState GetState() const override;
  download::DownloadInterruptReason GetLastReason() const override;
  bool IsPaused() const override;
  bool AllowMetered() const override;
  bool IsTemporary() const override;
  bool CanResume() const override;
  bool IsDone() const override;
  int64_t GetBytesWasted() const override;
  int32_t GetAutoResumeCount() const override;
  const GURL& GetURL() const override;
  const std::vector<GURL>& GetUrlChain() const override;
  bool IsUrlTruncated() const override;
  const GURL& GetOriginalUrl() const override;
  const GURL& GetReferrerUrl() const override;
  const std::string& GetSerializedEmbedderDownloadData() const override;
  const GURL& GetTabUrl() const override;
  const GURL& GetTabReferrerUrl() const override;
  const std::optional<url::Origin>& GetRequestInitiator() const override;
  std::string GetSuggestedFilename() const override;
  const scoped_refptr<const net::HttpResponseHeaders>& GetResponseHeaders()
      const override;
  std::string GetContentDisposition() const override;
  std::string GetMimeType() const override;
  std::string GetOriginalMimeType() const override;
  std::string GetRemoteAddress() const override;
  bool HasUserGesture() const override;
  ui::PageTransition GetTransitionType() const override;
  const std::string& GetLastModifiedTime() const override;
  const std::string& GetETag() const override;
  bool IsSavePackageDownload() const override;
  download::DownloadSource GetDownloadSource() const override;
  ::network::mojom::CredentialsMode GetCredentialsMode() const override;
  ::network::mojom::RequestMode GetFetchRequestMode() const override;
  const std::optional<net::IsolationInfo>& GetIsolationInfo() const override;
  const base::FilePath& GetFullPath() const override;
  const base::FilePath& GetTargetFilePath() const override;
  const base::FilePath& GetForcedFilePath() const override;
  base::FilePath GetTemporaryFilePath() const override;
  base::FilePath GetFileNameToReportUser() const override;
  TargetDisposition GetTargetDisposition() const override;
  const std::string& GetHash() const override;
  bool GetFileExternallyRemoved() const override;
  void DeleteFile(base::OnceCallback<void(bool)> callback) override;
  bool IsDangerous() const override;
  bool IsInsecure() const override;
  bool IsUserConfirmed() const override;
  download::DownloadDangerType GetDangerType() const override;
  InsecureDownloadStatus GetInsecureDownloadStatus() const override;
  download::DownloadFile* GetDownloadFile() override;
  download::DownloadItemRenameHandler* GetRenameHandler() override;
  bool TimeRemaining(base::TimeDelta* remaining) const override;
  int64_t CurrentSpeed() const override;
  int PercentComplete() const override;
  bool AllDataSaved() const override;
  int64_t GetTotalBytes() const override;
  int64_t GetReceivedBytes() const override;
  const std::vector<ReceivedSlice>& GetReceivedSlices() const override;
  int64_t GetUploadedBytes() const override;
  base::Time GetStartTime() const override;
  base::Time GetEndTime() const override;
  bool CanShowInFolder() override;
  bool CanOpenDownload() override;
  bool ShouldOpenFileBasedOnExtension() override;
  bool ShouldOpenFileByPolicyBasedOnExtension() override;
  bool GetOpenWhenComplete() const override;
  bool GetAutoOpened() override;
  bool GetOpened() const override;
  base::Time GetLastAccessTime() const override;
  bool IsTransient() const override;
  bool RequireSafetyChecks() const override;
  bool IsParallelDownload() const override;
  DownloadCreationType GetDownloadCreationType() const override;
  void OnContentCheckCompleted(
      download::DownloadDangerType danger_type,
      download::DownloadInterruptReason reason) override;
  void OnAsyncScanningCompleted(
      download::DownloadDangerType danger_type) override;
  void SetOpenWhenComplete(bool open) override;
  void SetOpened(bool opened) override;
  void SetLastAccessTime(base::Time last_access_time) override;
  void SetDisplayName(const base::FilePath& name) override;
  std::string DebugString(bool verbose) const override;
  void SimulateErrorForTesting(
      download::DownloadInterruptReason reason) override;

 private:
  const raw_ptr<StarryDownload> download_;
  uint32_t id_ = kInvalidId;
  // 接口返回引用的，Refresh 时算好。
  GURL url_;
  std::vector<GURL> url_chain_;
  base::FilePath full_path_;
  base::FilePath target_path_;
  bool opened_ = false;
  base::Time last_access_time_;
  base::ObserverList<Observer> observers_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_STARRY_DOWNLOAD_ITEM_H_
