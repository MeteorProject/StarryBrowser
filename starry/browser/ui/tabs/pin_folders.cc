// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/pin_folders.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/sessions/core/session_id.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "starry/browser/ui/split/split_tabs.h"
#include "starry/browser/ui/tabs/favorite_folders.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "starry/browser/ui/tabs/sidebar_tabs.h"
#include "starry/browser/ui/tabs/top_tab_strip.h"
#include "starry/common/internal_urls.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/image/canvas_image_source.h"
#include "url/gurl.h"

namespace starry::pin_folders {
namespace {
enum {
  kCreate = 1,
  kRename,
  kRemove,
  kMove,
  kRoot,
  kAddCurrent,
  kColorMenu,
  kColorNone,
  kOpenAll,
  kSleepAll,
  kSplit,
  kDelete,
  kFirstColor = 100,
  kFirstFolder = 1000,
};

// 文件夹最多往里找这么多层（设置被改坏了也不会没完没了）。
constexpr int kMaxDepth = 64;

}  // namespace

TabsView* VisibleTabs(BrowserWindowInterface* browser) {
  auto* view = StarryBrowserView::From(browser);
  if (!view || !view->has_starry_chrome()) {
    return nullptr;
  }
  if (view->chrome_layout().tab_strip()) {
    return view->tab_strip();
  }
  return view->sidebar()->tabs();
}

namespace {

// 文件夹现在的上一层（上一层没了的算在最外层）。
std::string ParentOf(const base::DictValue& folders, const std::string& id) {
  const auto* folder = folders.FindDict(id);
  const auto* parent = folder ? folder->FindString("parent") : nullptr;
  return parent && folders.FindDict(*parent) ? *parent : std::string();
}

// 每个文件夹归哪个窗口（SessionID），和开着的窗口里各有几个标签列表。
std::map<std::string, SessionID::id_type>& Homes() {
  static base::NoDestructor<std::map<std::string, SessionID::id_type>> homes;
  return *homes;
}

std::map<SessionID::id_type, int>& LiveWindows() {
  static base::NoDestructor<std::map<SessionID::id_type, int>> windows;
  return *windows;
}

bool Alive(SessionID::id_type window) {
  auto found = LiveWindows().find(window);
  return found != LiveWindows().end() && found->second > 0;
}

void NoteHome(const std::string& id, BrowserWindowInterface* browser) {
  Homes()[id] = browser->GetSessionID().id();
}

// 新文件夹放在哪一层、排在谁前面：从一个固定标签建的，出现在这个标签的位置
// 上（和它同一层），不然放在 parent 的最后。
std::pair<std::string, Child> NewFolderSpot(BrowserWindowInterface* browser,
                                            const std::string& parent,
                                            tabs::TabHandle move_tab) {
  std::string level = parent;
  Child at;
  if (move_tab.Get()) {
    for (const auto& [id, children] : Layout(browser)) {
      if (std::ranges::find(children, Child{move_tab, std::string()}) !=
          children.end()) {
        level = id;
        at.tab = move_tab;
      }
    }
  }
  return {level, at};
}

void Edit(BrowserWindowInterface* browser,
          Folder folder,
          bool create,
          tabs::TabHandle move_tab) {
  auto* view = StarryBrowserView::From(browser);
  if (!view) {
    return;
  }
  DialogSpec spec;
  spec.title = create ? L("tabs.newFolder") : L("tabs.renameFolder");
  spec.field = create ? std::u16string() : folder.name;
  spec.buttons = {{L("common.cancel"), ButtonVariant::kTertiary, true},
                  {L("common.save"), ButtonVariant::kFilled}};
  view->PresentDialog(
      std::move(spec),
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> window, Folder folder,
             bool create, tabs::TabHandle move_tab,
             std::optional<size_t> choice, const std::u16string& input) {
            if (!window || choice != 1) {
              return;
            }
            std::u16string name;
            base::TrimWhitespace(input, base::TRIM_ALL, &name);
            if (name.empty()) {
              return;
            }
            if (!create) {
              Rename(window->GetProfile(), folder.id, name);
              return;
            }
            // 同侧栏里就地新建：从固定标签建的出现在它的位置上，几处一起改，
            // 标签只重排一次。
            tabs::TabInterface* tab = move_tab.Get();
            if (tab && tab->GetBrowserWindowInterface() != window.get()) {
              tab = nullptr;
            }
            const auto [level, at] = NewFolderSpot(
                window.get(), folder.id, tab ? move_tab : tabs::TabHandle());
            TabsView* tabs = VisibleTabs(window.get());
            if (tabs) {
              tabs->BeginBatch();
            }
            const std::string id = Add(window->GetProfile(), level, name);
            NoteHome(id, window.get());
            Place(window.get(), Child{tabs::TabHandle(), id}, level, at);
            if (tab) {
              pinned_tabs::SetLocation(tab, false, id);
            }
            if (tabs) {
              tabs->EndBatch(TabListView::Change::kFold);
            }
          },
          browser->GetWeakPtr(), std::move(folder), create, move_tab));
}

// 侧栏的标签列表：文件夹在那里是一行，能就地起名。顶部标签布局下没有。
TabsView* InlineTabs(BrowserWindowInterface* browser) {
  auto* view = StarryBrowserView::From(browser);
  if (!view || !view->has_starry_chrome() ||
      view->chrome_layout().tab_strip()) {
    return nullptr;
  }
  return view->sidebar()->tabs();
}

}  // namespace

