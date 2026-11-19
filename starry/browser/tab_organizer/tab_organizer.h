// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_TAB_ORGANIZER_TAB_ORGANIZER_H_
#define STARRY_BROWSER_TAB_ORGANIZER_TAB_ORGANIZER_H_

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "components/sessions/core/session_id.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/tab_organizer/tab_archive.h"

class BrowserWindowInterface;
class GURL;
class Profile;

namespace content {
class WebContents;
}

// 标签管理：自动整理（闲置的、重复的普通标签在用户不看的时候
// 归档）、整理的记录和撤销。助手的 browser_tabs_manage 也经这里归档。
namespace starry::tab_organizer {

class ArchiveTagger;

// 设置 › 标签管理（prefs::kAutoArchive……）。
struct Settings {
  Settings();
  Settings(const Settings&);
  Settings& operator=(const Settings&);
  ~Settings();

  bool auto_archive = true;
  base::TimeDelta idle = base::Days(7);
  bool duplicates = true;
  // 不整理的网站（小写的主机名，含子域名）。
  std::vector<std::string> exceptions;
  // 归档留多久，0 是一直留着。
  base::TimeDelta retention = base::Days(90);
};
Settings ReadSettings(Profile* profile);
// 网址在「不整理的网站」里（含子域名）。
bool IsExcepted(const GURL& url, const Settings& settings);

// 谁做的整理（提示里写）。
enum class Source {
  kAuto,
  kAssistant,
  // 一键整理（侧栏的整理页）。
  kOrganize,
};

// 放进一个文件夹的一组标签（一键整理、助手的 move_to_folder）。
struct FolderGroup {
  FolderGroup();
  FolderGroup(const FolderGroup&);
  FolderGroup& operator=(const FolderGroup&);
  ~FolderGroup();

  // 新建时的名字。
  std::u16string name;
  // 已有的文件夹（pin_folders 的 ID）；空的是在固定区的最外层新建一个。
  std::string folder;
  std::vector<tabs::TabHandle> tabs;
};

// 一次整理做的事，整批撤销。
struct Batch {
  Batch();
  Batch(const Batch&);
  Batch& operator=(const Batch&);
  ~Batch();

  // 归档的一个标签：归档里的 ID，原来在哪个窗口、第几个。
  struct Item {
    std::string archive_id;
    SessionID window = SessionID::InvalidValue();
    int index = 0;
  };

  // 放进文件夹的一个标签（一键整理、助手的 move_to_folder）。
  struct Move {
    tabs::TabHandle tab;
    SessionID window = SessionID::InvalidValue();
    std::string folder;
  };
  // 放之前一个窗口里的普通标签：顺序和每个的父标签（在 tabs 里的下标，-1 是最外层）。
  // 撤销时按它放回原来的位置、挂回原来的父标签下面。
  struct Placement {
    Placement();
    Placement(const Placement&);
    Placement& operator=(const Placement&);
    ~Placement();

    SessionID window = SessionID::InvalidValue();
    std::vector<tabs::TabHandle> tabs;
    std::vector<int> parents;
  };

  std::string id;
  Source source = Source::kAuto;
  base::Time time;
  std::vector<Item> items;
  std::vector<Move> moves;
  std::vector<Placement> placements;
  // 用到的文件夹（含已有的）、其中新建的（撤销时空了就拿掉）。
  std::vector<std::string> folders;
  std::vector<std::string> created_folders;
  // 关掉的标签有几个（含没存进归档的空白标签页）、其中闲置的、重复的各几个。
  int closed = 0;
  int idle = 0;
  int duplicates = 0;
  bool undone = false;
};

class TabOrganizer : public ProfileObserver {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // 做完一批（自动整理、助手）。
    virtual void OnBatchDone(const Batch& batch) {}
    // 一批撤销了。
    virtual void OnBatchUndone(const Batch& batch) {}
  };

  // 一个用户资料一个（无痕窗口用原来那个的；无痕窗口里的标签不整理）。
  static TabOrganizer* ForProfile(Profile* profile);

