// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_DOWNLOAD_SERVICE_H_
#define STARRY_BROWSER_DOWNLOADS_DOWNLOAD_SERVICE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/download/content/public/all_download_item_notifier.h"
#include "components/download/public/common/quarantine_connection.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/cookie_manager.mojom-forward.h"
#include "url/gurl.h"

class Profile;
class ScopedProfileKeepAlive;

namespace content {
class WebContents;
}

namespace network {
class PendingSharedURLLoaderFactory;
}

namespace starry {

class ChromeDownload;
class Download;
class DownloadNotifications;
class StarryDownload;
class StarryDownloadItem;

// 一个用户资料（普通或无痕）的下载列表：Chrome 自己下载的和 Starry 自己下载的合在一起，
// 界面（下载按钮、弹出框、下载页、开始下载的动画）都看它。
//
// 网页里的下载都先由 Chrome 开始。Chrome 收到回应、定下文件名（问过存到哪里）之后，回应
// 声明了支持分段的普通下载（CanHandOff）交给 Starry 自己的下载器：GUID 不变，Chrome 的
// 那一项取消并从记录里去掉。Starry 的下载被服务器拒绝（4xx，一点数据还没收到）时交回
// Chrome 用网页的方式再试一次。扩展（chrome.downloads）发起的、可能有危险的、另存网页
// 这些留给 Chrome。
//
// Starry 自己下载的按开始的先后占「同时下载的文件数」，其余的排队；列表和续传的分段进度
// 存在用户资料目录的 `Starry Downloads.json`（无痕的不存）：状态变了 1 秒内存，下载中
// 每 3 秒先把数据刷到磁盘再存进度。退出时还在下载的，下次启动接着下（设置里可以关）。
class DownloadService : public KeyedService,
                        public download::AllDownloadItemNotifier::Observer,
                        public base::ImportantFileWriter::DataSerializer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // 列表里多了一项（新的下载，读回来的记录）。
    virtual void OnDownloadAdded(Download* download) {}
    // 状态、进度变了。Chrome 的下载交给 Starry 时也是这个（GUID 不变）。
    virtual void OnDownloadUpdated(Download* download) {}
    virtual void OnDownloadRemoved(const std::string& guid) {}
    virtual void OnDownloadServiceShutdown() {}
  };

  // 没有（系统用户资料）时是空的。
  static DownloadService* Get(Profile* profile);
  // 已经开始、还没关闭的（不新建，用户资料正在关闭时也能调）。
  static DownloadService* GetIfRunning(const Profile* profile);
  static const std::vector<DownloadService*>& Running();
  // 在 Chrome 建第一个用户资料之前调用一次。
  static void EnsureFactoryBuilt();

  explicit DownloadService(Profile* profile);
  DownloadService(const DownloadService&) = delete;
  DownloadService& operator=(const DownloadService&) = delete;
  ~DownloadService() override;

  Profile* profile() const { return profile_; }

  // 要列出来的下载，新的在前。
  std::vector<Download*> Downloads() const;
  Download* Find(std::string_view guid) const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // 从网址开始一个下载（下载页的「新建下载」、重新下载）：经 Chrome 开始，能分段的照样
  // 交给 Starry。`contents` 是发起的网页（开始下载的动画放在它的窗口里），可以是空的。
  void StartDownload(const GURL& url,
                     content::WebContents* contents,
                     const GURL& referrer = GURL());
  // 界面上的「重试」：能接着下的接着下，不能的从头重新下载。
  void Retry(Download* download, content::WebContents* contents);
  // 去掉已经结束的记录（不删文件）。
  void ClearInactive();
  // 去掉全部记录，进行中的先取消。
  void ClearAll();
  void PauseAll();
  void ResumeAll();
  // 下载页打开时：看看下好的文件还在不在。
  void CheckFilesExist();
  // 退出时会丢掉的 Starry 下载：无痕的没下完的（普通的下次启动接着下，不算）。退出确认时和
  // Chrome 的下载一起数。
  int LostOnShutdownCount() const;
  // 退出确认里选了退出：取消这些下载（不然再退出时又会问）。
  void CancelLostOnShutdown();

  // Starry 自己下载的在 Chrome 接口下的样子（扩展、程序坞图标，
  // chrome_download_bridge.h）。只算有了编号的。
  StarryDownloadItem* FindItem(uint32_t id) const;
  void AppendItems(
      std::vector<raw_ptr<download::DownloadItem, VectorExperimental>>* items)
      const;
  // 程序坞图标的下载个数和总进度：加上进行中的（含暂停、排队）。
  void AddProgress(int* count,
                   int64_t* received_bytes,
                   int64_t* total_bytes,
                   bool* certain) const;
  bool HasKeepAliveForTesting() const { return keep_alive_ != nullptr; }

  // 下面是 StarryDownload 调的。
  // 按开始的先后启动排队的下载，直到占满同时下载的文件数。
  void Pump();
  // `notable`：状态变了（要存盘），不只是进度。
  void NotifyUpdated(Download* download, bool notable);
  // 从列表里去掉（稍后删掉这个对象）。
  void RemoveStarryDownload(StarryDownload* download);
  // 服务器拒绝了 Starry 的请求：交回 Chrome 再试一次（稍后删掉这个对象）。
  void HandBack(StarryDownload* download);
  // 每个文件用几个连接：关掉多线程时是 1；限速时每 512 KB/s 一个连接（限了速多开连接也快不
  // 了，起步时的突发还更大）。
  int EffectiveConnections() const;
  bool RetriesAutomatically() const;
  std::unique_ptr<network::PendingSharedURLLoaderFactory> CreateLoaderFactory();
  mojo::PendingRemote<network::mojom::CookieManager> CreateCookieManager();
  // 下载这个网址时能不能带 Cookie（当成第一方；用户屏蔽了这个网站的 Cookie 时不带）。
  bool CookiesAllowed(const GURL& url) const;
  download::QuarantineConnectionCallback QuarantineConnection();
  void ScheduleSave();

  // KeyedService:
  void Shutdown() override;

  // download::AllDownloadItemNotifier::Observer:
  void OnManagerGoingDown(content::DownloadManager* manager) override;
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;
  void OnDownloadUpdated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;
  void OnDownloadRemoved(content::DownloadManager* manager,
                         download::DownloadItem* item) override;

  // base::ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

 private:
  struct Loaded;

  // 在文件的序列上读存盘的列表。
  static std::vector<Loaded> ReadRecords(const base::FilePath& path);

  void Init();
  void AddChromeDownload(download::DownloadItem* item);
  bool ShouldHandOff(const download::DownloadItem& item) const;
  void MaybeHandOff(download::DownloadItem* item);
  base::FilePath UniqueTarget(const base::FilePath& path) const;
  StarryDownload* FindStarry(std::string_view guid) const;
  std::unique_ptr<StarryDownload> TakeStarry(StarryDownload* download);
  void OnLoaded(std::vector<Loaded> loaded);
  // 读回来的记录向 DownloadManager 要编号。
  void OnItemId(const std::string& guid, uint32_t id);
  void ResumeInterrupted();
  void OnPrefsChanged();
  void UpdateTicker();
  void Tick();
  // 有 Starry 的下载在进行时，窗口都关了也不关闭这个用户资料（同 Chrome 的
  // DownloadStatusUpdater）。
  void UpdateKeepAlive();
  // 从列表里拿掉了一项：告诉扩展。
  void ItemGone(StarryDownload* download);

  const raw_ptr<Profile> profile_;
  std::unique_ptr<download::AllDownloadItemNotifier> notifier_;
  std::map<std::string, std::unique_ptr<ChromeDownload>, std::less<>>
      chrome_downloads_;
  std::vector<std::unique_ptr<StarryDownload>> starry_downloads_;
  // 看过要不要交给 Starry 的 Chrome 下载（一项只看一次：定下文件名的时候）。
  std::set<std::string, std::less<>> considered_;
  // 交给了 Starry 的：Chrome 那一项后来的变化不再管。
  std::set<std::string, std::less<>> handed_off_;
  // 交回 Chrome 的：不再交给 Starry，也不再播开始下载的动画。
  std::set<std::string, std::less<>> handed_back_;
  base::ObserverList<Observer> observers_;

  PrefChangeRegistrar prefs_;
  int effective_connections_ = 0;

  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  std::unique_ptr<base::ImportantFileWriter> writer_;
  // 存盘的列表读回来了（之前不存，免得覆盖掉）。
  bool loaded_ = false;
  bool shut_down_ = false;

  base::RepeatingTimer ticker_;
  base::TimeTicks last_checkpoint_;

  std::unique_ptr<ScopedProfileKeepAlive> keep_alive_;
  std::unique_ptr<DownloadNotifications> notifications_;

  base::WeakPtrFactory<DownloadService> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_DOWNLOAD_SERVICE_H_
