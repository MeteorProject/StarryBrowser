// Copyright 2026 The Starry Browser Authors

#include "starry/browser/tab_organizer/tab_organizer.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_contents/form_interaction_tab_helper.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/agent/agent_activity.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/tab_organizer/archive_tagger.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "starry/browser/ui/tabs/tab_list_view.h"
#include "starry/browser/ui/tabs/tab_tree.h"
#include "starry/browser/ui/tabs/tabs_view.h"
#include "starry/common/internal_urls.h"
#include "ui/base/base_window.h"
#include "ui/base/idle/idle.h"
#include "url/gurl.h"

namespace starry::tab_organizer {

namespace {

constexpr char kUserDataKey[] = "starry.tab_organizer";
// 启动后多久第一次看、之后隔多久看一次。
constexpr base::TimeDelta kFirstDelay = base::Minutes(1);
constexpr base::TimeDelta kInterval = base::Minutes(5);
// 用户闲置多久算没在看前台的窗口。
constexpr base::TimeDelta kQuietIdle = base::Seconds(60);
// 重复的标签闲置多久才归档（刚复制出来的不动）。
constexpr base::TimeDelta kDuplicateIdle = base::Hours(1);
// 记着最近几批（撤销用）。
constexpr size_t kMaxBatches = 20;

class OrganizerHolder : public base::SupportsUserData::Data {
 public:
  explicit OrganizerHolder(Profile* profile) : organizer(profile) {}
  TabOrganizer organizer;
};

// 最后一次看到这个标签（定时记下显示着的）。Chrome 的 GetLastActiveTime 是切过去的那一
// 刻，一个标签看了好几天、刚切走时它还是好几天前。
class LastSeen : public content::WebContentsUserData<LastSeen> {
 public:
  ~LastSeen() override = default;

  base::Time time;
  // 调试覆写的时间：不与 GetLastActiveTime 比较，直到标签再次显示。
  bool forced = false;