  explicit TabOrganizer(Profile* profile);
  TabOrganizer(const TabOrganizer&) = delete;
  TabOrganizer& operator=(const TabOrganizer&) = delete;
  ~TabOrganizer() override;

  // 有窗口了（TabsView）：开始定时看（第一次在 1 分钟后，之后每 5 分钟），归档时打主题。
  void Start();
  // 给归档打主题的（Start 以后才有）。
  ArchiveTagger* tagger() { return tagger_.get(); }
  // 现在看一遍（定时到了也是它）。force：不管用户在不在看那个窗口（调试用）。
  void RunNow(bool force);

  // 这个窗口里现在会被自动整理的标签和原因（按设置；不管用户在不在看）。
  std::vector<std::pair<tabs::TabInterface*, ArchiveReason>> FindCandidates(
      BrowserWindowInterface* browser,
      const Settings& settings) const;
  // 把这些标签归档成一批（可以在不同的窗口里）。网页有离开前要问的事件时先休眠再关
  // （不弹「离开此网站？」），休眠不了的跳过。空白标签页直接关、不进归档。返回这一
  // 批；一个都没关时为空。
  const Batch* Archive(
      const std::vector<std::pair<tabs::TabInterface*, ArchiveReason>>& tabs,
      Source source);
  // 把几组标签放进固定区的文件夹（标签固定下来），记成一批。新建的文件夹排在最外层的最后、
  // 折着（collapse_new）。已经固定的、关掉了的跳过。一个都没放时为空。
  const Batch* MoveToFolders(const std::vector<FolderGroup>& groups,
                             Source source,
                             bool collapse_new);
  // 把一批放回去。归档的：放回原来的窗口、原来的位置（窗口关了就放进最近用的窗口），
  // 在后台、切过去才载入，已经从归档里拿掉的（恢复过、清掉了）跳过。放进文件夹的：
  // 取消固定、放回原来的位置和父标签下面，用户之后又挤过的（不在那个文件夹里了）不动；
  // 新建的文件夹空了就拿掉。
  bool Undo(const std::string& batch_id);
  // 最近的一批（没有时为空）。
  const Batch* LastBatch() const;
  const Batch* FindBatch(const std::string& id) const;

  // 恢复归档里的一个：在 browser 的最后打开，select 时切过去。
  content::WebContents* Restore(BrowserWindowInterface* browser,
                                const std::string& archive_id,
                                bool select);

  // 这个标签多久没看了：最后一次切到它、最后一次看到它（每次定时记下显示着的）里晚
  // 的那个到现在。
  base::TimeDelta IdleFor(tabs::TabInterface* tab) const;
  // 这个标签为什么不能自动整理（固定的、显示着的、放着声音……），能的话为空。
  static const char* SkipReason(tabs::TabInterface* tab,
                                const Settings& settings);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // 调试用：覆写该标签的最后查看时间。
  static void SetLastSeenForTesting(tabs::TabInterface* tab, base::Time time);
  // 调试用：设置、每个标签（闲置多久、为什么不整理、会不会被整理）、最近几批。
  std::string DescribeForTesting(BrowserWindowInterface* browser) const;

 private:
  void OnProfileWillBeDestroyed(Profile* profile) override;
  void Tick();
  void UndoMoves(const Batch& batch);
  // 记下显示着的标签「刚看过」。
  void MarkSeen();
  // 用户没在看这个窗口：不在前台，或者用户闲置超过 1 分钟。
  bool Quiet(BrowserWindowInterface* browser) const;
  // 这个用户资料的普通窗口（不含无痕窗口、智能体的无痕窗口、小窗口）。
  std::vector<BrowserWindowInterface*> Windows() const;

  raw_ptr<Profile> profile_;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  bool started_ = false;
  std::unique_ptr<ArchiveTagger> tagger_;
  base::OneShotTimer first_timer_;
  base::RepeatingTimer timer_;
  // 最近的几批，新的在后。
  std::deque<Batch> batches_;
  base::ObserverList<Observer> observers_;
};

}  // namespace starry::tab_organizer

#endif  // STARRY_BROWSER_TAB_ORGANIZER_TAB_ORGANIZER_H_