bool CanSleep(tabs::TabInterface* tab) {
  content::WebContents* contents = tab->GetContents();
  if (contents->WasDiscarded() || contents->IsCurrentlyAudible()) {
    return false;
  }
  const GURL& url = contents->GetLastCommittedURL().is_empty()
                        ? contents->GetVisibleURL()
                        : contents->GetLastCommittedURL();
  if (IsBlankTabUrl(url) || InternalPageForUrl(url) != InternalPage::kNone ||
      IsInternalURL(url) || url.SchemeIs("devtools")) {
    return false;
  }
  const tabs::TabData data = tabs::TabData::FromTabInterface(tab);
  if (data.alert_state) {
    switch (*data.alert_state) {
      case tabs::TabAlert::kMediaRecording:
      case tabs::TabAlert::kTabCapturing:
      case tabs::TabAlert::kDesktopCapturing:
      case tabs::TabAlert::kAudioRecording:
      case tabs::TabAlert::kVideoRecording:
        return false;
      default:
        break;
    }
  }
  return true;
}

namespace {

// 删掉文件夹和里面的文件夹，这个窗口里在里面的标签关掉（进「最近关闭」）。
void DeleteNow(BrowserWindowInterface* browser, const std::string& id) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  if (!settings->GetDict(prefs::kPinFolders).FindDict(id)) {
    return;
  }
  std::vector<tabs::TabHandle> handles;
  for (tabs::TabInterface* tab : TabsIn(browser, id)) {
    handles.push_back(tab->GetHandle());
  }
  std::set<std::string> doomed{id};
  for (const Folder& folder : Read(browser->GetProfile())) {
    if (doomed.contains(folder.parent)) {
      doomed.insert(folder.id);
    }
  }
  TabsView* tabs = VisibleTabs(browser);
  if (tabs) {
    tabs->BeginBatch();
  }
  for (const tabs::TabHandle& handle : handles) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && tab->GetBrowserWindowInterface() == browser) {
      pinned_tabs::Archive(tab, /*to_tab_archive=*/false);
    }
  }
  {
    ScopedDictPrefUpdate update(settings, prefs::kPinFolders);
    for (const std::string& folder : doomed) {
      update->Remove(folder);
    }
  }
  if (tabs) {
    tabs->EndBatch(TabListView::Change::kFold);
  }
}

ui::ImageModel MenuIcon(const gfx::VectorIcon& icon) {
  // 颜色由菜单按自己的主题画（MenuHostView），这里的颜色只是占位。
  return ui::ImageModel::FromVectorIcon(icon, ui::kColorMenuIcon, 14);
}

// 颜色菜单里的圆点：选了颜色是实心的，「无」是一个圈。
class ColorDot : public gfx::CanvasImageSource {
 public:
  ColorDot(std::optional<SkColor> color, SkColor ring)
      : gfx::CanvasImageSource(gfx::Size(16, 16)), color_(color), ring_(ring) {}

  void Draw(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    if (color_) {
      flags.setColor(*color_);
      canvas->DrawCircle(gfx::PointF(8, 8), 5.5f, flags);
      return;
    }
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1.2f);
    flags.setColor(ring_);
    canvas->DrawCircle(gfx::PointF(8, 8), 5, flags);
  }

 private:
  const std::optional<SkColor> color_;
  const SkColor ring_;
};

ui::ImageModel DotIcon(std::optional<SkColor> color, const Theme& theme) {
  return ui::ImageModel::FromImageSkia(
      gfx::CanvasImageSource::MakeImageSkia<ColorDot>(
          color, theme.on_surface_variant));
}

std::u16string ColorTitle(std::string_view color) {
  if (color == "red") {
    return L("tabs.folderColor.red");
  }
  if (color == "orange") {
    return L("tabs.folderColor.orange");
  }
  if (color == "yellow") {
    return L("tabs.folderColor.yellow");
  }
  if (color == "green") {
    return L("tabs.folderColor.green");
  }
  if (color == "blue") {
    return L("tabs.folderColor.blue");
  }
  return L("tabs.folderColor.purple");
}

}  // namespace

std::vector<Folder> Read(Profile* profile) {
  const auto& dict = profile->GetPrefs()->GetDict(prefs::kPinFolders);
  std::map<std::string, std::vector<Folder>> children;
  for (auto [id, value] : dict) {
    const auto* data = value.GetIfDict();
    const auto* name = data ? data->FindString("name") : nullptr;
    if (!name) {
      continue;
    }
    const auto* parent = data->FindString("parent");
    Folder folder;
    folder.id = id;
    if (parent && dict.FindDict(*parent)) {
      folder.parent = *parent;
    }
    folder.name = base::UTF8ToUTF16(*name);
    folder.collapsed = data->FindBool("collapsed").value_or(false);
    folder.order = data->FindDouble("order");
    if (const auto* color = data->FindString("color");
        color && std::ranges::find(kColors, *color) != kColors.end()) {
      folder.color = *color;
    }
    children[folder.parent].push_back(std::move(folder));
  }
  for (auto& [parent, siblings] : children) {
    std::ranges::stable_sort(siblings, {}, [](const Folder& folder) {
      return std::make_tuple(!folder.order.has_value(),
                             folder.order.value_or(0), folder.name);
    });
  }
  std::vector<Folder> result;
  // Iterate rather than recurse so even a deeply nested imported profile is
  // safe.
  std::vector<Folder> stack(children[""].rbegin(), children[""].rend());
  while (!stack.empty()) {
    Folder folder = std::move(stack.back());
    stack.pop_back();
    folder.path += folder.name;
    auto& nested = children[folder.id];
    for (auto it = nested.rbegin(); it != nested.rend(); ++it) {
      it->depth = folder.depth + 1;
      it->visible = folder.visible && !folder.collapsed;
      it->path = folder.path + u" / ";
      stack.push_back(std::move(*it));
    }
    result.push_back(std::move(folder));
  }
  return result;
}

