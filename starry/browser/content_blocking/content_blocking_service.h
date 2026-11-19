// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_BLOCKING_SERVICE_H_
#define STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_BLOCKING_SERVICE_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "starry/components/content_filter/filter_engine.h"
#include "url/gurl.h"

namespace base {
class SequencedTaskRunner;
}

namespace starry::content_blocking {

class ListDownload;

// 内置的广告与跟踪拦截：订阅的过滤列表、自定义规则、不拦截的网站，以及列表的下载和
// 更新，把启用的规则合并进一个引擎（FilterEngine）并切换生效。整个浏览器共享一份实例，
// 普通窗口和无痕窗口共用；仅在界面线程访问。
//
// 文件都在用户数据目录的 `Starry Content Blocking/`：`State.json`（开关、订阅和它们的
// 更新情况、不拦截的网站）、`Lists/<订阅 ID>.txt`（展开了 !#include 的列表）、
// `CustomRules.txt`。引擎每次启动重新建（默认三份列表几十毫秒），不存编译结果。
class ContentBlockingService {
 public:
  enum class Category { kAds, kPrivacy, kAnnoyances, kCustom };

  struct Subscription {
    Subscription();
    Subscription(const Subscription&);
    Subscription& operator=(const Subscription&);
    ~Subscription();

    // 列表写的更新间隔（限制在 1 小时到 2 周，没写是 4 天）。
    base::TimeDelta UpdateInterval() const;
    // 该更新了：没取到过、或者过了更新间隔。上次失败的至少隔 1 小时再试。
    bool IsDue(base::Time now) const;

    std::string id;
    GURL url;
    // 自己添加的：列表的 `! Title:` 或者网站名。内置的用 title_key（界面文字）或者
    // 列表的名字（EasyList 这种不翻译）。
    std::string title;
    std::string title_key;
    std::string summary_key;
    GURL homepage;
    Category category = Category::kCustom;
    bool enabled = false;
    bool builtin = false;
    bool local = false;
    base::Time last_updated;
    base::Time last_attempt;
    std::optional<base::TimeDelta> expires;
    std::string etag;
    std::string last_modified;
    std::string version;
    // 上次更新失败的原因（界面文字）；成功后清掉。
    std::string last_error;
    // 列表内容的散列（内容变了才重建）。
    std::string content_hash;
    // Lists/<ID>.txt 在（下载过）。
    bool has_text = false;
    // 上次建引擎时这份列表合进来的、跳过的条数（has_stats 为 false 时还没建过）。
    bool has_stats = false;
    size_t rules = 0;
    size_t skipped = 0;
  };

  enum class Activity { kIdle, kDownloading, kBuilding };

  class Observer : public base::CheckedObserver {
   public:
    // 开关、订阅、更新进度、规则数、不拦截的网站……任何一样变了。
    virtual void OnContentBlockingChanged() = 0;
  };

  // 第一次调用时建好（StarryBrowserMainExtraParts::PreProfileInit 里 Start）。
  static ContentBlockingService* Get();

  ContentBlockingService(const ContentBlockingService&) = delete;
  ContentBlockingService& operator=(const ContentBlockingService&) = delete;

  // 读状态、建引擎、开始定时更新。恢复窗口之前调用；建好之前网页的请求先等着
  // （content_filter_proxy.h，最多等一会儿）。
  void Start();
  void Shutdown();

  bool enabled() const { return enabled_; }
  void SetEnabled(bool enabled);
  bool strip_tracking_parameters() const { return strip_tracking_parameters_; }
  void SetStripTrackingParameters(bool strip);

  const std::vector<Subscription>& subscriptions() const {
    return subscriptions_;
  }
  bool HasListText(const Subscription& subscription) const;
  void SetSubscriptionEnabled(const std::string& id, bool enabled);
  // 添加一个订阅：先下载，确认是过滤列表才加进来。callback 拿到失败的原因（界面文字），
  // 成功时是空的。
  void AddSubscription(const std::string& input,
                       base::OnceCallback<void(std::string error)> callback);
  // 只能移除自己添加的；内置的只能关掉。
  void RemoveSubscription(const std::string& id);
  void UpdateNow();