 private:
  friend class content::WebContentsUserData<LastSeen>;
  explicit LastSeen(content::WebContents* contents)
      : content::WebContentsUserData<LastSeen>(*contents) {}
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(LastSeen);

std::string NewId() {
  return base::HexEncode(base::RandBytesAsVector(6));
}

// 同标签的右键菜单（tab_menu.cc）：在录音、录像、共享屏幕。
bool IsCapturing(tabs::TabInterface* tab) {
  const tabs::TabData data = tabs::TabData::FromTabInterface(tab);
  if (!data.alert_state) {
    return false;
  }
  switch (*data.alert_state) {
    case tabs::TabAlert::kMediaRecording:
    case tabs::TabAlert::kTabCapturing:
    case tabs::TabAlert::kDesktopCapturing:
    case tabs::TabAlert::kAudioRecording:
    case tabs::TabAlert::kVideoRecording:
      return true;
    default:
      return false;
  }
}

// 「不整理的网站」里的一项：主机名（可以写成网址），去掉开头的 www.。
std::string NormalizeSite(const std::string& site) {
  std::string text(base::TrimWhitespaceASCII(site, base::TRIM_ALL));
  if (text.find("://") != std::string::npos) {
    text = GURL(text).host();
  }
  text = base::ToLowerASCII(text);
  if (base::StartsWith(text, "www.")) {
    text = text.substr(4);
  }
  return text;
}

// 比较是不是同一个网页：只看网页（http、https、file），不算 # 后面的。
std::string DuplicateKey(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS() && !url.SchemeIsFile()) {
    return std::string();
  }
  GURL::Replacements replacements;
  replacements.ClearRef();
  return url.ReplaceComponents(replacements).spec();
}

// 空白标签页、新标签页不进归档（直接关）。
bool ShouldStore(const GURL& url) {
  if (url.is_empty() || !url.is_valid() || url.IsAboutBlank()) {
    return false;
  }
  return url.host() != "newtab" && url.host() != "new-tab-page";
}

const char* ReasonName(ArchiveReason reason) {
  switch (reason) {
    case ArchiveReason::kIdle:
      return "idle";
    case ArchiveReason::kDuplicate:
      return "duplicate";
    case ArchiveReason::kManual:
      return "manual";
    case ArchiveReason::kAssistant:
      return "assistant";
  }
  return "idle";
}

const char* SourceName(Source source) {
  switch (source) {
    case Source::kAuto:
      return "auto";
    case Source::kAssistant:
      return "assistant";
    case Source::kOrganize:
      return "organize";
  }
  return "auto";
}

}  // namespace

bool IsExcepted(const GURL& url, const Settings& settings) {
  const std::string host = base::ToLowerASCII(url.host());
  return std::ranges::any_of(settings.exceptions, [&](const std::string& site) {
    return !site.empty() &&
           (host == site || base::EndsWith(host, base::StrCat({".", site})));
  });
}

FolderGroup::FolderGroup() = default;
FolderGroup::FolderGroup(const FolderGroup&) = default;
FolderGroup& FolderGroup::operator=(const FolderGroup&) = default;
FolderGroup::~FolderGroup() = default;

Batch::Placement::Placement() = default;
Batch::Placement::Placement(const Placement&) = default;
Batch::Placement& Batch::Placement::operator=(const Placement&) = default;
Batch::Placement::~Placement() = default;

Settings::Settings() = default;
Settings::Settings(const Settings&) = default;
Settings& Settings::operator=(const Settings&) = default;
Settings::~Settings() = default;

Settings ReadSettings(Profile* profile) {
  PrefService* prefs = prefs::PrefsFor(profile);
  Settings settings;
  settings.auto_archive = prefs->GetBoolean(prefs::kAutoArchive);
  settings.idle =
      base::Hours(std::max(1, prefs->GetInteger(prefs::kAutoArchiveHours)));
  settings.duplicates = prefs->GetBoolean(prefs::kArchiveDuplicates);
  for (const base::Value& site : prefs->GetList(prefs::kOrganizerExceptions)) {
    if (site.is_string()) {
      std::string normalized = NormalizeSite(site.GetString());
      if (!normalized.empty()) {
        settings.exceptions.push_back(std::move(normalized));
      }
    }
  }
  const int days = prefs->GetInteger(prefs::kArchiveRetentionDays);
  settings.retention = days > 0 ? base::Days(days) : base::TimeDelta();
  return settings;
}

Batch::Batch() = default;
Batch::Batch(const Batch&) = default;
Batch& Batch::operator=(const Batch&) = default;
Batch::~Batch() = default;

// static
TabOrganizer* TabOrganizer::ForProfile(Profile* profile) {
  Profile* original = profile->GetOriginalProfile();
  auto* holder =
      static_cast<OrganizerHolder*>(original->GetUserData(kUserDataKey));
  if (!holder) {
    auto created = std::make_unique<OrganizerHolder>(original);
    holder = created.get();
    original->SetUserData(kUserDataKey, std::move(created));
  }
  return &holder->organizer;
}

TabOrganizer::TabOrganizer(Profile* profile) : profile_(profile) {}

TabOrganizer::~TabOrganizer() = default;

void TabOrganizer::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  profile_observation_.Observe(profile_);
  // 归档在这时开始读进来。
  TabArchive::ForProfile(profile_);
  tagger_ = std::make_unique<ArchiveTagger>(profile_);
  first_timer_.Start(FROM_HERE, kFirstDelay,
                     base::BindOnce(&TabOrganizer::Tick, base::Unretained(this)));
  timer_.Start(FROM_HERE, kInterval,
               base::BindRepeating(&TabOrganizer::Tick, base::Unretained(this)));
}

void TabOrganizer::OnProfileWillBeDestroyed(Profile* profile) {
  // The organizer and archive are separate profile user-data entries. Detach
  // the tagger while both are alive, before ClearAllUserData destroys either.
  first_timer_.Stop();
  timer_.Stop();
  tagger_.reset();
  profile_observation_.Reset();
}

