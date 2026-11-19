// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/favorite_folders.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "starry/browser/ui/tabs/tabs_view.h"

namespace starry::favorite_folders {
namespace {

using pin_folders::Child;

// 几处一起改（标签的位置、排序值、文件夹），标签只重排一次。
class Batch {
 public:
  explicit Batch(BrowserWindowInterface* browser)
      : tabs_(pin_folders::VisibleTabs(browser)) {
    if (tabs_) {
      tabs_->BeginBatch();
    }
  }
  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;
  ~Batch() {
    if (tabs_) {
      tabs_->EndBatch(TabListView::Change::kInsert);
    }
  }

 private:
  raw_ptr<TabsView> tabs_;
};

// 新建一个文件夹，排在图块那一层的最后。
std::string Add(Profile* profile, const std::u16string& name) {
  const int order = pinned_tabs::NextFavoriteOrder(profile);
  std::string id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::DictValue value;
  value.Set("name", name);
  value.Set("order", order);
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kFavoriteFolders);
  update->Set(id, std::move(value));
  return id;
}

// 这一层按 order 的先后重新编排序值（1、2、3……）。
void Renumber(BrowserWindowInterface* browser,
              const std::vector<Child>& order) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  const auto& stored = settings->GetDict(prefs::kFavoriteFolders);
  std::vector<std::pair<tabs::TabHandle, int>> tab_orders;
  std::vector<std::pair<std::string, int>> folder_orders;
  for (size_t i = 0; i < order.size(); ++i) {
    const int value = static_cast<int>(i) + 1;
    if (order[i].is_folder()) {
      const auto* folder = stored.FindDict(order[i].folder);
      if (folder && folder->FindInt("order") != value) {
        folder_orders.emplace_back(order[i].folder, value);
      }
    } else {
      tab_orders.emplace_back(order[i].tab, value);
    }
  }
  pinned_tabs::SetFavoriteOrders(browser, tab_orders);
  // 没有要改的就不碰设置：每改一次，所有窗口都要重排一遍。
  if (folder_orders.empty()) {
    return;
  }
  ScopedDictPrefUpdate update(settings, prefs::kFavoriteFolders);
  for (const auto& [id, value] : folder_orders) {
    if (auto* folder = update->FindDict(id)) {
      folder->Set("order", value);
    }
  }
}

// 删掉文件夹和里面的常用标签：这个窗口里的关掉（进「最近关闭」），别的窗口里
// 的那几份跟着设置没了。
void DeleteNow(BrowserWindowInterface* browser, const std::string& id) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  if (!settings->GetDict(prefs::kFavoriteFolders).FindDict(id)) {
    return;
  }
  std::vector<tabs::TabHandle> handles;
  for (tabs::TabInterface* tab : TabsIn(browser, id)) {
    handles.push_back(tab->GetHandle());
  }
  Batch batch(browser);
  for (const tabs::TabHandle& handle : handles) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && tab->GetBrowserWindowInterface() == browser) {
      pinned_tabs::Archive(tab, /*to_tab_archive=*/false);
    }
  }
  std::vector<std::string> inside;
  for (auto [key, value] : settings->GetDict(prefs::kFavorites)) {
    const auto* entry = value.GetIfDict();
    const auto* folder = entry ? entry->FindString("folder") : nullptr;
    if (folder && *folder == id) {
      inside.push_back(key);
    }
  }
  if (!inside.empty()) {
    ScopedDictPrefUpdate update(settings, prefs::kFavorites);
    for (const std::string& key : inside) {
      update->Remove(key);
    }
  }
  ScopedDictPrefUpdate update(settings, prefs::kFavoriteFolders);
  update->Remove(id);
}

}  // namespace

