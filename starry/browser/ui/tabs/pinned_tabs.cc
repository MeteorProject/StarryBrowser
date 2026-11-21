// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/pinned_tabs.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/numerics/clamped_math.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/startup/startup_tab.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/sessions/content/content_serialized_navigation_builder.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/restore_type.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/split/split_tabs.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "starry/browser/ui/tabs/pinned_peek.h"
#include "starry/browser/ui/tabs/tab_tree.h"
#include "starry/common/internal_urls.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace starry {

namespace {

// 会话里每个标签的 extra_data 的键。
constexpr char kUrlKey[] = "starry.pinned_url";
constexpr char kNameKey[] = "starry.pin_name";
constexpr char kFavoriteKey[] = "starry.favorite_id";
constexpr char kSectionKey[] = "starry.pin_section";
constexpr char kFolderKey[] = "starry.pin_folder";
constexpr char kOrderKey[] = "starry.pin_order";
constexpr char kTitleKey[] = "starry.pinned_title";

}  // namespace

// 固定时的网址和标题，挂在标签的 WebContents 上（标签拖到别的窗口时跟着走），换
// WebContents 时带到新的上面。
class PinnedTabPage : public content::WebContentsUserData<PinnedTabPage>,
                      public content::WebContentsObserver {
 public:
  PinnedTabPage(const PinnedTabPage&) = delete;
  PinnedTabPage& operator=(const PinnedTabPage&) = delete;
  ~PinnedTabPage() override = default;

  static PinnedTabPage* Set(content::WebContents* contents,
                            const GURL& url,
                            const std::u16string& title) {
    CreateForWebContents(contents);
    PinnedTabPage* page = FromWebContents(contents);
    page->url_ = url;
    page->title_ = title;
    return page;
  }

  const GURL& url() const { return url_; }
  const std::u16string& title() const { return title_; }

  bool favorite = false;
  std::string folder;
  // 在所在的那一层里排第几；没排过的是空的。
  std::optional<double> order;
  std::u16string name;
  int favorite_order = 0;
  std::string favorite_id;
  // 在哪个图块文件夹里（常用标签的目录里的 "folder"）；在图块那一层是空的。
  std::string favorite_folder;
  bool closing = false;
  bool archiving = false;

  void AboutToBeDiscarded(content::WebContents* replacement) override {
    CopyTo(replacement);
  }

  void CopyTo(content::WebContents* replacement) const {
    if (!replacement) {
      return;
    }
    auto* copy = Set(replacement, url_, title_);
    copy->favorite = favorite;
    copy->name = name;
    copy->favorite_order = favorite_order;
    copy->favorite_id = favorite_id;
    copy->favorite_folder = favorite_folder;
    copy->folder = folder;
    copy->order = order;
  }

  void DidOpenRequestedURL(content::WebContents* target,
                           content::RenderFrameHost* frame,
                           const GURL& url,
                           const content::Referrer&,
                           WindowOpenDisposition disposition,
                           ui::PageTransition transition,
                           bool from_menu,
                           bool renderer_initiated) override {
    if (renderer_initiated && !from_menu &&
        disposition == WindowOpenDisposition::kNewForegroundTab &&
        ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_LINK) &&
        frame == web_contents()->GetPrimaryMainFrame()) {
      PinnedPeek::LinkOpened(web_contents(), target, url);
    }
  }

  void WebContentsDestroyed() override {
    if (archiving && favorite && !favorite_id.empty()) {
      ScopedDictPrefUpdate update(
          Profile::FromBrowserContext(web_contents()->GetBrowserContext())
              ->GetPrefs(),
          prefs::kFavorites);
      update->Remove(favorite_id);
    }
  }

  void BeforeUnloadFired(bool proceed) override {
    if (!proceed) {
      closing = false;
      archiving = false;
    }
  }

 private:
  friend class content::WebContentsUserData<PinnedTabPage>;

  explicit PinnedTabPage(content::WebContents* contents)
      : content::WebContentsUserData<PinnedTabPage>(*contents),
        content::WebContentsObserver(contents) {}

  GURL url_;
  std::u16string title_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PinnedTabPage);

