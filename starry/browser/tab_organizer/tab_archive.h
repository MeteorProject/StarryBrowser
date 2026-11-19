// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_TAB_ORGANIZER_TAB_ARCHIVE_H_
#define STARRY_BROWSER_TAB_ORGANIZER_TAB_ARCHIVE_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "url/gurl.h"

class BrowserWindowInterface;
class Profile;

namespace base {
class SequencedTaskRunner;
}

namespace content {
class WebContents;
}

// 归档的标签：关掉标签，但存下它，能搜、能恢复。
namespace starry::tab_organizer {

// 为什么归档的。
enum class ArchiveReason {
  // 闲置太久（自动整理）。
  kIdle,
  // 和别的标签网址一样（自动整理）。
  kDuplicate,
  // 用户自己归档的（固定标签的「归档标签页」）。
  kManual,
  // AI 助手、智能体归档的（browser_tabs_manage）。
  kAssistant,
};

struct ArchivedTab {
  ArchivedTab();
  ArchivedTab(const ArchivedTab&);
  ArchivedTab(ArchivedTab&&);
  ArchivedTab& operator=(const ArchivedTab&);
  ArchivedTab& operator=(ArchivedTab&&);
  ~ArchivedTab();

  std::string id;
  GURL url;
  std::u16string title;
  // 归档前最后一次看它的时间、归档的时间。
  base::Time last_active;
  base::Time archived;
  ArchiveReason reason = ArchiveReason::kIdle;
  // 同一次整理里归档的一批（TabOrganizer 的 Batch，撤销用）。
  std::string batch;
  // 最近的前进后退（不存网页的状态），current 是当时在哪一条。一周以前归档的只留
  // 当前这一条。
  std::vector<sessions::SerializedNavigationEntry> navigations;
  int current = 0;
  // 会话里的附加数据（树状标签页的父子），恢复时交回去。
  std::map<std::string, std::string> extra_data;
  // 归档前网页的样子（标签预览存的 JPEG）：Capture 取出来，Add 时写进缩略图的
  // 目录，之后只记 thumbnail（有没有那个文件）。整页的「已归档」显示它。
  scoped_refptr<base::RefCountedBytes> thumbnail_data;
  bool thumbnail = false;
  // 归档后 AI 整理的模型打的主题（ArchiveTagger），能按它搜、整页能按它筛；还没打、打不
  // 了的是空的。
  std::u16string topic;
  // 问过模型（打上了、模型说没有合适的）：不再问。
  bool topic_tried = false;
};

// 一个用户资料（无痕窗口用原来那个的，但不往里放）一份，所有窗口共用。存在用户资料
// 目录的 `Tab Archive`（JSON），启动时在后台读进来。新的在前。缩略图在旁边的
// `Tab Archive Thumbnails/<id>.jpg`，只留最新的 kMaxThumbnails 个，条目拿掉时跟着删。
class TabArchive : public base::ImportantFileWriter::DataSerializer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // 加了、拿掉了、读进来了。
    virtual void OnArchiveChanged() = 0;
  };

  static TabArchive* ForProfile(Profile* profile);

  explicit TabArchive(const base::FilePath& path);
  TabArchive(const TabArchive&) = delete;
  TabArchive& operator=(const TabArchive&) = delete;
  ~TabArchive() override;

  // 从标签里取出要存的：网址、标题、最近的前进后退、树里的位置。reason、batch 由调用
  // 的人填（这里给了默认的）。
  static ArchivedTab Capture(content::WebContents* contents,
                             ArchiveReason reason);
  // 在 browser 里打开一个归档的标签（index 是 -1 或者超出时放在最后；不能放进固定
  // 区）。select：切过去，否则在后台、切过去才载入。
  static content::WebContents* Open(BrowserWindowInterface* browser,
                                    const ArchivedTab& tab,
                                    int index,
                                    bool select);

  void Add(std::vector<ArchivedTab> tabs);
  // 拿掉并返回（恢复的时候）。
  std::optional<ArchivedTab> Take(const std::string& id);
  void Remove(const std::vector<std::string>& ids);
  // 记下主题（ID → 主题，空的是模型没给），这些都记成问过了。
  void SetTopics(const std::map<std::string, std::u16string>& topics);
  void Clear();
  // 过期的删掉（retention 是 0 时一直留着），一周以前的只留当前这一条前进后退。
  void Expire(base::TimeDelta retention);

  bool loaded() const { return loaded_; }
  // 新的在前。
  const std::vector<ArchivedTab>& tabs() const { return tabs_; }
  const ArchivedTab* Find(const std::string& id) const;
  // 每个词（空白隔开，不分大小写）都出现在标题、网址或者主题里的，新的在前，最多 limit
  // 个。空的 query 是最新的几个。
  std::vector<const ArchivedTab*> Search(std::u16string_view query,
                                         size_t limit) const;

  // 在后台读这个标签的缩略图（JPEG）；没有时交回空的。
  void ReadThumbnail(
      const std::string& id,
      base::OnceCallback<void(scoped_refptr<base::RefCountedMemory>)> callback)
      const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // 调试用：每一条一行（标题、网址、原因、归档了多久、前进后退几条）。
  std::string DescribeForTesting() const;

 private:
  // base::ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

  void OnLoaded(std::optional<std::string> data);
  void Changed();
  base::FilePath ThumbnailPath(const std::string& id) const;
  // 新加的缩略图写下来，超出 kMaxThumbnails 的删掉。
  void StoreThumbnails();
  // 这些条目要拿掉了：有缩略图的删掉文件。
  void DropThumbnails(const std::vector<const ArchivedTab*>& tabs);
  // 读进来以后：目录里没有条目对应的缩略图删掉（上次没删成的）。
  void SweepThumbnails();

  base::FilePath path_;
  base::FilePath thumbnail_dir_;
  // 读写文件的后台序列（写的时候 ImportantFileWriter 也用它）。
  scoped_refptr<base::SequencedTaskRunner> file_runner_;
  base::ImportantFileWriter writer_;
  // 新的在前。
  std::vector<ArchivedTab> tabs_;
  bool loaded_ = false;
  // 还没读进来时就改了：读进来以后再写。
  bool dirty_before_load_ = false;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<TabArchive> weak_factory_{this};
};

}  // namespace starry::tab_organizer

#endif  // STARRY_BROWSER_TAB_ORGANIZER_TAB_ARCHIVE_H_