std::vector<pin_folders::Folder> Read(Profile* profile) {
  std::vector<pin_folders::Folder> result;
  for (auto [id, value] :
       profile->GetPrefs()->GetDict(prefs::kFavoriteFolders)) {
    const auto* data = value.GetIfDict();
    const auto* name = data ? data->FindString("name") : nullptr;
    if (!name) {
      continue;
    }
    pin_folders::Folder folder;
    folder.id = id;
    folder.name = base::UTF8ToUTF16(*name);
    folder.path = folder.name;
    folder.order = data->FindInt("order").value_or(0);
    if (const auto* color = data->FindString("color");
        color && std::ranges::find(pin_folders::kColors, *color) !=
                     pin_folders::kColors.end()) {
      folder.color = *color;
    }
    result.push_back(std::move(folder));
  }
  std::ranges::stable_sort(result, {}, [](const pin_folders::Folder& folder) {
    return std::make_tuple(folder.order.value_or(0), folder.name);
  });
  return result;
}

std::optional<pin_folders::Folder> Find(Profile* profile,
                                        const std::string& id) {
  for (pin_folders::Folder& folder : Read(profile)) {
    if (folder.id == id) {
      return std::move(folder);
    }
  }
  return std::nullopt;
}

pin_folders::Levels Layout(BrowserWindowInterface* browser) {
  struct Entry {
    Child child;
    int order = 0;
    // 排序值相同的：标签按模型里的先后，文件夹在后面、按 Read 的先后。
    int rank = 0;
  };
  std::map<std::string, std::vector<Entry>> levels;
  levels[std::string()];
  const std::vector<pin_folders::Folder> folders = Read(browser->GetProfile());
  std::set<std::string> ids;
  for (const pin_folders::Folder& folder : folders) {
    ids.insert(folder.id);
    levels[folder.id];
  }
  TabStripModel* model = browser->GetTabStripModel();
  const int pinned = model->IndexOfFirstNonPinnedTab();
  for (int i = 0; i < pinned; ++i) {
    tabs::TabInterface* tab = model->GetTabAtIndex(i);
    if (!pinned_tabs::IsFavorite(tab)) {
      continue;
    }
    // 文件夹没了（还没同步过来）的算在图块那一层。
    std::string folder = pinned_tabs::FavoriteFolder(tab);
    if (!ids.contains(folder)) {
      folder.clear();
    }
    levels[folder].push_back({Child{tab->GetHandle(), std::string()},
                              pinned_tabs::FavoriteOrder(tab), i});
  }
  int rank = pinned;
  for (const pin_folders::Folder& folder : folders) {
    levels[std::string()].push_back({Child{tabs::TabHandle(), folder.id},
                                     static_cast<int>(folder.order.value_or(0)),
                                     rank++});
  }
  pin_folders::Levels result;
  for (auto& [id, entries] : levels) {
    std::ranges::stable_sort(entries, {}, [](const Entry& entry) {
      return std::make_pair(entry.order, entry.rank);
    });
    std::vector<Child>& children = result[id];
    for (Entry& entry : entries) {
      children.push_back(std::move(entry.child));
    }
  }
  return result;
}

std::vector<tabs::TabInterface*> TabsIn(BrowserWindowInterface* browser,
                                        const std::string& id) {
  std::vector<tabs::TabInterface*> result;
  const pin_folders::Levels levels = Layout(browser);
  auto level = levels.find(id);
  if (id.empty() || level == levels.end()) {
    return result;
  }
  for (const Child& child : level->second) {
    if (tabs::TabInterface* tab = child.tab.Get()) {
      result.push_back(tab);
    }
  }
  return result;
}

bool Place(BrowserWindowInterface* browser,
           const Child& item,
           const std::string& folder,
           const Child& before) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  const auto& folders = settings->GetDict(prefs::kFavoriteFolders);
  // 要放进去的文件夹可能刚在别的窗口里删了：放在图块那一层。
  const std::string level = folders.FindDict(folder) ? folder : std::string();
  Batch batch(browser);
  if (item.is_folder()) {
    if (!folders.FindDict(item.folder) || !level.empty()) {
      return false;
    }
  } else {
    tabs::TabInterface* tab = item.tab.Get();
    if (!tab || tab->GetBrowserWindowInterface() != browser ||
        !pinned_tabs::SetFavoriteFolder(tab, level)) {
      return false;
    }
  }
  std::vector<Child> order = Layout(browser)[level];
  std::erase(order, item);
  order.insert(before.empty() ? order.end() : std::ranges::find(order, before),
               item);
  Renumber(browser, order);
  return true;
}