namespace {

// 能当书签回去的网址：空白标签不算。
bool IsUsable(const GURL& url) {
  return url.is_valid() && !IsBlankTabUrl(url);
}

// 现在这一页：已提交的导航（休眠、恢复了还没载入的标签是会话里选中的那一条），还没有
// 时用地址栏的网址（正在打开的）。用显示的网址（starry://…），同书签。
std::pair<GURL, std::u16string> CurrentPage(content::WebContents* contents) {
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  if (entry && !entry->IsInitialEntry()) {
    return {entry->GetVirtualURL(), entry->GetTitle()};
  }
  return {contents->GetVisibleURL(), contents->GetTitle()};
}

// 记下现在这一页。空白的不记，等打开了网页再补。
PinnedTabPage* Remember(content::WebContents* contents) {
  auto [url, title] = CurrentPage(contents);
  if (!IsUsable(url)) {
    return nullptr;
  }
  return PinnedTabPage::Set(contents, url, title);
}

// 写进会话（取消固定时写空的，恢复时不认）。无痕窗口没有会话，不写。
void SaveInSession(content::WebContents* contents, const PinnedTabPage* page) {
  SessionService* service = SessionServiceFactory::GetForProfileIfExisting(
      Profile::FromBrowserContext(contents->GetBrowserContext()));
  if (!service) {
    return;
  }
  const SessionID window_id =
      sessions::SessionTabHelper::IdForWindowContainingTab(contents);
  const SessionID tab_id = sessions::SessionTabHelper::IdForTab(contents);
  if (!window_id.is_valid() || !tab_id.is_valid()) {
    return;
  }
  service->AddTabExtraData(window_id, tab_id, kNameKey,
                           page ? base::UTF16ToUTF8(page->name) : "");
  service->AddTabExtraData(window_id, tab_id, kFavoriteKey,
                           page ? page->favorite_id : "");
  service->AddTabExtraData(window_id, tab_id, kSectionKey,
                           page && !page->favorite ? "pinned" : "");
  service->AddTabExtraData(window_id, tab_id, kFolderKey,
                           page ? page->folder : "");
  service->AddTabExtraData(
      window_id, tab_id, kOrderKey,
      page && page->order ? base::NumberToString(*page->order) : "");
  service->AddTabExtraData(window_id, tab_id, kUrlKey,
                           page ? page->url().spec() : std::string());
  service->AddTabExtraData(
      window_id, tab_id, kTitleKey,
      page ? base::UTF16ToUTF8(page->title()) : std::string());
}

// 当前标签换成最近用过的另一个醒着的标签，没有就开一个新标签页。休眠的不叫醒：
// 一路 ⌘W 下去时，刚关掉的固定标签不会又被打开。
void ActivateAnother(BrowserWindowInterface* browser, tabs::TabInterface* tab) {
  TabStripModel* model = browser->GetTabStripModel();
  int next = TabStripModel::kNoTab;
  base::TimeTicks next_time;
  for (int i = 0; i < model->count(); ++i) {
    tabs::TabInterface* other = model->GetTabAtIndex(i);
    content::WebContents* contents = other->GetContents();
    if (other == tab || contents->WasDiscarded()) {
      continue;
    }
    const base::TimeTicks time = contents->GetLastActiveTimeTicks();
    if (next == TabStripModel::kNoTab || time > next_time) {
      next = i;
      next_time = time;
    }
  }
  if (next != TabStripModel::kNoTab) {
    model->ActivateTabAt(next);
  } else {
    chrome::AddTabAt(browser, GURL(), -1, /*foreground=*/true);
  }
}

std::unique_ptr<content::WebContents> CreateSleepingContents(
    Profile* profile,
    const GURL& url,
    const std::u16string& title,
    content::WebContents* old_contents) {
  // 同 Chrome 恢复会话里的一个标签（ContentSerializedNavigationBuilder 补上没有的
  // 网页状态）。
  sessions::SerializedNavigationEntry navigation;
  navigation.set_index(0);
  navigation.set_virtual_url(url);
  navigation.set_title(title);
  navigation.set_transition_type(ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  navigation.set_timestamp(base::Time::Now());
  std::vector<std::unique_ptr<content::NavigationEntry>> entries =
      sessions::ContentSerializedNavigationBuilder::ToNavigationEntries(
          base::span_from_ref(navigation), profile);

  content::WebContents::CreateParams params(profile);
  params.initially_hidden = true;
  params.desired_renderer_state =
      content::WebContents::CreateParams::kNoRendererProcess;
  if (old_contents) {
    params.last_active_time = old_contents->GetLastActiveTime();
    params.last_active_time_ticks = old_contents->GetLastActiveTimeTicks();
  }
  std::unique_ptr<content::WebContents> fresh =
      content::WebContents::Create(params);
  fresh->GetController().Restore(0, content::RestoreType::kRestored, &entries);
  // 同 Chrome 以前的休眠（TabLifecycleUnit::FinishDiscard）。会话、标签休眠的状态
  // 跟着 TabStripModel 的 kReplaced 换过去。浏览器自己的页面不算休眠（同恢复的标签，
  // restored_tabs.cc）。
  if (InternalPageForUrl(url) == InternalPage::kNone && !IsInternalURL(url) &&
      !url.SchemeIs("devtools")) {
    fresh->SetWasDiscarded(true);
  }
  return fresh;
}

int NextOrder(Profile* profile) {
  int next = 0;
  for (const char* path : {prefs::kFavorites, prefs::kFavoriteFolders}) {
    for (auto [id, value] : profile->GetPrefs()->GetDict(path)) {
      if (const auto* entry = value.GetIfDict()) {
        next = std::max(
            next,
            base::ClampAdd(entry->FindInt("order").value_or(0), 1).RawValue());
      }
    }
  }
  return next;
}

void PublishFavorite(Profile* profile, PinnedTabPage& page) {
  if (page.favorite_id.empty()) {
    page.favorite_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }
  const auto* existing = profile->GetPrefs()
                             ->GetDict(prefs::kFavorites)
                             .FindDict(page.favorite_id);
  page.favorite_order =
      existing ? existing->FindInt("order").value_or(0) : NextOrder(profile);
  base::DictValue entry;
  entry.Set("url", page.url().spec());
  entry.Set("title", page.title());
  entry.Set("name", page.name);
  entry.Set("order", page.favorite_order);
  if (!page.favorite_folder.empty()) {
    entry.Set("folder", page.favorite_folder);
  }
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kFavorites);
  update->Set(page.favorite_id, std::move(entry));
}

void ForgetFavorite(Profile* profile, PinnedTabPage& page) {
  if (page.favorite_id.empty()) {
    return;
  }
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kFavorites);
  update->Remove(page.favorite_id);
  page.favorite_id.clear();
}