void Toggle(Profile* profile, const std::string& id) {
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kPinFolders);
  if (auto* folder = update->FindDict(id)) {
    folder->Set("collapsed", !folder->FindBool("collapsed").value_or(false));
  }
}

void SetCollapsed(Profile* profile, const std::string& id, bool collapsed) {
  const auto* current =
      profile->GetPrefs()->GetDict(prefs::kPinFolders).FindDict(id);
  if (!current || current->FindBool("collapsed").value_or(false) == collapsed) {
    return;
  }
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kPinFolders);
  update->FindDict(id)->Set("collapsed", collapsed);
}

void SetTreeCollapsed(Profile* profile, const std::string& id, bool collapsed) {
  std::set<std::string> tree{id};
  for (const Folder& folder : Read(profile)) {
    if (tree.contains(folder.parent)) {
      tree.insert(folder.id);
    }
  }
  // 一次改完：所有窗口只重排一遍。
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kPinFolders);
  for (const std::string& folder : tree) {
    if (auto* value = update->FindDict(folder)) {
      value->Set("collapsed", collapsed);
    }
  }
}

std::optional<SkColor> ColorValue(std::string_view color, bool dark) {
  if (color == "red") {
    return dark ? SkColorSetRGB(0xFF, 0x45, 0x3A)
                : SkColorSetRGB(0xFF, 0x3B, 0x30);
  }
  if (color == "orange") {
    return dark ? SkColorSetRGB(0xFF, 0x9F, 0x0A)
                : SkColorSetRGB(0xFF, 0x95, 0x00);
  }
  if (color == "yellow") {
    return dark ? SkColorSetRGB(0xFF, 0xD6, 0x0A)
                : SkColorSetRGB(0xE6, 0xB8, 0x00);
  }
  if (color == "green") {
    return dark ? SkColorSetRGB(0x30, 0xD1, 0x58)
                : SkColorSetRGB(0x28, 0xB4, 0x4C);
  }
  if (color == "blue") {
    return dark ? SkColorSetRGB(0x0A, 0x84, 0xFF)
                : SkColorSetRGB(0x00, 0x7A, 0xFF);
  }
  if (color == "purple") {
    return dark ? SkColorSetRGB(0xBF, 0x5A, 0xF2)
                : SkColorSetRGB(0xAF, 0x52, 0xDE);
  }
  return std::nullopt;
}

void SetColor(Profile* profile,
              const std::string& id,
              const std::string& color) {
  const auto* current =
      profile->GetPrefs()->GetDict(prefs::kPinFolders).FindDict(id);
  if (!current) {
    return;
  }
  const auto* old = current->FindString("color");
  if ((old ? *old : std::string()) == color) {
    return;
  }
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kPinFolders);
  auto* value = update->FindDict(id);
  if (color.empty()) {
    value->Remove("color");
  } else {
    value->Set("color", color);
  }
}

Levels Layout(BrowserWindowInterface* browser) {
  struct Entry {
    Child child;
    bool unordered = false;
    double order = 0;
    // 没排过的、排序值相同的之间：标签按模型里的先后，文件夹按 Read 的先后。
    int rank = 0;
  };
  std::map<std::string, std::vector<Entry>> levels;
  std::set<std::string> ids;
  int rank = 0;
  for (const Folder& folder : Read(browser->GetProfile())) {
    ids.insert(folder.id);
    levels[folder.parent].push_back({Child{tabs::TabHandle(), folder.id},
                                     !folder.order.has_value(),
                                     folder.order.value_or(0), rank++});
  }
  TabStripModel* model = browser->GetTabStripModel();
  for (int i = 0; i < model->IndexOfFirstNonPinnedTab(); ++i) {
    tabs::TabInterface* tab = model->GetTabAtIndex(i);
    if (pinned_tabs::IsFavorite(tab)) {
      continue;
    }
    // 文件夹没了（解散了、在别的窗口里删了）的标签显示在最外层。
    std::string parent = pinned_tabs::Folder(tab);
    if (!ids.contains(parent)) {
      parent.clear();
    }
    const std::optional<double> order = pinned_tabs::Order(tab);
    levels[parent].push_back({Child{tab->GetHandle(), std::string()},
                              !order.has_value(), order.value_or(0), i});
  }
  Levels result;
  for (auto& [parent, entries] : levels) {
    std::ranges::stable_sort(entries, {}, [](const Entry& entry) {
      return std::make_tuple(entry.unordered, entry.order,
                             entry.child.is_folder(), entry.rank);
    });
    std::vector<Child>& children = result[parent];
    for (Entry& entry : entries) {
      children.push_back(std::move(entry.child));
    }
  }
  return result;
}