bool CanCreate(BrowserWindowInterface* browser, tabs::TabInterface* tab) {
  return pinned_tabs::HasRoomForFavorite(browser) ||
         (tab && pinned_tabs::IsFavorite(tab) &&
          pinned_tabs::FavoriteFolder(tab).empty());
}

std::string CreateNamed(BrowserWindowInterface* browser,
                        const std::u16string& name,
                        tabs::TabHandle handle) {
  tabs::TabInterface* tab = handle.Get();
  if (tab && tab->GetBrowserWindowInterface() != browser) {
    tab = nullptr;
  }
  if (name.empty() || !CanCreate(browser, tab)) {
    return std::string();
  }
  Batch batch(browser);
  const std::string id = Add(browser->GetProfile(), name);
  // 从图块那一层的常用标签建的：文件夹出现在它的位置上。
  Child at;
  if (tab && pinned_tabs::IsFavorite(tab) &&
      pinned_tabs::FavoriteFolder(tab).empty()) {
    at.tab = handle;
  }
  favorite_folders::Place(browser, Child{tabs::TabHandle(), id}, std::string(),
                          at);
  if (tab) {
    favorite_folders::Place(browser, Child{handle, std::string()}, id);
  }
  return id;
}

void Create(BrowserWindowInterface* browser, tabs::TabHandle tab) {
  auto* view = StarryBrowserView::From(browser);
  if (!view || !CanCreate(browser, tab.Get())) {
    return;
  }
  DialogSpec spec;
  spec.title = L("tabs.newFavoriteFolder");
  spec.field = std::u16string();
  spec.buttons = {{L("common.cancel"), ButtonVariant::kTertiary, true},
                  {L("common.save"), ButtonVariant::kFilled}};
  view->PresentDialog(
      std::move(spec),
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> window, tabs::TabHandle tab,
             std::optional<size_t> choice, const std::u16string& input) {
            if (!window || choice != 1) {
              return;
            }
            std::u16string name;
            base::TrimWhitespace(input, base::TRIM_ALL, &name);
            CreateNamed(window.get(), name, tab);
          },
          browser->GetWeakPtr(), tab));
}

void Rename(BrowserWindowInterface* browser, const std::string& id) {
  auto* view = StarryBrowserView::From(browser);
  const std::optional<pin_folders::Folder> folder =
      Find(browser->GetProfile(), id);
  if (!view || !folder) {
    return;
  }
  DialogSpec spec;
  spec.title = L("tabs.renameFolder");
  spec.field = folder->name;
  spec.buttons = {{L("common.cancel"), ButtonVariant::kTertiary, true},
                  {L("common.save"), ButtonVariant::kFilled}};
  view->PresentDialog(
      std::move(spec),
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> window, std::string id,
             std::optional<size_t> choice, const std::u16string& input) {
            if (window && choice == 1) {
              SetName(window->GetProfile(), id, input);
            }
          },
          browser->GetWeakPtr(), id));
}

void SetName(Profile* profile,
             const std::string& id,
             const std::u16string& input) {
  std::u16string name;
  base::TrimWhitespace(input, base::TRIM_ALL, &name);
  PrefService* settings = profile->GetPrefs();
  const auto* current = settings->GetDict(prefs::kFavoriteFolders).FindDict(id);
  const auto* old = current ? current->FindString("name") : nullptr;
  if (name.empty() || !current || (old && base::UTF8ToUTF16(*old) == name)) {
    return;
  }
  ScopedDictPrefUpdate update(settings, prefs::kFavoriteFolders);
  update->FindDict(id)->Set("name", name);
}