// 替换后台标签的 WebContents。新标签只保留固定网址，激活时才载入。
void ResetToPinnedPage(TabStripModel* model, tabs::TabInterface* tab) {
  content::WebContents* old_contents = tab->GetContents();
  Profile* profile =
      Profile::FromBrowserContext(old_contents->GetBrowserContext());
  // 拷贝出来：旧的 WebContents 连同挂在上面的这一份一会儿就没了。
  const PinnedTabPage& page = *PinnedTabPage::FromWebContents(old_contents);
  const GURL url = page.url();
  const std::u16string pinned_title = page.title();

  // 标题用这个网址最近一次的（网页的标题会变，比如未读数），没打开过就用固定时的。
  std::u16string title = pinned_title;
  content::NavigationController& old_controller = old_contents->GetController();
  for (int i = old_controller.GetEntryCount() - 1; i >= 0; --i) {
    content::NavigationEntry* entry = old_controller.GetEntryAtIndex(i);
    if (entry->GetVirtualURL() == url && !entry->GetTitle().empty()) {
      title = entry->GetTitle();
      break;
    }
  }

  auto fresh = CreateSleepingContents(profile, url, title, old_contents);
  old_contents->AboutToBeDiscarded(fresh.get());
  content::WebContents* contents = fresh.get();
  model->DiscardWebContents(old_contents, std::move(fresh)).reset();

  // 同恢复的休眠标签：先从数据库里取回网站图标，图块上认得出是哪个网页。
  if (favicon::ContentFaviconDriver* favicon =
          favicon::ContentFaviconDriver::FromWebContents(contents)) {
    favicon->FetchFavicon(url, /*is_same_document=*/false);
  }
  model->NotifyTabChanged(tab, TabChangeType::kAll);
}

}  // namespace

namespace pinned_tabs {

void PinnedStateChanged(tabs::TabInterface* tab) {
  content::WebContents* contents = tab->GetContents();
  if (tab->IsPinned()) {
    if (!PinnedTabPage::FromWebContents(contents)) {
      if (auto* page = Remember(contents)) {
        page->favorite = false;
        SaveInSession(contents, page);
      }
    }
  } else if (auto* page = PinnedTabPage::FromWebContents(contents)) {
    ForgetFavorite(Profile::FromBrowserContext(contents->GetBrowserContext()),
                   *page);
    contents->RemoveUserData(PinnedTabPage::UserDataKey());
    SaveInSession(contents, nullptr);
  }
}

GURL PinnedUrl(tabs::TabInterface* tab) {
  if (!tab->IsPinned()) {
    return GURL();
  }
  content::WebContents* contents = tab->GetContents();
  const PinnedTabPage* page = PinnedTabPage::FromWebContents(contents);
  if (!page && (page = Remember(contents))) {
    SaveInSession(contents, page);
  }
  return page ? page->url() : GURL();
}

bool IsFavorite(tabs::TabInterface* tab) {
  const auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
  return tab->IsPinned() && page && page->favorite;
}

std::string Folder(tabs::TabInterface* tab) {
  const auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
  return tab->IsPinned() && page ? page->folder : std::string();
}

std::string FavoriteFolder(tabs::TabInterface* tab) {
  const auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
  return tab->IsPinned() && page && page->favorite ? page->favorite_folder
                                                   : std::string();
}

void InitializeProfile(Profile* profile) {
  // Off-the-record PrefService normally reads through to its parent. Give
  // private windows their own in-memory collections from the first window.
  static constexpr char kPrivateCollectionsKey = 0;
  if (profile->IsOffTheRecord() &&
      !profile->GetUserData(&kPrivateCollectionsKey)) {
    profile->SetUserData(&kPrivateCollectionsKey,
                         std::make_unique<base::SupportsUserData::Data>());
    profile->GetPrefs()->SetDict(prefs::kFavorites, base::DictValue());
    profile->GetPrefs()->SetDict(prefs::kPinFolders, base::DictValue());
    profile->GetPrefs()->SetDict(prefs::kFavoriteFolders, base::DictValue());
  }
}

void TabClosed(content::WebContents* contents) {
  if (auto* page = PinnedTabPage::FromWebContents(contents);
      page && page->favorite) {
    ForgetFavorite(Profile::FromBrowserContext(contents->GetBrowserContext()),
                   *page);
  }
}

void CopyStateIfMissing(content::WebContents* old_contents,
                        content::WebContents* replacement) {
  auto* page = PinnedTabPage::FromWebContents(old_contents);
  if (page && !PinnedTabPage::FromWebContents(replacement)) {
    page->CopyTo(replacement);
    SaveInSession(replacement, PinnedTabPage::FromWebContents(replacement));
  }
}

int FavoriteOrder(tabs::TabInterface* tab) {
  auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
  return page ? page->favorite_order : 0;
}

bool HasRoomForFavorite(BrowserWindowInterface* browser,
                        const std::string& folder) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  const auto& folders = settings->GetDict(prefs::kFavoriteFolders);
  // 图块那一层：常用标签和图块文件夹各占一格。
  size_t count = folder.empty() ? folders.size() : 0;
  for (auto [id, value] : settings->GetDict(prefs::kFavorites)) {
    const auto* entry = value.GetIfDict();
    if (!entry) {
      continue;
    }
    const auto* in = entry->FindString("folder");
    const bool inside = in && !in->empty() && folders.FindDict(*in);
    if (inside ? *in == folder : folder.empty()) {
      ++count;
    }
  }
  return count < kMaxFavoriteTiles;
}