void TabOrganizer::Tick() {
  RunNow(/*force=*/false);
}

void TabOrganizer::RunNow(bool force) {
  MarkSeen();
  const Settings settings = ReadSettings(profile_);
  TabArchive* archive = TabArchive::ForProfile(profile_);
  if (archive->loaded()) {
    archive->Expire(settings.retention);
  }
  std::vector<std::pair<tabs::TabInterface*, ArchiveReason>> all;
  for (BrowserWindowInterface* browser : Windows()) {
    if (!force && !Quiet(browser)) {
      continue;
    }
    std::ranges::move(FindCandidates(browser, settings),
                      std::back_inserter(all));
  }
  if (!all.empty()) {
    Archive(all, Source::kAuto);
  }
}

std::vector<BrowserWindowInterface*> TabOrganizer::Windows() const {
  std::vector<BrowserWindowInterface*> windows;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetProfile() == profile_ &&
        browser->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
      windows.push_back(browser);
    }
  }
  return windows;
}

bool TabOrganizer::Quiet(BrowserWindowInterface* browser) const {
  ui::BaseWindow* window = browser->GetWindow();
  if (!window || !window->IsActive() || window->IsMinimized()) {
    return true;
  }
  return base::Seconds(ui::CalculateIdleTime()) >= kQuietIdle;
}

void TabOrganizer::MarkSeen() {
  const base::Time now = base::Time::Now();
  for (BrowserWindowInterface* browser : Windows()) {
    ui::BaseWindow* window = browser->GetWindow();
    if (!window || !window->IsVisible() || window->IsMinimized()) {
      continue;
    }
    TabStripModel* model = browser->GetTabStripModel();
    for (int i = 0; i < model->count(); ++i) {
      content::WebContents* contents = model->GetWebContentsAt(i);
      if (contents->GetVisibility() != content::Visibility::VISIBLE) {
        continue;
      }
      LastSeen::CreateForWebContents(contents);
      LastSeen* seen = LastSeen::FromWebContents(contents);
      seen->time = now;
      seen->forced = false;
    }
  }
}

base::TimeDelta TabOrganizer::IdleFor(tabs::TabInterface* tab) const {
  content::WebContents* contents = tab->GetContents();
  base::Time last = contents->GetLastActiveTime();
  if (const LastSeen* seen = LastSeen::FromWebContents(contents)) {
    last = seen->forced ? seen->time : std::max(last, seen->time);
  }
  return std::max(base::TimeDelta(), base::Time::Now() - last);
}

// static
const char* TabOrganizer::SkipReason(tabs::TabInterface* tab,
                                     const Settings& settings) {
  if (tab->IsPinned()) {
    return "pinned";
  }
  if (tab->IsActivated()) {
    return "active";
  }
  if (tab->IsSplit()) {
    return "split";
  }
  content::WebContents* contents = tab->GetContents();
  // 浏览器自己的页面（设置、历史记录……）不动；新标签页照旧闲置了就关。
  if (const GURL& url = contents->GetVisibleURL();
      IsInternalURL(url) && ShouldStore(url)) {
    return "internal";
  }
  if (contents->GetVisibility() == content::Visibility::VISIBLE) {
    return "visible";
  }
  if (contents->IsCurrentlyAudible()) {
    return "audible";
  }
  if (IsCapturing(tab)) {
    return "capturing";
  }
  if (contents->HasPictureInPictureVideo() ||
      contents->HasPictureInPictureDocument()) {
    return "pip";
  }
  if (agent::AgentActivities::Get()->For(tab)) {
    return "agent";
  }
  if (FormInteractionTabHelper* form = FormInteractionTabHelper::From(tab);
      form && form->had_form_interaction()) {
    return "form";
  }
  if (IsExcepted(contents->GetVisibleURL(), settings)) {
    return "exception";
  }
  return nullptr;
}

