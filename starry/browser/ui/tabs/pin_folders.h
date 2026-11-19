// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_PIN_FOLDERS_H_
#define STARRY_BROWSER_UI_TABS_PIN_FOLDERS_H_

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback_forward.h"
#include "components/tabs/public/tab_interface.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/menus/simple_menu_model.h"

class BrowserWindowInterface;
class Profile;

namespace starry {
class TabsView;
}

namespace starry::pin_folders {

struct Folder {
  std::string id;
  std::string parent;
  std::u16string name;
  std::u16string path;
  int depth = 0;
  bool collapsed = false;
  bool visible = true;
  // 在上一层里排第几（和那一层的固定标签的排序值比）；没排过的是空的。
  std::optional<double> order;
  // 图标的颜色（kColors 里的一个），没选是空的。
  std::string color;
};

// 所有文件夹，上一层在前、里面的紧跟在后面。同一层里排过的在前（按排序值），没排
// 过的按名字。
std::vector<Folder> Read(Profile* profile);
void Toggle(Profile* profile, const std::string& id);
void SetCollapsed(Profile* profile, const std::string& id, bool collapsed);
// 连同里面所有的文件夹一起展开、折叠（按住 ⌥ 点文件夹）。
void SetTreeCollapsed(Profile* profile, const std::string& id, bool collapsed);

// 文件夹能选的颜色（菜单里的顺序）：只染图标。
inline constexpr std::array<std::string_view, 6> kColors = {
    "red", "orange", "yellow", "green", "blue", "purple"};
// 颜色在深色、浅色界面上的值（同系统的强调色）；不认识的是空的。
std::optional<SkColor> ColorValue(std::string_view color, bool dark);
// color 是空的：去掉颜色。
void SetColor(Profile* profile,
              const std::string& id,
              const std::string& color);

// 新建文件夹最多到第几层（最外层是第 1 层）。导入的、以前建的更深的照常显示。
inline constexpr int kMaxCreateDepth = 3;

// 固定区某一层里的一项：一个固定标签，或者一个文件夹。
struct Child {
  tabs::TabHandle tab;
  std::string folder;