int NextFavoriteOrder(Profile* profile) {
  return NextOrder(profile);
}

void SetFavoriteOrders(
    BrowserWindowInterface* browser,
    const std::vector<std::pair<tabs::TabHandle, int>>& orders) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  const auto& catalog = settings->GetDict(prefs::kFavorites);
  std::vector<std::pair<std::string, int>> changed;
  for (const auto& [handle, order] : orders) {
    tabs::TabInterface* tab = handle.Get();
    auto* page =
        tab ? PinnedTabPage::FromWebContents(tab->GetContents()) : nullptr;
    if (!page || !page->favorite || page->favorite_id.empty()) {
      continue;
    }
    // 这个窗口里马上按新的顺序排，别的窗口等设置变了再同步。
    page->favorite_order = order;
    const auto* entry = catalog.FindDict(page->favorite_id);
    if (entry && entry->FindInt("order") != order) {
      changed.emplace_back(page->favorite_id, order);
    }
  }
  // 没有要改的就不碰设置：每改一次，所有窗口都要重排一遍。
  if (changed.empty()) {
    return;
  }
  ScopedDictPrefUpdate update(settings, prefs::kFavorites);
  for (const auto& [id, order] : changed) {
    if (auto* entry = update->FindDict(id)) {
      entry->Set("order", order);
    }
  }
}

namespace {

// 固定（空白标签固定不了），记下现在的网址。
PinnedTabPage* EnsurePinned(tabs::TabInterface* tab) {
  auto* model = tab->GetBrowserWindowInterface()->GetTabStripModel();
  if (!tab->IsPinned()) {
    if (!IsUsable(CurrentPage(tab->GetContents()).first)) {
      return nullptr;
    }
    model->SetTabPinned(model->GetIndexOfTab(tab), true);
  }
  PinnedUrl(tab);
  return PinnedTabPage::FromWebContents(tab->GetContents());
}

}  // namespace

void SetLocation(tabs::TabInterface* tab, bool favorite, std::string folder) {
  if (favorite) {
    SetFavoriteFolder(tab, FavoriteFolder(tab));
    return;
  }
  auto* browser = tab->GetBrowserWindowInterface();
  PinnedTabPage* page = EnsurePinned(tab);
  if (!page) {
    return;
  }
  if (page->favorite) {
    ForgetFavorite(browser->GetProfile(), *page);
  }
  if (page->favorite || page->folder != folder) {
    page->order.reset();
  }
  page->favorite = false;
  page->favorite_folder.clear();
  page->folder = std::move(folder);
  SaveInSession(tab->GetContents(), page);
  browser->GetTabStripModel()->NotifyTabChanged(tab, TabChangeType::kAll);
}