std::vector<std::pair<tabs::TabInterface*, ArchiveReason>>
TabOrganizer::FindCandidates(BrowserWindowInterface* browser,
                             const Settings& settings) const {
  std::vector<std::pair<tabs::TabInterface*, ArchiveReason>> result;
  if (!settings.auto_archive && !settings.duplicates) {
    return result;
  }
  // 同一个网页留哪一个（这个用户资料的所有普通窗口里）：固定的、显示着的优先，再是最
  // 近看过的。
  std::map<std::string, tabs::TabInterface*> keepers;
  if (settings.duplicates) {
    auto rank = [&](tabs::TabInterface* tab) {
      const bool shown = tab->IsActivated() ||
                         tab->GetContents()->GetVisibility() ==
                             content::Visibility::VISIBLE;
      return std::make_tuple(tab->IsPinned(), shown, -IdleFor(tab));
    };
    for (BrowserWindowInterface* window : Windows()) {
      TabStripModel* model = window->GetTabStripModel();
      for (int i = 0; i < model->count(); ++i) {
        tabs::TabInterface* tab = model->GetTabAtIndex(i);
        const std::string key =
            DuplicateKey(tab->GetContents()->GetVisibleURL());
        if (key.empty()) {
          continue;
        }
        auto [it, inserted] = keepers.try_emplace(key, tab);
        if (!inserted && rank(tab) > rank(it->second)) {
          it->second = tab;
        }
      }
    }
  }

  TabStripModel* model = browser->GetTabStripModel();
  const int first = model->IndexOfFirstNonPinnedTab();
  const int count = model->count();
  std::vector<std::optional<ArchiveReason>> want(count);
  for (int i = first; i < count; ++i) {
    tabs::TabInterface* tab = model->GetTabAtIndex(i);
    if (SkipReason(tab, settings)) {
      continue;
    }
    const base::TimeDelta idle = IdleFor(tab);
    if (settings.auto_archive && idle >= settings.idle) {
      want[i] = ArchiveReason::kIdle;
      continue;
    }
    if (settings.duplicates && idle >= kDuplicateIdle) {
      const std::string key = DuplicateKey(tab->GetContents()->GetVisibleURL());
      auto it = keepers.find(key);
      if (!key.empty() && it != keepers.end() && it->second != tab) {
        want[i] = ArchiveReason::kDuplicate;
      }
    }
  }
  // 树里的父标签：整枝都要整理才整理它（从后往前，子孙先定下来）。
  for (int i = count - 1; i >= first; --i) {
    if (!want[i]) {
      continue;
    }
    const int end = tab_tree::BranchEnd(model, i);
    for (int j = i + 1; j < end && j < count; ++j) {
      if (!want[j]) {
        want[i].reset();
        break;
      }
    }
  }
  for (int i = first; i < count; ++i) {
    if (want[i]) {
      result.emplace_back(model->GetTabAtIndex(i), *want[i]);
    }
  }
  return result;
}