  const std::string& custom_rules() const { return custom_rules_; }
  void SetCustomRules(const std::string& text);
  size_t custom_rule_count() const { return custom_rule_count_; }
  size_t custom_skipped() const { return custom_skipped_; }

  // 不拦截的网站（去掉了 www.，按字母排）。
  const std::vector<std::string>& allowlist() const { return allowlist_; }
  bool IsAllowlisted(const GURL& url) const;
  // 记网站时去掉 `www.`，整个网站（包括子域名）都不拦；取消时连同包含它的上级一起去掉。
  void SetAllowlisted(const std::string& host, bool allowlisted);

  Activity activity() const { return activity_; }
  // 下载中：第几个（从 0 开始）、一共几个。
  int downloads_done() const { return downloads_done_; }
  int downloads_total() const { return downloads_total_; }
  size_t total_rules() const { return total_rules_; }
  base::Time last_build() const { return last_build_; }
  bool ready() const { return ready_; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  struct LoadedState;
  struct BuildResult;
  friend class base::NoDestructor<ContentBlockingService>;

  ContentBlockingService();
  ~ContentBlockingService();

  base::FilePath ListFile(const std::string& id) const;
  base::FilePath TextFile(const Subscription& subscription) const;
  Subscription* Find(const std::string& id);

  void OnStateLoaded(std::unique_ptr<LoadedState> state);
  base::DictValue SerializeState() const;
  void SaveState();
  void Changed();
  void PublishConfig();

  void ScheduleRebuild(base::TimeDelta delay);
  void Rebuild();
  void OnBuilt(int generation, std::unique_ptr<BuildResult> result);

  void StartUpdates();
  void StopUpdates();
  void CheckForUpdates(bool only_due);
  void DownloadNext();
  void Download(const std::string& id,
                base::OnceCallback<void(bool changed)> done);
  void OnSubscriptionTextReady(const std::string& id,
                               base::OnceCallback<void(bool changed)> done,
                               int status,
                               std::string text,
                               std::string etag,
                               std::string last_modified,
                               std::string error);

  base::FilePath directory_;
  scoped_refptr<base::SequencedTaskRunner> file_runner_;
  bool started_ = false;
  bool loaded_ = false;
  bool ready_ = false;
  bool enabled_ = true;
  bool strip_tracking_parameters_ = true;
  std::vector<Subscription> subscriptions_;
  std::vector<std::string> allowlist_;
  std::string custom_rules_;
  size_t custom_rule_count_ = 0;
  size_t custom_skipped_ = 0;
  size_t total_rules_ = 0;
  base::Time last_build_;
  // 本地列表；不为空时不联网、不存状态。
  std::vector<base::FilePath> local_lists_;
  bool automatic_updates_ = true;

  scoped_refptr<const content_filter::FilterEngine> engine_;
  int build_generation_ = 0;
  base::OneShotTimer rebuild_timer_;
  base::TimeTicks build_started_;

  Activity activity_ = Activity::kIdle;
  int downloads_done_ = 0;
  int downloads_total_ = 0;
  // 排着队要下载的订阅（一个一个下）。
  std::vector<std::string> download_queue_;
  bool queue_running_ = false;
  bool queue_changed_ = false;
  // 正在下载的（排队的和「添加订阅」的）。
  std::map<std::string, std::unique_ptr<ListDownload>> downloads_;
  base::OneShotTimer startup_update_timer_;
  base::RepeatingTimer update_timer_;

  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<ContentBlockingService> weak_factory_{this};
};

}  // namespace starry::content_blocking

#endif  // STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_BLOCKING_SERVICE_H_