bool SetFavoriteFolder(tabs::TabInterface* tab, const std::string& folder) {
  auto* browser = tab->GetBrowserWindowInterface();
  if (!browser) {
    return false;
  }
  Profile* profile = browser->GetProfile();
  if (!folder.empty() &&
      !profile->GetPrefs()->GetDict(prefs::kFavoriteFolders).FindDict(folder)) {
    return false;
  }
  if (IsFavorite(tab) && FavoriteFolder(tab) == folder) {
    return true;
  }
  if (!HasRoomForFavorite(browser, folder)) {
    return false;
  }
  PinnedTabPage* page = EnsurePinned(tab);
  if (!page) {
    return false;
  }
  const bool listed = page->favorite && !page->favorite_id.empty() &&
                      profile->GetPrefs()
                          ->GetDict(prefs::kFavorites)
                          .FindDict(page->favorite_id);
  page->favorite = true;
  page->folder.clear();
  page->order.reset();
  page->favorite_folder = folder;
  if (!listed) {
    PublishFavorite(profile, *page);
  } else {
    // 换了一层：排到那一层的最后。
    page->favorite_order = NextOrder(profile);
    ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kFavorites);
    base::DictValue* entry = update->FindDict(page->favorite_id);
    entry->Set("order", page->favorite_order);
    if (folder.empty()) {
      entry->Remove("folder");
    } else {
      entry->Set("folder", folder);
    }
  }
  SaveInSession(tab->GetContents(), page);
  browser->GetTabStripModel()->NotifyTabChanged(tab, TabChangeType::kAll);
  return true;
}

void ClearFavoriteFolder(BrowserWindowInterface* browser,
                         const std::string& folder) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  std::vector<std::string> inside;
  for (auto [id, value] : settings->GetDict(prefs::kFavorites)) {
    const auto* entry = value.GetIfDict();
    const auto* in = entry ? entry->FindString("folder") : nullptr;
    if (in && *in == folder) {
      inside.push_back(id);
    }
  }
  if (!inside.empty()) {
    ScopedDictPrefUpdate update(settings, prefs::kFavorites);
    for (const std::string& id : inside) {
      if (auto* entry = update->FindDict(id)) {
        entry->Remove("folder");
      }
    }
  }
  // 这个窗口里马上换过去，别的窗口等设置变了再同步。
  TabStripModel* model = browser->GetTabStripModel();
  for (int i = 0; i < model->IndexOfFirstNonPinnedTab(); ++i) {
    tabs::TabInterface* tab = model->GetTabAtIndex(i);
    auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
    if (page && page->favorite && page->favorite_folder == folder) {
      page->favorite_folder.clear();
      model->NotifyTabChanged(tab, TabChangeType::kAll);
    }
  }
}

std::optional<double> Order(tabs::TabInterface* tab) {
  const auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
  return tab->IsPinned() && page && !page->favorite ? page->order
                                                    : std::nullopt;
}

void SetOrder(tabs::TabInterface* tab, std::optional<double> order) {
  auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
  if (!tab->IsPinned() || !page || page->favorite || page->order == order) {
    return;
  }
  page->order = order;
  SaveInSession(tab->GetContents(), page);
  tab->GetBrowserWindowInterface()->GetTabStripModel()->NotifyTabChanged(
      tab, TabChangeType::kAll);
}