const Batch* TabOrganizer::Archive(
    const std::vector<std::pair<tabs::TabInterface*, ArchiveReason>>& tabs,
    Source source) {
  struct Pending {
    tabs::TabHandle handle;
    ArchiveReason reason;
    SessionID window = SessionID::InvalidValue();
    int index = 0;
  };
  std::vector<Pending> pending;
  for (const auto& [tab, reason] : tabs) {
    BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
    if (!browser || browser->GetProfile() != profile_) {
      continue;
    }
    pending.push_back({tab->GetHandle(), reason, browser->GetSessionID(),
                       browser->GetTabStripModel()->GetIndexOfTab(tab)});
  }
  // 从后往前关：同一个窗口里前面的下标不变，子标签先于父标签关掉。
  std::ranges::sort(pending, std::ranges::greater(), &Pending::index);

  Batch batch;
  batch.id = NewId();
  batch.source = source;
  batch.time = base::Time::Now();
  std::vector<ArchivedTab> archived;
  for (const Pending& item : pending) {
    tabs::TabInterface* tab = item.handle.Get();
    if (!tab) {
      continue;
    }
    BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
    TabStripModel* model = browser->GetTabStripModel();
    // 不关窗口里的最后一个标签（不然窗口跟着关了）。
    if (model->count() <= 1) {
      continue;
    }
    content::WebContents* contents = tab->GetContents();
    std::optional<ArchivedTab> entry;
    if (ShouldStore(contents->GetVisibleURL())) {
      entry = TabArchive::Capture(contents, item.reason);
      entry->batch = batch.id;
      entry->last_active = batch.time - IdleFor(tab);
    }
    // 网页有离开前要问的事件（beforeunload、unload）：先休眠，关的时候就不会弹「离开此
    // 网站？」。填过表单的上面已经跳过了。
    if (contents->NeedToFireBeforeUnloadOrUnloadEvents() &&
        !contents->WasDiscarded()) {
      TabListInterface* list = TabListInterface::From(browser);
      if (!list || !list->DiscardTab(item.handle)) {
        continue;
      }
      tab = item.handle.Get();
      if (!tab) {
        continue;
      }
    }
    const int index = model->GetIndexOfTab(tab);
    if (index == TabStripModel::kNoTab) {
      continue;
    }
    model->CloseWebContentsAt(index, TabCloseTypes::CLOSE_NONE);
    ++batch.closed;
    if (item.reason == ArchiveReason::kDuplicate) {
      ++batch.duplicates;
    } else {
      ++batch.idle;
    }
    if (entry) {
      batch.items.push_back({entry->id, item.window, item.index});
      archived.push_back(std::move(*entry));
    }
  }
  if (batch.closed == 0) {
    return nullptr;
  }
  TabArchive::ForProfile(profile_)->Add(std::move(archived));
  batches_.push_back(std::move(batch));
  if (batches_.size() > kMaxBatches) {
    batches_.pop_front();
  }
  const Batch& done = batches_.back();
  observers_.Notify(&Observer::OnBatchDone, done);
  return &done;
}