std::set<std::string> ShownIn(BrowserWindowInterface* browser) {
  Profile* profile = browser->GetProfile();
  const auto& folders = profile->GetPrefs()->GetDict(prefs::kPinFolders);
  const SessionID::id_type self = browser->GetSessionID().id();
  // 里面（含子文件夹）有这个窗口、别的窗口的固定标签的文件夹。
  std::set<std::string> mine;
  std::set<std::string> others;
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    TabStripModel* model =
        window->GetProfile() == profile ? window->GetTabStripModel() : nullptr;
    if (!model) {
      continue;
    }
    std::set<std::string>& target = window == browser ? mine : others;
    for (int i = 0; i < model->IndexOfFirstNonPinnedTab(); ++i) {
      tabs::TabInterface* tab = model->GetTabAtIndex(i);
      if (pinned_tabs::IsFavorite(tab)) {
        continue;
      }
      std::string up = pinned_tabs::Folder(tab);
      for (int depth = 0; !up.empty() && folders.FindDict(up) &&
                          depth < kMaxDepth && target.insert(up).second;
           ++depth) {
        up = ParentOf(folders, up);
      }
    }
  }
  std::map<std::string, SessionID::id_type>& homes = Homes();
  std::set<std::string> shown;
  for (auto [id, value] : folders) {
    if (!value.is_dict()) {
      continue;
    }
    auto home = homes.find(id);
    const bool owned = home != homes.end() && Alive(home->second);
    if (mine.contains(id)) {
      // 最先在哪个开着的窗口里有标签，空了以后还留在那个窗口里。
      if (!owned) {
        homes[id] = self;
      }
      shown.insert(id);
    } else if (owned ? home->second == self : !others.contains(id)) {
      shown.insert(id);
    }
  }
  // 显示的文件夹的上一层也显示。
  for (const std::string& id : std::set<std::string>(shown)) {
    std::string up = ParentOf(folders, id);
    for (int depth = 0;
         !up.empty() && depth < kMaxDepth && shown.insert(up).second; ++depth) {
      up = ParentOf(folders, up);
    }
  }
  return shown;
}

void WindowShown(BrowserWindowInterface* browser) {
  ++LiveWindows()[browser->GetSessionID().id()];
}

void WindowGone(BrowserWindowInterface* browser) {
  auto& live = LiveWindows();
  auto found = live.find(browser->GetSessionID().id());
  if (found == live.end() || --found->second > 0) {
    return;
  }
  live.erase(found);
  if (browser_shutdown::HasShutdownStarted()) {
    return;
  }
  // 归这个窗口的空文件夹没有主了：别的窗口重新算一遍（可能要在那里出现）。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<Profile> profile) {
                       if (!profile || browser_shutdown::HasShutdownStarted()) {
                         return;
                       }
                       for (BrowserWindowInterface* window :
                            GetAllBrowserWindowInterfaces()) {
                         if (window->GetProfile() != profile.get() ||
                             !Alive(window->GetSessionID().id())) {
                           continue;
                         }
                         if (TabsView* tabs = VisibleTabs(window)) {
                           tabs->RefreshFolders();
                         }
                       }
                     },
                     browser->GetProfile()->GetWeakPtr()));
}

namespace {

// 这一层按 order 的先后重新编排序值（1、2、3……）。
void Renumber(BrowserWindowInterface* browser,
              const std::vector<Child>& order) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  std::vector<std::pair<std::string, double>> changed;
  for (size_t i = 0; i < order.size(); ++i) {
    const double value = static_cast<double>(i + 1);
    if (order[i].is_folder()) {
      const auto* folder =
          settings->GetDict(prefs::kPinFolders).FindDict(order[i].folder);
      if (folder && folder->FindDouble("order") != value) {
        changed.emplace_back(order[i].folder, value);
      }
    } else if (tabs::TabInterface* tab = order[i].tab.Get()) {
      pinned_tabs::SetOrder(tab, value);
    }
  }
  // 没有要改的就不碰设置：每改一次，所有窗口的侧栏都要重排一遍。
  if (changed.empty()) {
    return;
  }
  ScopedDictPrefUpdate update(settings, prefs::kPinFolders);
  for (const auto& [id, value] : changed) {
    if (auto* folder = update->FindDict(id)) {
      folder->Set("order", value);
    }
  }
}

}  // namespace

void Place(BrowserWindowInterface* browser,
           const Child& item,
           const std::string& parent,
           const Child& before) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  // 要放进去的文件夹可能刚在别的窗口里删了：放在最外层。
  const std::string level =
      settings->GetDict(prefs::kPinFolders).FindDict(parent) ? parent
                                                             : std::string();
  if (item.is_folder()) {
    const auto& folders = settings->GetDict(prefs::kPinFolders);
    if (!folders.FindDict(item.folder)) {
      return;
    }
    // 不能放进自己或者自己里面的文件夹。
    std::set<std::string> visited{item.folder};
    for (std::string up = level; !up.empty(); up = ParentOf(folders, up)) {
      if (!visited.insert(up).second) {
        return;
      }
    }
    const auto* stored = folders.FindDict(item.folder)->FindString("parent");
    if ((stored ? *stored : std::string()) != level) {
      ScopedDictPrefUpdate update(settings, prefs::kPinFolders);
      update->FindDict(item.folder)->Set("parent", level);
    }
  } else {
    tabs::TabInterface* tab = item.tab.Get();
    if (!tab || tab->GetBrowserWindowInterface() != browser) {
      return;
    }
    if (!tab->IsPinned() || pinned_tabs::IsFavorite(tab) ||
        pinned_tabs::Folder(tab) != level) {
      pinned_tabs::SetLocation(tab, false, level);
    }
    // 空白标签没有网址可记，固定不了。
    if (!tab->IsPinned() || pinned_tabs::IsFavorite(tab)) {
      return;
    }
  }
  std::vector<Child> order = Layout(browser)[level];
  std::erase(order, item);
  order.insert(before.empty() ? order.end() : std::ranges::find(order, before),
               item);
  Renumber(browser, order);
}