void SyncFavorites(BrowserWindowInterface* browser) {
  auto* model = browser->GetTabStripModel();
  Profile* profile = browser->GetProfile();
  if (browser->IsDeleteScheduled() || model->empty() || model->closing_all() ||
      SessionRestore::IsRestoring(profile)) {
    return;
  }
  const base::DictValue favorites =
      profile->GetPrefs()->GetDict(prefs::kFavorites).Clone();
  // 常用标签在哪个图块文件夹里：文件夹没了的算在图块那一层。
  std::set<std::string> folder_ids;
  for (auto [id, value] :
       profile->GetPrefs()->GetDict(prefs::kFavoriteFolders)) {
    if (value.is_dict()) {
      folder_ids.insert(id);
    }
  }
  auto folder_of = [&folder_ids](const base::DictValue& entry) {
    const auto* folder = entry.FindString("folder");
    return folder && folder_ids.contains(*folder) ? *folder : std::string();
  };
  std::set<std::string> present;
  std::vector<tabs::TabHandle> local;
  for (int i = 0; i < model->IndexOfFirstNonPinnedTab(); ++i) {
    local.push_back(model->GetTabAtIndex(i)->GetHandle());
  }
  for (const auto& handle : local) {
    auto* tab = handle.Get();
    if (!tab || !IsFavorite(tab) || !PinnedUrl(tab).is_valid()) {
      continue;
    }
    auto* contents = tab->GetContents();
    auto* page = PinnedTabPage::FromWebContents(contents);
    if (page->favorite_id.empty()) {
      // Migrate an existing tile. Windows that saved the same favorite can
      // share an entry while duplicate tiles in one window retain their IDs.
      for (auto [id, value] : favorites) {
        const auto* entry = value.GetIfDict();
        const auto* url = entry ? entry->FindString("url") : nullptr;
        if (url && *url == page->url().spec() && !present.contains(id)) {
          page->favorite_id = id;
          break;
        }
      }
      if (page->favorite_id.empty()) {
        PublishFavorite(profile, *page);
      }
      SaveInSession(contents, page);
    }
    const auto* entry = profile->GetPrefs()
                            ->GetDict(prefs::kFavorites)
                            .FindDict(page->favorite_id);
    const auto* url = entry ? entry->FindString("url") : nullptr;
    if (!url || present.contains(page->favorite_id)) {
      // Removal in another window must preserve a live page with unsaved work.
      page->favorite_id.clear();
      if (contents->WasDiscarded() && !tab->IsActivated()) {
        model->CloseWebContentsAt(model->GetIndexOfTab(tab),
                                  TabCloseTypes::CLOSE_NONE);
      } else {
        model->SetTabPinned(model->GetIndexOfTab(tab), false);
      }
      continue;
    }
    present.insert(page->favorite_id);
    page->favorite_order = entry->FindInt("order").value_or(0);
    const std::string folder = folder_of(*entry);
    const bool moved = page->favorite_folder != folder;
    page->favorite_folder = folder;
    const auto* title = entry->FindString("title");
    const auto saved_title =
        title ? base::UTF8ToUTF16(*title) : std::u16string();
    const auto* name = entry->FindString("name");
    const std::u16string saved_name =
        name ? base::UTF8ToUTF16(*name) : std::u16string();
    if (GURL(*url) != page->url() || saved_title != page->title() ||
        saved_name != page->name) {
      page->name = saved_name;
      PinnedTabPage::Set(contents, GURL(*url), saved_title);
      SaveInSession(contents, page);
      if (contents->WasDiscarded() && !tab->IsActivated()) {
        ResetToPinnedPage(model, tab);
      } else {
        model->NotifyTabChanged(tab, TabChangeType::kAll);
      }
    } else if (moved) {
      model->NotifyTabChanged(tab, TabChangeType::kAll);
    }
  }
  const auto& catalog = profile->GetPrefs()->GetDict(prefs::kFavorites);
  for (auto [id, value] : catalog) {
    if (present.contains(id)) {
      continue;
    }
    const auto* entry = value.GetIfDict();
    const auto* url = entry ? entry->FindString("url") : nullptr;
    if (!url || !IsUsable(GURL(*url))) {
      continue;
    }
    const auto* title = entry->FindString("title");
    auto text = title ? base::UTF8ToUTF16(*title) : std::u16string();
    auto contents = CreateSleepingContents(profile, GURL(*url), text, nullptr);
    auto* page = PinnedTabPage::Set(contents.get(), GURL(*url), text);
    if (const auto* name = entry->FindString("name")) {
      page->name = base::UTF8ToUTF16(*name);
    }
    page->favorite = true;
    page->favorite_id = id;
    page->favorite_order = entry->FindInt("order").value_or(0);
    page->favorite_folder = folder_of(*entry);
    model->InsertWebContentsAt(model->IndexOfFirstNonPinnedTab(),
                               std::move(contents), AddTabTypes::ADD_PINNED);
  }
}

