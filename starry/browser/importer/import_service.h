// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_IMPORTER_IMPORT_SERVICE_H_
#define STARRY_BROWSER_IMPORTER_IMPORT_SERVICE_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/scoped_observation.h"
#include "base/supports_user_data.h"
#include "base/task/cancelable_task_tracker.h"
#include "chrome/browser/importer/importer_progress_observer.h"
#include "components/bookmarks/browser/base_bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/user_data_importer/common/importer_data_types.h"
#include "starry/browser/importer/chromium_reader.h"
#include "starry/browser/importer/import_source.h"

class ExternalProcessImporterHost;
class ImporterList;
class Profile;

namespace starry {

class CountingProfileWriter;

// 一个用户资料的「导入浏览器数据」（设置 › 通用）：找出本机的其他浏览器，把选的那个
// 的书签、历史记录、密码、搜索引擎、表单的自动填充导入进来。
//
// 从哪导入：
// - Chromium 系的浏览器（chromium_browsers.h）：Starry 自己读它的文件（chromium_reader.h），
//   在线程池里读，读完在界面线程上交给 Chrome 的 ProfileWriter 写进来；
// - Firefox、Safari、书签文件（HTML）这些 Chrome 的导入器认得的：交给
//   ExternalProcessImporterHost，在工具进程里读（同 Chrome 设置页的 ImportDataHandler）。
// 同一时间只导入一个；导入在用户资料上，设置页关了也会做完，再打开时看得到结果。
// 历史记录再导入一次不会重复（同一个网址、同一时刻的访问跳过），书签会再放一个文件夹
// （同 Chrome）。
class ImportService : public base::SupportsUserData::Data,
                      public importer::ImporterProgressObserver,
                      public bookmarks::BaseBookmarkModelObserver {
 public:
  enum class Status { kRunning, kDone, kFailed };

  // 一次导入的进度和结果。
  struct Job {
    Job();
    Job(const Job&);
    Job& operator=(const Job&);
    ~Job();

    std::string source_id;
    // 浏览器（和用户资料）的名字，书签文件是文件名。
    std::u16string source_name;
    // 要导入的几样：user_data_importer::ImportItem 的位。
    uint16_t items = 0;
    Status status = Status::kRunning;
    // 正在读的一样（kRunning 时；还没开始读时是 NONE）。
    user_data_importer::ImportItem current = user_data_importer::NONE;
    // 导入了多少。
    size_t bookmarks = 0;
    size_t history = 0;
    size_t passwords = 0;
    size_t search_engines = 0;
    size_t autofill = 0;
    // 解不开的密码有几个；拿不到密码的密钥（macOS 的钥匙串没有允许）。
    size_t undecryptable_passwords = 0;
    bool password_key_unavailable = false;
    // 读不出来的几样。
    uint16_t failed_items = 0;
    // kFailed 时为什么：firefoxRunning（Firefox 开着，它的用户资料被锁住了）、
    // unreadable（一样也没读出来）。
    std::string error;
  };

  class Observer : public base::CheckedObserver {
   public:
    // 来源、进度、结果有变化。
    virtual void OnImportChanged() = 0;
  };

  // 这个用户资料的（无痕的换成原来的），第一次用时建。
  static ImportService* Get(Profile* profile);

  explicit ImportService(Profile* profile);
  ImportService(const ImportService&) = delete;
  ImportService& operator=(const ImportService&) = delete;
  ~ImportService() override;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // 找本机的其他浏览器（在后台读文件，找完通知）。正在找时不重来。
  void DetectSources();
  bool detecting() const { return pending_detections_ > 0; }
  bool detected() const { return detected_; }
  // Chromium 系的按最近用过的在前，然后是 Firefox、Safari 这些。
  const std::vector<ImportSource>& sources() const { return sources_; }
  // macOS：用户没给「完全磁盘访问权限」，读不了 Safari 的书签。
  bool safari_needs_access() const { return safari_needs_access_; }

  const std::optional<Job>& job() const { return job_; }
  bool running() const { return job_ && job_->status == Status::kRunning; }
  // 从 sources() 里的这一个导入 items（只导入它有的）。正在导入时、没有这个来源时回
  // false。
  bool Start(const std::string& source_id, uint16_t items);
  // 导入一个书签文件（别的浏览器导出的 HTML）。
  bool StartBookmarksFile(const base::FilePath& file);
  // 收起上一次的结果。
  void ClearJob();

  // importer::ImporterProgressObserver:
  void ImportStarted() override;
  void ImportItemStarted(user_data_importer::ImportItem item) override;
  void ImportItemEnded(user_data_importer::ImportItem item) override;
  void ImportEnded() override;

  // bookmarks::BaseBookmarkModelObserver:
  void BookmarkModelLoaded(bool ids_reassigned) override;
  void BookmarkModelBeingDeleted() override;
  void BookmarkModelChanged() override {}

 private:
  void OnChromiumDetected(std::vector<ImportSource> sources);
  void OnBuiltinDetected();
  void DetectionDone();

  void StartChromium(const ImportSource& source, uint16_t items);
  void OnChromiumItemStarted(int generation,
                             user_data_importer::ImportItem item);
  void OnChromiumRead(int generation, ChromiumImportData data);
  // 书签、搜索引擎的模型载入了才能写。
  void WriteWhenReady();
  void WriteChromiumData(ChromiumImportData data);
  void OnHistoryWritten(int generation, size_t added);

  void StartBuiltin(const user_data_importer::SourceProfile& profile,
                    uint16_t items);
  void Finish();
  void NotifyChanged();

  const raw_ptr<Profile> profile_;
  base::ObserverList<Observer> observers_;

  int pending_detections_ = 0;
  bool detected_ = false;
  // 正在找的这一轮里已经找到的（Chromium 系的和 Chrome 的导入器认得的，谁先找完都行）。
  std::vector<ImportSource> found_sources_;
  std::unique_ptr<ImporterList> importer_list_;
  std::vector<ImportSource> sources_;
  bool safari_needs_access_ = false;

  std::optional<Job> job_;
  // 每开始一次加一：上一次的回调晚到时认得出来。
  int generation_ = 0;
  // Chromium 系的：读出来、等着模型载入后写的。
  std::optional<ChromiumImportData> pending_data_;
  base::ScopedObservation<bookmarks::BookmarkModel,
                          bookmarks::BaseBookmarkModelObserver>
      bookmark_observation_{this};
  base::CallbackListSubscription template_url_subscription_;
  base::CancelableTaskTracker history_tracker_;
  // Chrome 的导入器：自己删自己（ImportEnded 之后），这里只记着。
  raw_ptr<ExternalProcessImporterHost> importer_host_ = nullptr;
  scoped_refptr<CountingProfileWriter> counting_writer_;
  bool builtin_item_ended_ = false;

  base::WeakPtrFactory<ImportService> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_IMPORTER_IMPORT_SERVICE_H_