void Dissolve(BrowserWindowInterface* browser, const std::string& id) {
  PrefService* settings = browser->GetProfile()->GetPrefs();
  if (!settings->GetDict(prefs::kPinFolders).FindDict(id)) {
    return;
  }
  const std::string parent =
      ParentOf(settings->GetDict(prefs::kPinFolders), id);
  Levels levels = Layout(browser);
  const std::vector<Child> inside = levels[id];
  std::vector<Child> order = levels[parent];
  auto at = std::ranges::find(order, Child{tabs::TabHandle(), id});
  if (at != order.end()) {
    at = order.erase(at);
  }
  order.insert(at, inside.begin(), inside.end());
  {
    ScopedDictPrefUpdate update(settings, prefs::kPinFolders);
    for (const Child& child : inside) {
      if (auto* folder = update->FindDict(child.folder)) {
        folder->Set("parent", parent);
      }
    }
    update->Remove(id);
  }
  for (const Child& child : inside) {
    if (tabs::TabInterface* tab = child.tab.Get()) {
      pinned_tabs::SetLocation(tab, false, parent);
    }
  }
  Renumber(browser, order);
}

std::string DescribeForTesting(BrowserWindowInterface* browser) {
  const Levels levels = Layout(browser);
  const std::set<std::string> shown = ShownIn(browser);
  std::map<std::string, std::u16string> names;
  for (const Folder& folder : Read(browser->GetProfile())) {
    names[folder.id] = folder.name;
  }
  // 文件夹不会成环（Read 只从最外层往下走）。
  auto describe = [&](auto&& self, const std::string& level) -> std::string {
    std::string text;
    auto found = levels.find(level);
    if (found == levels.end()) {
      return text;
    }
    for (const Child& child : found->second) {
      if (child.is_folder() && !shown.contains(child.folder)) {
        continue;
      }
      if (!text.empty()) {
        text += ", ";
      }
      if (child.is_folder()) {
        const std::string inside = self(self, child.folder);
        text += "[" + base::UTF16ToUTF8(names[child.folder]) +
                (inside.empty() ? "" : ": " + inside) + "]";
      } else if (tabs::TabInterface* tab = child.tab.Get()) {
        text += base::UTF16ToUTF8(pinned_tabs::DisplayName(tab));
      }
    }
    return text;
  };
  return describe(describe, std::string());
}

std::string Add(Profile* profile,
                const std::string& parent,
                const std::u16string& name) {
  profile->GetPrefs()->SetBoolean(prefs::kPinsCollapsed, false);
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kPinFolders);
  std::string id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::DictValue value;
  value.Set("name", name);
  if (auto* up = update->FindDict(parent)) {
    value.Set("parent", parent);
    up->Set("collapsed", false);
  }
  update->Set(id, std::move(value));
  return id;
}

void Rename(Profile* profile,
            const std::string& id,
            const std::u16string& name) {
  const auto* current =
      profile->GetPrefs()->GetDict(prefs::kPinFolders).FindDict(id);
  const auto* old_name = current ? current->FindString("name") : nullptr;
  if (!current || (old_name && base::UTF8ToUTF16(*old_name) == name)) {
    return;
  }
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kPinFolders);
  if (auto* value = update->FindDict(id)) {
    value->Set("name", name);
  }
}

void Remove(Profile* profile, const std::string& id) {
  // Children of a missing folder are displayed at the root, including tabs
  // restored from an older session after the folder was dissolved.
  ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kPinFolders);
  update->Remove(id);
}

void Create(BrowserWindowInterface* browser,
            const std::string& parent,
            tabs::TabHandle move_tab) {
  if (TabsView* tabs = InlineTabs(browser)) {
    TabsView::NewFolder fresh;
    tabs::TabInterface* tab = move_tab.Get();
    if (tab && tab->GetBrowserWindowInterface() != browser) {
      tab = nullptr;
    }
    // 从一个固定标签建的：文件夹出现在这个标签的位置上。
    const auto [level, at] =
        NewFolderSpot(browser, parent, tab ? move_tab : tabs::TabHandle());
    // 几处一起改，列表只重排一次：文件夹直接出现在该在的位置。
    tabs->BeginBatch();
    fresh.id = Add(browser->GetProfile(), level, L("tabs.folderDefaultName"));
    NoteHome(fresh.id, browser);
    Place(browser, Child{tabs::TabHandle(), fresh.id}, level, at);
    if (tab) {
      fresh.moved = move_tab;
      fresh.was_pinned = tab->IsPinned();
      fresh.was_favorite = pinned_tabs::IsFavorite(tab);
      fresh.old_folder = level;
      pinned_tabs::SetLocation(tab, false, fresh.id);
    }
    tabs->EndBatch(TabListView::Change::kFold);
    const std::string id = fresh.id;
    tabs->BeginFolderRename(id, std::move(fresh));
    return;
  }
  Folder folder;
  folder.id = parent;
  Edit(browser, std::move(folder), true, move_tab);
}