const Batch* TabOrganizer::MoveToFolders(const std::vector<FolderGroup>& groups,
                                         Source source,
                                         bool collapse_new) {
  // 要动的标签在哪些窗口：先记下这些窗口里普通标签原来的顺序和父子。
  std::vector<BrowserWindowInterface*> windows;
  for (const FolderGroup& group : groups) {
    for (const tabs::TabHandle& handle : group.tabs) {
      tabs::TabInterface* tab = handle.Get();
      BrowserWindowInterface* browser =
          tab ? tab->GetBrowserWindowInterface() : nullptr;
      if (browser && browser->GetProfile() == profile_ && !tab->IsPinned() &&
          std::ranges::find(windows, browser) == windows.end()) {
        windows.push_back(browser);
      }
    }
  }
  if (windows.empty()) {
    return nullptr;
  }
  Batch batch;
  batch.id = NewId();
  batch.source = source;
  batch.time = base::Time::Now();
  // 几处一起改：列表只在最后重排一次。
  std::vector<TabsView*> views;
  for (BrowserWindowInterface* browser : windows) {
    TabStripModel* model = browser->GetTabStripModel();
    const std::vector<tab_tree::Node> nodes = tab_tree::Compute(model);
    const int first = model->IndexOfFirstNonPinnedTab();
    Batch::Placement placement;
    placement.window = browser->GetSessionID();
    for (int i = first; i < model->count(); ++i) {
      placement.tabs.push_back(model->GetTabAtIndex(i)->GetHandle());
      const int parent =
          static_cast<size_t>(i) < nodes.size() ? nodes[i].parent : -1;
      placement.parents.push_back(parent >= first ? parent - first : -1);
    }
    batch.placements.push_back(std::move(placement));
    if (TabsView* view = pin_folders::VisibleTabs(browser)) {
      view->BeginBatch();
      views.push_back(view);
    }
  }
  std::set<std::string> existing;
  for (const pin_folders::Folder& folder : pin_folders::Read(profile_)) {
    existing.insert(folder.id);
  }
  for (const FolderGroup& group : groups) {
    std::vector<tabs::TabInterface*> tabs;
    for (const tabs::TabHandle& handle : group.tabs) {
      tabs::TabInterface* tab = handle.Get();
      if (tab && !tab->IsPinned() &&
          std::ranges::find(windows, tab->GetBrowserWindowInterface()) !=
              windows.end() &&
          std::ranges::find(tabs, tab) == tabs.end()) {
        tabs.push_back(tab);
      }
    }
    if (tabs.empty()) {
      continue;
    }
    // 要放进去的文件夹可能刚在别处删了：新建一个。
    std::string folder =
        existing.contains(group.folder) ? group.folder : std::string();
    if (folder.empty()) {
      std::u16string name;
      base::TrimWhitespace(group.name, base::TRIM_ALL, &name);
      if (name.empty()) {
        continue;
      }
      folder = pin_folders::Add(profile_, std::string(), name);
      existing.insert(folder);
      batch.created_folders.push_back(folder);
      // 排在最外层的最后（不然按名字排在没排过的那些里）。
      pin_folders::Place(tabs.front()->GetBrowserWindowInterface(),
                         pin_folders::Child{tabs::TabHandle(), folder},
                         std::string());
      if (collapse_new) {
        pin_folders::SetCollapsed(profile_, folder, true);
      }
    }
    for (tabs::TabInterface* tab : tabs) {
      BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
      pin_folders::Place(
          browser, pin_folders::Child{tab->GetHandle(), std::string()}, folder);
      // 固定不了的（没有网址可记）留在列表里。
      if (tab->IsPinned()) {
        batch.moves.push_back(
            {tab->GetHandle(), browser->GetSessionID(), folder});
      }
    }
    if (std::ranges::find(batch.folders, folder) == batch.folders.end()) {
      batch.folders.push_back(folder);
    }
  }
  for (TabsView* view : views) {
    view->EndBatch(TabListView::Change::kFold);
  }
  if (batch.moves.empty()) {
    for (const std::string& id : batch.created_folders) {
      pin_folders::Remove(profile_, id);
    }
    return nullptr;
  }
  batches_.push_back(std::move(batch));
  if (batches_.size() > kMaxBatches) {
    batches_.pop_front();
  }
  const Batch& done = batches_.back();
  observers_.Notify(&Observer::OnBatchDone, done);
  return &done;
}