void Archive(tabs::TabInterface* tab, bool to_tab_archive) {
  auto* model = tab->GetBrowserWindowInterface()->GetTabStripModel();
  // 用户明说要归档的也放进标签管理的归档，能搜、能恢复；
  // 「最近关闭」里照样有。
  Profile* profile = tab->GetBrowserWindowInterface()->GetProfile();
  if (to_tab_archive && !profile->IsOffTheRecord()) {
    std::vector<tab_organizer::ArchivedTab> archived;
    archived.push_back(tab_organizer::TabArchive::Capture(
        tab->GetContents(), tab_organizer::ArchiveReason::kManual));
    tab_organizer::TabArchive::ForProfile(profile)->Add(std::move(archived));
  }
  if (auto* page = PinnedTabPage::FromWebContents(tab->GetContents())) {
    page->closing = false;
    page->archiving = true;
  }
  model->CloseWebContentsAt(model->GetIndexOfTab(tab),
                            TabCloseTypes::CLOSE_USER_GESTURE |
                                TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
}

std::u16string DisplayName(tabs::TabInterface* tab) {
  auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
  if (!tab->IsPinned() || !page) {
    return {};
  }
  return page->name.empty() ? page->title() : page->name;
}

void Rename(tabs::TabInterface* tab) {
  auto* view = StarryBrowserView::From(tab->GetBrowserWindowInterface());
  if (!view || !tab->IsPinned()) {
    return;
  }
  DialogSpec spec;
  spec.title = L("tabs.renamePin");
  spec.field = DisplayName(tab);
  spec.buttons = {{L("common.cancel"), ButtonVariant::kTertiary, true},
                  {L("common.save"), ButtonVariant::kFilled}};
  view->PresentDialog(
      std::move(spec),
      base::BindOnce(
          [](tabs::TabHandle handle, std::optional<size_t> button,
             const std::u16string& text) {
            auto* tab = handle.Get();
            if (!tab || button != 1 || !tab->IsPinned() ||
                !tab->GetBrowserWindowInterface()) {
              return;
            }
            auto* page = PinnedTabPage::FromWebContents(tab->GetContents());
            if (!page) {
              return;
            }
            base::TrimWhitespace(text, base::TRIM_ALL, &page->name);
            auto* browser = tab->GetBrowserWindowInterface();
            if (page->favorite) {
              PublishFavorite(browser->GetProfile(), *page);
            }
            SaveInSession(tab->GetContents(), page);
            browser->GetTabStripModel()->NotifyTabChanged(tab,
                                                          TabChangeType::kAll);
          },
          tab->GetHandle()));
}

void SetPinnedURL(tabs::TabInterface* tab, const GURL& url) {
  if (!tab->IsPinned() || !IsUsable(url)) {
    return;
  }
  auto [current, title] = CurrentPage(tab->GetContents());
  auto* page = PinnedTabPage::Set(tab->GetContents(), url,
                                  url == current ? title : std::u16string());
  if (page->favorite) {
    PublishFavorite(tab->GetBrowserWindowInterface()->GetProfile(), *page);
  }
  SaveInSession(tab->GetContents(), page);
  auto* model = tab->GetBrowserWindowInterface()->GetTabStripModel();
  if (tab->GetContents()->WasDiscarded() && !tab->IsActivated()) {
    ResetToPinnedPage(model, tab);
  } else {
    model->NotifyTabChanged(tab, TabChangeType::kAll);
  }
}

void Reset(tabs::TabInterface* tab, bool keep_current) {
  const GURL url = PinnedUrl(tab);
  auto* browser = tab->GetBrowserWindowInterface();
  if (!browser || !IsUsable(url)) {
    return;
  }
  auto* contents = tab->GetContents();
  if (keep_current && contents->GetLastCommittedURL() != url) {
    chrome::AddTabAt(browser, contents->GetLastCommittedURL(), -1, false);
  }
  browser->GetTabStripModel()->ActivateTabAt(
      browser->GetTabStripModel()->GetIndexOfTab(tab));
  contents->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_BOOKMARK, {});
}

void FinishClose(BrowserWindowInterface* browser, tabs::TabInterface* tab) {
  // 分屏里的一格：先移出分屏，剩下的照常显示（当前标签交给剩下的一格）。
  if (tab->IsSplit()) {
    split::RemoveFromSplit(browser, tab);
  }
  if (tab->IsActivated()) {
    ActivateAnother(browser, tab);
  }
  ResetToPinnedPage(browser->GetTabStripModel(), tab);
}

bool Close(BrowserWindowInterface* browser, tabs::TabInterface* tab) {
  if (!IsUsable(PinnedUrl(tab))) {
    return false;
  }
  auto* contents = tab->GetContents();
  auto* page = PinnedTabPage::FromWebContents(contents);
  if (page->closing) {
    return true;
  }
  if (contents->NeedToFireBeforeUnloadOrUnloadEvents()) {
    page->closing = true;
    contents->DispatchBeforeUnload(/*auto_cancel=*/false);
  } else {
    FinishClose(browser, tab);
  }
  return true;
}

}  // namespace pinned_tabs

// starry_hooks.h：
bool EncodeStartupPin(content::WebContents* contents, base::DictValue& value) {
  auto* page = PinnedTabPage::FromWebContents(contents);
  if (page && page->favorite && !page->favorite_id.empty()) {
    // Every regular window recreates its own sleeping copy from the shared
    // catalog.
    return false;
  }
  std::map<std::string, std::string> extra;
  AddTabExtraData(contents, true, &extra);
  if (auto url = extra.find(kUrlKey); url != extra.end()) {
    value.Set("url", url->second);
    base::DictValue encoded;
    for (auto& [key, text] : extra) {
      encoded.Set(key, std::move(text));
    }
    value.Set("starry_pin", std::move(encoded));
  }
  return true;
}

void EnsureRestoredPin(content::WebContents* contents) {
  // A session predating Starry's extra_data displayed every pin as a tile.
  if (!PinnedTabPage::FromWebContents(contents)) {
    if (auto* page = Remember(contents)) {
      page->favorite = true;
    }
  }
}

void DecodeStartupPin(const base::DictValue& value, StartupTab& tab) {
  if (const auto* extra = value.FindDict("starry_pin")) {
    for (auto [key, entry] : *extra) {
      if (entry.is_string()) {
        tab.pinned_extra_data.emplace(key, entry.GetString());
      }
    }
  } else {
    // Chromium's older pinned_tabs preference contains just the URL.
    tab.pinned_extra_data[kUrlKey] = tab.url.spec();
  }
}

