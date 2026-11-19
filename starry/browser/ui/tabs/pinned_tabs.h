// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_PINNED_TABS_H_
#define STARRY_BROWSER_UI_TABS_PINNED_TABS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "components/tabs/public/tab_interface.h"

class BrowserWindowInterface;
class GURL;
class Profile;

// Favorites and pinned rows both remember a starting URL. Closing runs normal
// beforeunload/unload handlers and then replaces the page with a sleeping tab.
// Archiving removes the entry through Chromium's historical-tab close path.
// Session extra_data preserves the saved page, display name and location.
namespace starry::pinned_tabs {

// 常用图块那一层、每个图块文件夹里最多放几个（图块文件夹在图块那一层占一格）。
inline constexpr size_t kMaxFavoriteTiles = 12;

// 标签固定、取消固定以后调用（TabsView 的 OnTabPinnedStateChanged）：固定了就记下现在的
// 网址和标题、写进会话；取消固定就忘掉。
void PinnedStateChanged(tabs::TabInterface* tab);

// 固定时的网址。没记下过的（以前的会话恢复的、扩展直接开成固定的）用现在的网址补上。
// 不是固定标签时是空的。
GURL PinnedUrl(tabs::TabInterface* tab);

bool IsFavorite(tabs::TabInterface* tab);
std::string Folder(tabs::TabInterface* tab);
// 常用标签在哪个图块文件夹里（favorite_folders.h）；在图块那一层的、不是常用
// 的是空的。
std::string FavoriteFolder(tabs::TabInterface* tab);
void InitializeProfile(Profile* profile);
void TabClosed(content::WebContents* contents);
void CopyStateIfMissing(content::WebContents* old_contents,
                        content::WebContents* replacement);
int FavoriteOrder(tabs::TabInterface* tab);
// 常用的 folder 这一层（空的是图块那一层）还能不能再放一个。
bool HasRoomForFavorite(BrowserWindowInterface* browser,
                        const std::string& folder = std::string());
// 放进常用的 folder 这一层的最后（不是常用的先固定、变成常用的）。那一层满了、
// 文件夹没了、空白标签固定不了时返回 false。
bool SetFavoriteFolder(tabs::TabInterface* tab, const std::string& folder);
// 图块文件夹要没了（解散、删除）：里面的常用标签回到图块那一层。
void ClearFavoriteFolder(BrowserWindowInterface* browser,
                         const std::string& folder);
// 常用标签的排序值（和同一层的图块文件夹比），所有窗口共用。
void SetFavoriteOrders(
    BrowserWindowInterface* browser,
    const std::vector<std::pair<tabs::TabHandle, int>>& orders);
// 排在所有常用标签、图块文件夹后面的排序值。
int NextFavoriteOrder(Profile* profile);
void SyncFavorites(BrowserWindowInterface* browser);
// 关掉固定标签、不再留着（进「最近关闭」）。to_tab_archive：也放进标签管理的归档
// （右键「归档标签页」）；点 ✕ 关掉休眠的、删文件夹时不放。
void Archive(tabs::TabInterface* tab, bool to_tab_archive);
// 换了地方（进了别的文件夹、从图块变成行）的标签排到那一层的最后。已经是常用
// 的标签再设成常用的，留在原来那一层。
void SetLocation(tabs::TabInterface* tab, bool favorite, std::string folder);
// 固定标签（不算常用的图块）在它那一层里的排序值，和同一层的文件夹的排序值比
// （pin_folders::Layout）。没排过的是空的，排在最后。跟着标签写进会话。
std::optional<double> Order(tabs::TabInterface* tab);
void SetOrder(tabs::TabInterface* tab, std::optional<double> order);
std::u16string DisplayName(tabs::TabInterface* tab);
void Rename(tabs::TabInterface* tab);
void Reset(tabs::TabInterface* tab, bool keep_current);
void SetPinnedURL(tabs::TabInterface* tab, const GURL& url);

// 关掉固定标签（见上）。不是固定标签、没有可以回去的网址时返回 false，照常关。
bool Close(BrowserWindowInterface* browser, tabs::TabInterface* tab);

}  // namespace starry::pinned_tabs

#endif  // STARRY_BROWSER_UI_TABS_PINNED_TABS_H_