std::string Merge(BrowserWindowInterface* browser,
                  tabs::TabHandle onto,
                  tabs::TabHandle dragged) {
  tabs::TabInterface* target = onto.Get();
  tabs::TabInterface* tab = dragged.Get();
  if (!target || !tab || target == tab ||
      target->GetBrowserWindowInterface() != browser ||
      tab->GetBrowserWindowInterface() != browser ||
      !pinned_tabs::IsFavorite(target) ||
      !pinned_tabs::FavoriteFolder(target).empty()) {
    return std::string();
  }
  Batch batch(browser);
  const std::string id =
      CreateNamed(browser, L("tabs.folderDefaultName"), onto);
  if (!id.empty()) {
    favorite_folders::Place(browser, Child{dragged, std::string()}, id);
  }
  return id;
}

void SetColor(Profile* profile,
              const std::string& id,
              const std::string& color) {
  const auto* current =
      profile->GetPrefs()->GetDict(prefs::kFavoriteFolders).FindDict(id);
  if (!current) {
    return;
  }
  const auto* old = current->FindString("color");
  if ((old ? *old : std::string()) == color) {
    return;
  }
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kFavoriteFolders);
  auto* value = update->FindDict(id);
  if (color.empty()) {
    value->Remove("color");
  } else {
    value->Set("color", color);
  }
}

void Dissolve(BrowserWindowInterface* browser, const std::string& id) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  if (!settings->GetDict(prefs::kFavoriteFolders).FindDict(id)) {
    return;
  }
  Batch batch(browser);
  pin_folders::Levels levels = Layout(browser);
  const std::vector<Child> inside = levels[id];
  std::vector<Child> order = levels[std::string()];
  auto at = std::ranges::find(order, Child{tabs::TabHandle(), id});
  if (at != order.end()) {
    at = order.erase(at);
  }
  order.insert(at, inside.begin(), inside.end());
  pinned_tabs::ClearFavoriteFolder(browser, id);
  {
    ScopedDictPrefUpdate update(settings, prefs::kFavoriteFolders);
    update->Remove(id);
  }
  Renumber(browser, order);
}

void Delete(BrowserWindowInterface* browser, const std::string& id) {
  const std::optional<pin_folders::Folder> folder =
      Find(browser->GetProfile(), id);
  if (!folder) {
    return;
  }
  const size_t count = TabsIn(browser, id).size();
  if (count == 0 || !StarryBrowserView::From(browser)) {
    DeleteNow(browser, id);
    return;
  }
  pin_folders::ConfirmDelete(
      browser, folder->name, count, /*favorites=*/true,
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> window, std::string id) {
            if (window) {
              DeleteNow(window.get(), id);
            }
          },
          browser->GetWeakPtr(), id));
}

std::string DescribeForTesting(BrowserWindowInterface* browser) {
  const pin_folders::Levels levels = Layout(browser);
  std::map<std::string, std::u16string> names;
  for (const pin_folders::Folder& folder : Read(browser->GetProfile())) {
    names[folder.id] = folder.name;
  }
  auto tab_name = [](const Child& child) {
    tabs::TabInterface* tab = child.tab.Get();
    return tab ? base::UTF16ToUTF8(pinned_tabs::DisplayName(tab))
               : std::string("?");
  };
  std::string text;
  auto grid = levels.find(std::string());
  if (grid == levels.end()) {
    return text;
  }
  for (const Child& child : grid->second) {
    if (!text.empty()) {
      text += ", ";
    }
    if (!child.is_folder()) {
      text += tab_name(child);
      continue;
    }
    std::string inner;
    if (auto inside = levels.find(child.folder); inside != levels.end()) {
      for (const Child& kid : inside->second) {
        if (!inner.empty()) {
          inner += ", ";
        }
        inner += tab_name(kid);
      }
    }
    text += "[" + base::UTF16ToUTF8(names[child.folder]) +
            (inner.empty() ? "" : ": " + inner) + "]";
  }
  return text;
}

}  // namespace starry::favorite_folders
