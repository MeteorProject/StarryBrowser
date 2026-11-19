// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_FAVORITE_FOLDERS_H_
#define STARRY_BROWSER_UI_TABS_FAVORITE_FOLDERS_H_

#include <optional>
#include <string>
#include <vector>

#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/tabs/pin_folders.h"

class BrowserWindowInterface;
class Profile;

// 常用文件夹只有一层。文件夹元数据由同一用户资料的窗口共享，各窗口分别持有标签。
namespace starry::favorite_folders {

// 按排序值（和图块那一层的常用标签用同一套排序值）。只用 id、name、order、
// color。
std::vector<pin_folders::Folder> Read(Profile* profile);
std::optional<pin_folders::Folder> Find(Profile* profile,
                                        const std::string& id);

// 这个窗口的常用图块：键是空的是图块那一层（常用标签和文件夹按排序值混排），
// 别的是每个文件夹里的常用标签。每个文件夹都有一项（空的也有）。
pin_folders::Levels Layout(BrowserWindowInterface* browser);
// 文件夹里这个窗口的常用标签，按显示的顺序。
std::vector<tabs::TabInterface*> TabsIn(BrowserWindowInterface* browser,
                                        const std::string& id);

// 把一项放进 folder 这一层（空的是图块那一层）、排在 before 之前（空的是放在
// 最后），这一层重新编排序值。不是常用的标签先变成常用的；文件夹只能在图块
// 那一层。那一层满了、空白标签固定不了时不动，返回 false。
bool Place(BrowserWindowInterface* browser,
           const pin_folders::Child& item,
           const std::string& folder,
           const pin_folders::Child& before = {});

// 能不能新建一个：图块那一层还有空，或者 tab 就在图块那一层（它进了新的文件
// 夹，腾出一格）。
bool CanCreate(BrowserWindowInterface* browser, tabs::TabInterface* tab);
// 新建：对话框里起名。tab 不是空的时把它放进去（它在图块那一层时，文件夹出现
// 在它的位置上）。
void Create(BrowserWindowInterface* browser, tabs::TabHandle tab = {});
// 同上，不问名字（调试用）。返回新文件夹的 ID，建不了是空的。
std::string CreateNamed(BrowserWindowInterface* browser,
                        const std::u16string& name,
                        tabs::TabHandle tab = {});
void Rename(BrowserWindowInterface* browser, const std::string& id);
// 改名（不问）：名字是空的、没变时不动。
void SetName(Profile* profile,
             const std::string& id,
             const std::u16string& name);
// 标签 dragged 拖到图块那一层的常用标签 onto 上：在 onto 的位置建一个文件夹
// （默认的名字），两个都放进去（onto 在前）。返回新文件夹的 ID，建不了是空的。
std::string Merge(BrowserWindowInterface* browser,
                  tabs::TabHandle onto,
                  tabs::TabHandle dragged);
// color 是空的：去掉颜色。
void SetColor(Profile* profile,
              const std::string& id,
              const std::string& color);
// 解散：里面的常用标签留在图块那一层、文件夹原来的位置上。
void Dissolve(BrowserWindowInterface* browser, const std::string& id);
// 删除文件夹和里面的常用标签（所有窗口里都没了，这个窗口里的进「最近关闭」，
// 能恢复）。里面有标签时先问一下。
void Delete(BrowserWindowInterface* browser, const std::string& id);
// 调试用：常用图块的样子，比如「A, [Work: B, C], D」。
std::string DescribeForTesting(BrowserWindowInterface* browser);

}  // namespace starry::favorite_folders

#endif  // STARRY_BROWSER_UI_TABS_FAVORITE_FOLDERS_H_