void TabOrganizer::UndoMoves(const Batch& batch) {
  const std::vector<BrowserWindowInterface*> windows = Windows();
  for (const Batch::Placement& placement : batch.placements) {
    auto window = std::ranges::find_if(windows, [&](BrowserWindowInterface* b) {
      return b->GetSessionID() == placement.window;
    });
    if (window == windows.end()) {
      continue;
    }
    BrowserWindowInterface* browser = *window;
    TabStripModel* model = browser->GetTabStripModel();
    // 要放回去的：还在这个窗口里、还在放进去的那个文件夹里（用户之后挤过的不动）。
    std::vector<tabs::TabHandle> moved;
    for (const Batch::Move& move : batch.moves) {
      tabs::TabInterface* tab = move.tab.Get();
      if (move.window == placement.window && tab &&
          tab->GetBrowserWindowInterface() == browser && tab->IsPinned() &&
          !pinned_tabs::IsFavorite(tab) &&
          pinned_tabs::Folder(tab) == move.folder) {
        moved.push_back(move.tab);
      }
    }
    if (moved.empty()) {
      continue;
    }
    const auto was_moved = [&](const tabs::TabHandle& handle) {
      return std::ranges::find(moved, handle) != moved.end();
    };
    TabsView* view = pin_folders::VisibleTabs(browser);
    if (view) {
      view->BeginBatch();
    }
    // 从后往前放：它后面原来的那些已经回到原处，放在第一个还在列表里的前面。
    for (size_t i = placement.tabs.size(); i-- > 0;) {
      if (!was_moved(placement.tabs[i])) {
        continue;
      }
      tabs::TabInterface* tab = placement.tabs[i].Get();
      int index = model->GetIndexOfTab(tab);
      if (index == TabStripModel::kNoTab) {
        continue;
      }
      model->SetTabPinned(index, false);
      index = model->GetIndexOfTab(tab);
      int to = model->count();
      for (size_t j = i + 1; j < placement.tabs.size(); ++j) {
        tabs::TabInterface* next = placement.tabs[j].Get();
        if (next && next->GetBrowserWindowInterface() == browser &&
            !next->IsPinned()) {
          to = model->GetIndexOfTab(next);
          break;
        }
      }
      tab_tree::MoveBranch(model, index, to);
    }
    // 父子：放回去的挂回原来的父标签下面，原来挂在它们下面、固定时上移了的挂回来（从
    // 前往后，父标签先挂好）。
    for (size_t i = 0; i < placement.tabs.size(); ++i) {
      const int parent = placement.parents[i];
      if (parent < 0 || static_cast<size_t>(parent) >= placement.tabs.size() ||
          (!was_moved(placement.tabs[i]) &&
           !was_moved(placement.tabs[parent]))) {
        continue;
      }
      tabs::TabInterface* tab = placement.tabs[i].Get();
      tabs::TabInterface* up = placement.tabs[parent].Get();
      if (!tab || !up || tab->GetBrowserWindowInterface() != browser ||
          up->GetBrowserWindowInterface() != browser || tab->IsPinned() ||
          up->IsPinned()) {
        continue;
      }
      const int index = model->GetIndexOfTab(tab);
      const int parent_index = model->GetIndexOfTab(up);
      if (parent_index >= index) {
        continue;
      }
      const std::vector<tab_tree::Node> nodes = tab_tree::Compute(model);
      if (nodes[index].parent != parent_index) {
        tab_tree::MoveBranch(model, index, index, parent_index);
      }
    }
    if (view) {
      view->EndBatch(TabListView::Change::kFold);
    }
  }
  // 新建的文件夹空了（哪个窗口里都没有标签、没有子文件夹）就拿掉。
  const std::vector<pin_folders::Folder> folders = pin_folders::Read(profile_);
  for (const std::string& id : batch.created_folders) {
    bool used = std::ranges::any_of(
        folders, [&](const pin_folders::Folder& f) { return f.parent == id; });
    for (BrowserWindowInterface* window : windows) {
      used = used || !pin_folders::TabsIn(window, id).empty();
    }
    if (!used) {
      pin_folders::Remove(profile_, id);
    }
  }
}

bool TabOrganizer::Undo(const std::string& batch_id) {
  auto it = std::ranges::find(batches_, batch_id, &Batch::id);
  if (it == batches_.end() || it->undone) {
    return false;
  }
  if (!it->moves.empty()) {
    UndoMoves(*it);
  }
  TabArchive* archive = TabArchive::ForProfile(profile_);
  std::vector<Batch::Item> items = it->items;
  // 从前往后放回去：先放的在前面，父标签先于子标签。
  std::ranges::sort(items, [](const Batch::Item& a, const Batch::Item& b) {
    return std::make_pair(a.window.id(), a.index) <
           std::make_pair(b.window.id(), b.index);
  });
  const std::vector<BrowserWindowInterface*> windows = Windows();
  BrowserWindowInterface* fallback = nullptr;
  if (BrowserWindowInterface* last =
          GetLastActiveBrowserWindowInterfaceWithAnyProfile();
      last && std::ranges::find(windows, last) != windows.end()) {
    fallback = last;
  } else if (!windows.empty()) {
    fallback = windows.front();
  }
  for (const Batch::Item& item : items) {
    auto window = std::ranges::find_if(windows, [&](BrowserWindowInterface* b) {
      return b->GetSessionID() == item.window;
    });
    BrowserWindowInterface* browser =
        window != windows.end() ? *window : fallback;
    if (!browser) {
      continue;
    }
    std::optional<ArchivedTab> entry = archive->Take(item.archive_id);
    if (!entry) {
      continue;
    }
    TabArchive::Open(browser, *entry, item.index, /*select=*/false);
  }
  it->undone = true;
  observers_.Notify(&Observer::OnBatchUndone, *it);
  return true;
}