  bool is_folder() const { return !folder.empty(); }
  bool empty() const { return folder.empty() && tab == tabs::TabHandle(); }
  bool operator==(const Child&) const = default;
};

// 这个窗口的固定区里每一层有什么（键是那一层的文件夹，最外层是空的），按显示的
// 顺序：标签和文件夹混排，排过的按排序值，没排过的排在后面（先是标签，按在
// TabStripModel 里的先后；再是文件夹，按名字）。
// 文件夹存在设置里、所有窗口共用，固定标签是各个窗口自己的，所以要按窗口算。
// 这里有所有的文件夹，包括这个窗口里不显示的（ShownIn）。
using Levels = std::map<std::string, std::vector<Child>>;
Levels Layout(BrowserWindowInterface* browser);

// 这个窗口里显示哪些文件夹。文件夹所有窗口共用，标签是各个窗口自己的，所以别的
// 窗口里不显示空的文件夹，只显示：
// - 里面（含子文件夹）有这个窗口的固定标签的；
// - 归这个窗口的：在这里新建的，或者在这里有过标签、后来空了的（最先在哪个开着
//   的窗口里有标签就归哪个，那个窗口关了再换）；
// - 不归任何开着的窗口、哪个窗口里也没有标签的（比如重新启动以后的空文件夹）。
// 显示的文件夹的上一层也显示。别的在「移到文件夹」的菜单里照常能选。
std::set<std::string> ShownIn(BrowserWindowInterface* browser);
// 窗口的标签列表建好了、要拿掉了（TabsView）：窗口没了时别的窗口重新算。
void WindowShown(BrowserWindowInterface* browser);
void WindowGone(BrowserWindowInterface* browser);

// 把 item 放进 parent 这一层、排在 before 之前（空的 before 是放在最后），
// 然后给这一层重新编排序值。普通标签会先固定；文件夹不能放进自己或者自己里面的
// 文件夹。
void Place(BrowserWindowInterface* browser,
           const Child& item,
           const std::string& parent,
           const Child& before = {});
// 解散文件夹：里面的东西留在它原来的位置上。
void Dissolve(BrowserWindowInterface* browser, const std::string& id);
// 调试用：这个窗口的固定区的样子（不算不显示的文件夹），比如
// 「A, [Work: B, [Sub: C]], D」。
std::string DescribeForTesting(BrowserWindowInterface* browser);
// 新建一个文件夹（上一层、整个固定区折叠着就展开），返回它的 ID。
std::string Add(Profile* profile,
                const std::string& parent,
                const std::u16string& name);
void Rename(Profile* profile,
            const std::string& id,
            const std::u16string& name);
// 移除文件夹，里面的标签留下（显示在最外层）。
void Remove(Profile* profile, const std::string& id);
// 用户新建文件夹：侧栏里直接多出一行、就地起名（Esc 取消）；顶部标签布局下还是
// 对话框。move_tab 是要放进去的标签：它是固定标签时，文件夹出现在它的位置上
// （和它同一层），不然放在 parent 的最后。
void Create(BrowserWindowInterface* browser,
            const std::string& parent,
            tabs::TabHandle move_tab = {});

// 文件夹（含子文件夹）里这个窗口的标签，按显示的顺序。
std::vector<tabs::TabInterface*> TabsIn(BrowserWindowInterface* browser,
                                        const std::string& id);
// 全部打开：休眠着的（包括关掉后留着的固定标签）都载入，不切过去。
void OpenAll(BrowserWindowInterface* browser, const std::string& id);
// 全部休眠：当前标签在里面时先换到外面最近用过的一个；放着声音、开着摄像头的
// 不休眠。
void SleepAll(BrowserWindowInterface* browser, const std::string& id);
void OpenInSplit(BrowserWindowInterface* browser, const std::string& id);
// 删除文件夹（连同里面的文件夹）和这个窗口里在里面的标签（进「最近关闭」，能
// 恢复）。有标签时先问一下。别的窗口里在里面的标签留下，显示在最外层。
void Delete(BrowserWindowInterface* browser, const std::string& id);

// 显示着的标签（侧栏的，或者顶部标签行的）；没有 Starry 的界面时是空的。几处
// 一起改时用它先不刷新（BeginBatch）。
TabsView* VisibleTabs(BrowserWindowInterface* browser);
// 能不能休眠（同标签的右键菜单：空白页、浏览器自己的页面、放着声音、开着
// 摄像头的不行）。
bool CanSleep(tabs::TabInterface* tab);
// 一组标签（一个文件夹里的）：全部打开、全部休眠、一起分屏（2–3 个时）。
void OpenTabs(const std::vector<tabs::TabInterface*>& group);
void SleepTabs(BrowserWindowInterface* browser,
               const std::vector<tabs::TabInterface*>& group);
void SplitTabs(BrowserWindowInterface* browser,
               const std::vector<tabs::TabInterface*>& group);
// 删除文件夹（里面有 count 个标签）前问一下，点了删除才调用 confirmed。
// favorites：常用图块的文件夹（所有窗口里都会删掉）。
void ConfirmDelete(BrowserWindowInterface* browser,
                   const std::u16string& name,
                   size_t count,
                   bool favorites,
                   base::OnceClosure confirmed);

// 文件夹的菜单（右键、「更多」）。favorites：常用图块的文件夹
// （favorite_folders.h），没有新建子文件夹、移到文件夹。
class Menu : public ui::SimpleMenuModel::Delegate {
 public:
  Menu(BrowserWindowInterface* browser, Folder folder, bool favorites = false);
  ~Menu() override;
  bool IsCommandIdEnabled(int command) const override;
  bool IsCommandIdChecked(int command) const override;
  void ExecuteCommand(int command, int flags) override;
  ui::SimpleMenuModel* model() { return &model_; }
  // 危险的操作（删除），菜单里用危险色。
  static bool IsDestructive(int command);

 private:
  // 里面的标签（含子文件夹），按显示的顺序。
  std::vector<tabs::TabInterface*> Inside() const;
  void ExecuteFavorite(int command);

  raw_ptr<BrowserWindowInterface> browser_;
  Folder folder_;
  bool favorites_ = false;
  std::vector<std::string> destinations_;
  std::set<int> disabled_;
  ui::SimpleMenuModel move_model_{this};
  ui::SimpleMenuModel color_model_{this};
  ui::SimpleMenuModel model_{this};
};
}  // namespace starry::pin_folders
#endif  // STARRY_BROWSER_UI_TABS_PIN_FOLDERS_H_