std::vector<tabs::TabInterface*> TabsIn(BrowserWindowInterface* browser,
                                        const std::string& id) {
  const Levels levels = Layout(browser);
  std::vector<tabs::TabInterface*> result;
  auto collect = [&](auto&& self, const std::string& level, int depth) -> void {
    auto found = levels.find(level);
    if (found == levels.end() || depth > kMaxDepth) {
      return;
    }
    for (const Child& child : found->second) {
      if (child.is_folder()) {
        self(self, child.folder, depth + 1);
      } else if (tabs::TabInterface* tab = child.tab.Get()) {
        result.push_back(tab);
      }
    }
  };
  collect(collect, id, 0);
  return result;
}

void OpenAll(BrowserWindowInterface* browser, const std::string& id) {
  OpenTabs(TabsIn(browser, id));
}

void OpenTabs(const std::vector<tabs::TabInterface*>& group) {
  for (tabs::TabInterface* tab : group) {
    content::WebContents* contents = tab->GetContents();
    if (!contents->WasDiscarded()) {
      continue;
    }
    // 同 Chrome 载入休眠的后台标签（TabLifecycleUnit::Load）。
    contents->GetController().SetNeedsReload();
    contents->GetController().LoadIfNecessary();
  }
}

void SleepAll(BrowserWindowInterface* browser, const std::string& id) {
  SleepTabs(browser, TabsIn(browser, id));
}

void SleepTabs(BrowserWindowInterface* browser,
               const std::vector<tabs::TabInterface*>& inside) {
  TabStripModel* model = browser->GetTabStripModel();
  tabs::TabInterface* active = browser->GetActiveTabInterface();
  if (active && std::ranges::contains(inside, active) && CanSleep(active)) {
    // 当前标签在里面：先换到外面最近用过的一个（醒着的优先），没有就开一个
    // 新标签页（同标签的右键菜单里的休眠）。
    int next = TabStripModel::kNoTab;
    bool next_awake = false;
    base::TimeTicks next_time;
    for (int i = 0; i < model->count(); ++i) {
      tabs::TabInterface* other = model->GetTabAtIndex(i);
      if (std::ranges::contains(inside, other)) {
        continue;
      }
      content::WebContents* other_contents = other->GetContents();
      const bool awake = !other_contents->WasDiscarded();
      const base::TimeTicks time = other_contents->GetLastActiveTimeTicks();
      if (next == TabStripModel::kNoTab || (awake && !next_awake) ||
          (awake == next_awake && time > next_time)) {
        next = i;
        next_awake = awake;
        next_time = time;
      }
    }
    if (next != TabStripModel::kNoTab) {
      model->ActivateTabAt(next);
    } else {
      chrome::AddTabAt(browser, GURL(), -1, /*foreground=*/true);
    }
  }
  TabListInterface* list = TabListInterface::From(browser);
  if (!list) {
    return;
  }
  std::vector<tabs::TabHandle> handles;
  for (tabs::TabInterface* tab : inside) {
    handles.push_back(tab->GetHandle());
  }
  for (const tabs::TabHandle& handle : handles) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && tab->GetBrowserWindowInterface() == browser && CanSleep(tab) &&
        !tab->IsActivated()) {
      list->DiscardTab(handle);
    }
  }
}

void OpenInSplit(BrowserWindowInterface* browser, const std::string& id) {
  SplitTabs(browser, TabsIn(browser, id));
}

void SplitTabs(BrowserWindowInterface* browser,
               const std::vector<tabs::TabInterface*>& group) {
  if (group.size() < 2 || group.size() > 3) {
    return;
  }
  std::vector<tabs::TabHandle> handles;
  for (tabs::TabInterface* tab : group) {
    handles.push_back(tab->GetHandle());
  }
  // 先从原来的分屏里拿出来，再按显示的顺序从左到右排。
  for (const tabs::TabHandle& handle : handles) {
    if (tabs::TabInterface* tab = handle.Get(); tab && tab->IsSplit()) {
      split::RemoveFromSplit(browser, tab);
    }
  }
  TabStripModel* model = browser->GetTabStripModel();
  for (size_t i = 1; i < handles.size(); ++i) {
    tabs::TabInterface* dragged = handles[i].Get();
    tabs::TabInterface* target = handles[i - 1].Get();
    if (!dragged || !target) {
      return;
    }
    const split::Drop drop =
        split::Evaluate(model, dragged, target, split::Edge::kRight);
    if (!drop.preview || !split::Apply(browser, drop)) {
      return;
    }
  }
  if (tabs::TabInterface* first = handles[0].Get()) {
    const int index = model->GetIndexOfTab(first);
    if (index != TabStripModel::kNoTab) {
      model->ActivateTabAt(index);
    }
  }
}