const Batch* TabOrganizer::LastBatch() const {
  return batches_.empty() ? nullptr : &batches_.back();
}

const Batch* TabOrganizer::FindBatch(const std::string& id) const {
  auto it = std::ranges::find(batches_, id, &Batch::id);
  return it == batches_.end() ? nullptr : &*it;
}

content::WebContents* TabOrganizer::Restore(BrowserWindowInterface* browser,
                                            const std::string& archive_id,
                                            bool select) {
  std::optional<ArchivedTab> entry =
      TabArchive::ForProfile(profile_)->Take(archive_id);
  if (!entry) {
    return nullptr;
  }
  return TabArchive::Open(browser, *entry, -1, select);
}

void TabOrganizer::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void TabOrganizer::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

// static
void TabOrganizer::SetLastSeenForTesting(tabs::TabInterface* tab,
                                         base::Time time) {
  content::WebContents* contents = tab->GetContents();
  LastSeen::CreateForWebContents(contents);
  LastSeen* seen = LastSeen::FromWebContents(contents);
  seen->time = time;
  seen->forced = true;
}

std::string TabOrganizer::DescribeForTesting(
    BrowserWindowInterface* browser) const {
  const Settings settings = ReadSettings(profile_);
  std::string sites;
  for (const std::string& site : settings.exceptions) {
    base::StrAppend(&sites, {sites.empty() ? "" : ",", site});
  }
  std::string text = base::StrCat(
      {"organizer auto=", settings.auto_archive ? "1" : "0",
       " idle_h=", base::NumberToString(settings.idle.InHours()),
       " dup=", settings.duplicates ? "1" : "0",
       " retention_d=", base::NumberToString(settings.retention.InDays()),
       " exceptions=[", sites, "] quiet=", Quiet(browser) ? "1" : "0",
       " system_idle_s=", base::NumberToString(ui::CalculateIdleTime()), "\n"});
  std::map<tabs::TabInterface*, ArchiveReason> candidates;
  for (const auto& [tab, reason] : FindCandidates(browser, settings)) {
    candidates[tab] = reason;
  }
  TabStripModel* model = browser->GetTabStripModel();
  for (int i = 0; i < model->count(); ++i) {
    tabs::TabInterface* tab = model->GetTabAtIndex(i);
    const char* skip = SkipReason(tab, settings);
    auto candidate = candidates.find(tab);
    base::StrAppend(
        &text,
        {"  tab ", base::NumberToString(i), " \"",
         base::UTF16ToUTF8(tab->GetContents()->GetTitle()), "\" idle_m=",
         base::NumberToString(IdleFor(tab).InMinutes()),
         skip ? base::StrCat({" skip=", skip}) : std::string(),
         candidate != candidates.end()
             ? base::StrCat({" archive=", ReasonName(candidate->second)})
             : std::string(),
         "\n"});
  }
  for (const Batch& batch : batches_) {
    base::StrAppend(
        &text, {"  batch ",  batch.id,
                " source=",  SourceName(batch.source),
                " closed=",  base::NumberToString(batch.closed),
                " idle=",    base::NumberToString(batch.idle),
                " dup=",     base::NumberToString(batch.duplicates),
                " items=",   base::NumberToString(batch.items.size()),
                " moved=",   base::NumberToString(batch.moves.size()),
                " folders=", base::NumberToString(batch.folders.size()),
                " created=", base::NumberToString(batch.created_folders.size()),
                " undone=",  batch.undone ? "1" : "0",
                "\n"});
  }
  return text;
}

}  // namespace starry::tab_organizer