bool RestoreStartupPin(BrowserWindowInterface* browser,
                       const StartupTab& tab,
                       int add_types) {
  if (tab.pinned_extra_data.empty() || tab.is_untrusted_launch ||
      !IsUsable(tab.url)) {
    return false;
  }
  const auto title = tab.pinned_extra_data.find(kTitleKey);
  auto contents = CreateSleepingContents(browser->GetProfile(), tab.url,
                                         title == tab.pinned_extra_data.end()
                                             ? std::u16string()
                                             : base::UTF8ToUTF16(title->second),
                                         nullptr);
  RestoreTabExtraData(contents.get(), /*from_session_restore=*/true,
                      tab.pinned_extra_data);
  auto* model = browser->GetTabStripModel();
  model->InsertWebContentsAt(model->IndexOfFirstNonPinnedTab(),
                             std::move(contents), add_types);
  return true;
}

bool FinishPinnedTabClose(BrowserWindowInterface* browser,
                          content::WebContents* contents) {
  auto* page = PinnedTabPage::FromWebContents(contents);
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!page || !page->closing || !tab || !tab->IsPinned() ||
      browser->GetTabStripModel()->GetIndexOfTab(tab) ==
          TabStripModel::kNoTab) {
    return false;
  }
  pinned_tabs::FinishClose(browser, tab);
  return true;
}

bool HandleCloseTabCommand(BrowserWindowInterface* browser) {
  if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    return false;
  }
  // 设置向导里 ⌘W 不关向导页（向导不能跳过）。
  if (StarryBrowserView* view = StarryBrowserView::From(browser);
      view && view->in_onboarding()) {
    return true;
  }
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  if (!tab) {
    return false;
  }
  // 树状标签页设成「连同子标签一起关」：⌘W 关整枝。
  TabStripModel* model = browser->GetTabStripModel();
  const int index = model->GetIndexOfTab(tab);
  if (!tab->IsPinned() &&
      tab_tree::ClosesBranch(model, index, /*from_sidebar=*/false)) {
    tab_tree::CloseBranch(model, index);
    return true;
  }
  return tab->IsPinned() && pinned_tabs::Close(browser, tab);
}

void AddTabExtraData(content::WebContents* contents,
                     bool pinned,
                     std::map<std::string, std::string>* extra_data) {
  tab_tree::AddExtraData(contents, extra_data);
  if (!pinned) {
    return;
  }
  // 在重写会话文件的中途：只记下，不再另外写一条。
  const PinnedTabPage* page = PinnedTabPage::FromWebContents(contents);
  if (!page) {
    page = Remember(contents);
  }
  if (page) {
    (*extra_data)[kNameKey] = base::UTF16ToUTF8(page->name);
    (*extra_data)[kFavoriteKey] = page->favorite_id;
    (*extra_data)[kSectionKey] = page->favorite ? "favorite" : "pinned";
    (*extra_data)[kFolderKey] = page->folder;
    (*extra_data)[kOrderKey] =
        page->order ? base::NumberToString(*page->order) : std::string();
    (*extra_data)[kUrlKey] = page->url().spec();
    (*extra_data)[kTitleKey] = base::UTF16ToUTF8(page->title());
  }
}

void RestoreTabExtraData(content::WebContents* contents,
                         bool from_session_restore,
                         const std::map<std::string, std::string>& extra_data) {
  tab_tree::RestoreExtraData(contents, from_session_restore, extra_data);
  const auto url = extra_data.find(kUrlKey);
  if (url == extra_data.end()) {
    return;
  }
  const GURL pinned_url(url->second);
  if (!IsUsable(pinned_url)) {
    return;
  }
  const auto title = extra_data.find(kTitleKey);
  auto* page = PinnedTabPage::Set(contents, pinned_url,
                                  title == extra_data.end()
                                      ? std::u16string()
                                      : base::UTF8ToUTF16(title->second));
  if (auto name = extra_data.find(kNameKey); name != extra_data.end()) {
    page->name = base::UTF8ToUTF16(name->second);
  }
  if (auto id = extra_data.find(kFavoriteKey); id != extra_data.end()) {
    page->favorite_id = id->second;
  }
  auto section = extra_data.find(kSectionKey);
  page->favorite = section == extra_data.end() || section->second != "pinned";
  if (page->favorite && !from_session_restore && !page->favorite_id.empty()) {
    auto* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
    if (!profile->GetPrefs()
             ->GetDict(prefs::kFavorites)
             .FindDict(page->favorite_id)) {
      PublishFavorite(profile, *page);
    }
  }
  if (auto folder = extra_data.find(kFolderKey);
      !page->favorite && folder != extra_data.end()) {
    page->folder = folder->second;
  }
  double value = 0;
  if (auto order = extra_data.find(kOrderKey);
      !page->favorite && order != extra_data.end() &&
      base::StringToDouble(order->second, &value)) {
    page->order = value;
  }
}

}  // namespace starry