void Delete(BrowserWindowInterface* browser, const std::string& id) {
  const size_t count = TabsIn(browser, id).size();
  if (count == 0 || !StarryBrowserView::From(browser)) {
    DeleteNow(browser, id);
    return;
  }
  std::u16string name;
  for (const Folder& folder : Read(browser->GetProfile())) {
    if (folder.id == id) {
      name = folder.name;
    }
  }
  ConfirmDelete(
      browser, name, count, /*favorites=*/false,
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> window, std::string id) {
            if (window) {
              DeleteNow(window.get(), id);
            }
          },
          browser->GetWeakPtr(), id));
}

void ConfirmDelete(BrowserWindowInterface* browser,
                   const std::u16string& name,
                   size_t count,
                   bool favorites,
                   base::OnceClosure confirmed) {
  auto* view = StarryBrowserView::From(browser);
  if (!view) {
    return;
  }
  DialogSpec spec;
  spec.tone = DialogSpec::Tone::kDanger;
  spec.icon = &icons::kTrash;
  spec.title = L("tabs.deleteFolder.title", {{"name", name}});
  spec.message = L(favorites ? "tabs.deleteFavoriteFolder.message"
                             : "tabs.deleteFolder.message",
                   {{"count", static_cast<int>(count)}});
  spec.buttons = {{L("common.cancel"), ButtonVariant::kTertiary, true},
                  {L("tabs.deleteFolder.confirm"), ButtonVariant::kDanger}};
  view->PresentDialog(std::move(spec), base::BindOnce(
                                           [](base::OnceClosure confirmed,
                                              std::optional<size_t> choice,
                                              const std::u16string&) {
                                             if (choice == 1) {
                                               std::move(confirmed).Run();
                                             }
                                           },
                                           std::move(confirmed)));
}

Menu::Menu(BrowserWindowInterface* browser, Folder folder, bool favorites)
    : browser_(browser), folder_(std::move(folder)), favorites_(favorites) {
  auto* view = StarryBrowserView::From(browser_);
  const Theme theme = view ? view->theme() : Theme::DarkBase();
  const std::vector<tabs::TabInterface*> inside = Inside();

  model_.AddItemWithIcon(kAddCurrent, L("tabs.addCurrentToFolder"),
                         MenuIcon(icons::kPlus));
  tabs::TabInterface* active = browser_->GetActiveTabInterface();
  // 当前标签已经在里面、图块文件夹满了时不能再加。
  const bool here =
      active &&
      (favorites_ ? pinned_tabs::IsFavorite(active) &&
                        pinned_tabs::FavoriteFolder(active) == folder_.id
                  : active->IsPinned() && !pinned_tabs::IsFavorite(active) &&
                        pinned_tabs::Folder(active) == folder_.id);
  if (!active || IsBlankTabUrl(active->GetContents()->GetVisibleURL()) ||
      here ||
      (favorites_ && !pinned_tabs::HasRoomForFavorite(browser_, folder_.id))) {
    disabled_.insert(kAddCurrent);
  }
  if (!favorites_) {
    model_.AddItemWithIcon(kCreate, L("tabs.newSubfolder"),
                           MenuIcon(icons::kFolderBadgePlus));
    if (folder_.depth + 1 >= kMaxCreateDepth) {
      disabled_.insert(kCreate);
    }
  }
  model_.AddItemWithIcon(kRename, L("tabs.renameFolder"),
                         MenuIcon(icons::kPencil));

  color_model_.AddRadioItem(kColorNone, L("tabs.folderColor.none"), 0);
  color_model_.SetIcon(0, DotIcon(std::nullopt, theme));
  for (size_t i = 0; i < kColors.size(); ++i) {
    color_model_.AddRadioItem(kFirstColor + static_cast<int>(i),
                              ColorTitle(kColors[i]), 0);
    color_model_.SetIcon(i + 1,
                         DotIcon(ColorValue(kColors[i], theme.is_dark), theme));
  }
  model_.AddSubMenuWithIcon(
      kColorMenu, L("tabs.folderColor"), &color_model_,
      DotIcon(ColorValue(folder_.color, theme.is_dark), theme));

  if (!favorites_) {
    move_model_.AddItem(kRoot, L("tabs.pinnedSection"));
    std::set<std::string> descendants{folder_.id};
    for (const auto& candidate : Read(browser_->GetProfile())) {
      if (descendants.contains(candidate.parent)) {
        descendants.insert(candidate.id);
      }
      if (descendants.contains(candidate.id)) {
        continue;
      }
      move_model_.AddItem(kFirstFolder + destinations_.size(), candidate.path);
      destinations_.push_back(candidate.id);
    }
    model_.AddSubMenuWithIcon(kMove, L("tabs.moveToFolder"), &move_model_,
                              MenuIcon(icons::kFolder));
  }
  model_.AddSeparator(ui::NORMAL_SEPARATOR);

  model_.AddItemWithIcon(kOpenAll, L("tabs.openAll"), MenuIcon(icons::kSunMax));
  if (std::ranges::none_of(inside, [](tabs::TabInterface* tab) {
        return tab->GetContents()->WasDiscarded();
      })) {
    disabled_.insert(kOpenAll);
  }
  model_.AddItemWithIcon(kSleepAll, L("tabs.sleepAll"),
                         MenuIcon(icons::kMoonZzz));
  if (std::ranges::none_of(inside, &CanSleep)) {
    disabled_.insert(kSleepAll);
  }
  if (inside.size() >= 2 && inside.size() <= 3) {
    model_.AddItemWithIcon(
        kSplit, L("tabs.openInSplit"),
        MenuIcon(inside.size() == 3 ? icons::kSplit3x1 : icons::kSplit2x1));
  }
  model_.AddSeparator(ui::NORMAL_SEPARATOR);
  model_.AddItemWithIcon(kRemove, L("tabs.dissolveFolder"),
                         MenuIcon(icons::kMinusCircle));
  model_.AddItemWithIcon(kDelete, L("tabs.deleteFolder"),
                         MenuIcon(icons::kTrash));
}

Menu::~Menu() = default;

std::vector<tabs::TabInterface*> Menu::Inside() const {
  return favorites_ ? favorite_folders::TabsIn(browser_, folder_.id)
                    : TabsIn(browser_, folder_.id);
}

void Menu::ExecuteFavorite(int command) {
  const std::string& id = folder_.id;
  if (command == kRename) {
    // 卡片开着：就地改卡片上的名字，不然是对话框。
    TabsView* tabs = VisibleTabs(browser_);
    if (!tabs || !tabs->BeginCardRename(id)) {
      favorite_folders::Rename(browser_, id);
    }
  } else if (command == kAddCurrent) {
    if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
      favorite_folders::Place(browser_, Child{tab->GetHandle(), std::string()},
                              id);
    }
  } else if (command == kColorNone) {
    favorite_folders::SetColor(browser_->GetProfile(), id, std::string());
  } else if (command >= kFirstColor &&
             static_cast<size_t>(command - kFirstColor) < kColors.size()) {
    favorite_folders::SetColor(browser_->GetProfile(), id,
                               std::string(kColors[command - kFirstColor]));
  } else if (command == kOpenAll) {
    OpenTabs(Inside());
  } else if (command == kSleepAll) {
    SleepTabs(browser_, Inside());
  } else if (command == kSplit) {
    SplitTabs(browser_, Inside());
  } else if (command == kRemove) {
    favorite_folders::Dissolve(browser_, id);
  } else if (command == kDelete) {
    favorite_folders::Delete(browser_, id);
  }
}

// static
bool Menu::IsDestructive(int command) {
  return command == kDelete;
}

bool Menu::IsCommandIdEnabled(int command) const {
  return !disabled_.contains(command);
}

bool Menu::IsCommandIdChecked(int command) const {
  if (command == kColorNone) {
    return folder_.color.empty();
  }
  if (command >= kFirstColor &&
      static_cast<size_t>(command - kFirstColor) < kColors.size()) {
    return folder_.color == kColors[command - kFirstColor];
  }
  return false;
}

void Menu::ExecuteCommand(int command, int flags) {
  if (favorites_) {
    ExecuteFavorite(command);
    return;
  }
  Profile* profile = browser_->GetProfile();
  if (command == kCreate) {
    Create(browser_, folder_.id);
  } else if (command == kRename) {
    // 卡片开着（顶部的胶囊）：就地改卡片上的名字；侧栏里改那一行。
    TabsView* visible = VisibleTabs(browser_);
    TabsView* tabs = InlineTabs(browser_);
    if (!(visible && visible->BeginCardRename(folder_.id)) &&
        (!tabs || !tabs->BeginFolderRename(folder_.id))) {
      Edit(browser_, folder_, false, {});
    }
  } else if (command == kAddCurrent) {
    if (tabs::TabInterface* tab = browser_->GetActiveTabInterface()) {
      pinned_tabs::SetLocation(tab, false, folder_.id);
    }
  } else if (command == kColorNone) {
    SetColor(profile, folder_.id, std::string());
  } else if (command >= kFirstColor &&
             static_cast<size_t>(command - kFirstColor) < kColors.size()) {
    SetColor(profile, folder_.id, std::string(kColors[command - kFirstColor]));
  } else if (command == kOpenAll) {
    OpenAll(browser_, folder_.id);
  } else if (command == kSleepAll) {
    SleepAll(browser_, folder_.id);
  } else if (command == kSplit) {
    OpenInSplit(browser_, folder_.id);
  } else if (command == kDelete) {
    Delete(browser_, folder_.id);
  } else if (command == kRoot || (command >= kFirstFolder &&
                                  static_cast<size_t>(command - kFirstFolder) <
                                      destinations_.size())) {
    const std::string parent = command == kRoot
                                   ? std::string()
                                   : destinations_[command - kFirstFolder];
    // 菜单开着时，要去的文件夹可能在别的窗口里被删了。
    if (!parent.empty() &&
        !profile->GetPrefs()->GetDict(prefs::kPinFolders).FindDict(parent)) {
      return;
    }
    // 放在那一层的最后（Place 不会把文件夹放进自己里面）。
    TabsView* tabs = VisibleTabs(browser_);
    if (tabs) {
      tabs->BeginBatch();
    }
    Place(browser_, Child{tabs::TabHandle(), folder_.id}, parent);
    if (tabs) {
      tabs->EndBatch(TabListView::Change::kFold);
    }
  } else if (command == kRemove) {
    TabsView* tabs = VisibleTabs(browser_);
    if (tabs) {
      tabs->BeginBatch();
    }
    Dissolve(browser_, folder_.id);
    if (tabs) {
      tabs->EndBatch(TabListView::Change::kFold);
    }
  }
}
}  // namespace starry::pin_folders
