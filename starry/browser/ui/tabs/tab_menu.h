// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_TAB_MENU_H_
#define STARRY_BROWSER_UI_TABS_TAB_MENU_H_

#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/menus/simple_menu_model.h"

class BrowserWindowInterface;

namespace starry {

// 标签右键菜单：
//   重新载入、休眠/唤醒、拷贝链接、复制标签页、固定/取消固定、静音/取消静音（有声音或
//   静音着时）；在分屏里时移出分屏、解散分屏，不在时「与当前标签页分屏」（放在
//   右边、左边、下面、上面）；关闭其他标签页、关闭标签页。
//   侧栏树中的普通标签另含树操作项（tab_tree.h）：在下方新建标签页、折叠/展开这一组、
//   折叠其他组、提升一级、作为上一个标签页的子标签页；有子标签时关闭这一组、关闭
//   子标签页、把这一组移到新窗口。不可用项不显示。
// 所有操作均直接作用于 Chrome 的 TabStripModel。
class TabMenu : public ui::SimpleMenuModel::Delegate {
 public:
  // 命令 ID（只在这个菜单里用）。
  enum Command {
    kReload = 1,
    kSleep,
    kWake,
    kCopyLink,
    kDuplicate,
    kPin,
    kUnpin,
    kMute,
    kUnmute,
    kCloseOthers,
    kClose,
    kSplitRemove,
    kSplitDissolve,
    kSplitRight,
    kSplitLeft,
    kSplitBottom,
    kSplitTop,
    kFavorite,
    kMoveToPins,
    kReset,
    kReplace,
    kEdit,
    kRename,
    kArchive,
    kOrganize,
    kSuggestAccept,
    kSuggestDismiss,
    kSuggestRule,
    kFolderMenu,
    kNewFolder,
    kFavoriteFolderMenu,
    kNewFavoriteFolder,
    kFavoriteRoot,
    kTreeNewBelow,
    kTreeCollapse,
    kTreeExpand,
    kTreeCollapseOthers,
    kTreeHoist,
    kTreeOutdent,
    kTreeIndent,
    kTreeCloseGroup,
    kTreeCloseChildren,
    kTreeMoveToNewWindow,
    kFirstFolder = 1000,
    kFirstFavoriteFolder = 2000,
  };

  TabMenu(BrowserWindowInterface* browser, tabs::TabInterface* tab);
  TabMenu(const TabMenu&) = delete;
  TabMenu& operator=(const TabMenu&) = delete;
  ~TabMenu() override;

  ui::SimpleMenuModel* model() { return &model_; }

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdEnabled(int command_id) const override;
  bool IsCommandIdChecked(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  void Build(tabs::TabInterface* tab);
  void Sleep(tabs::TabInterface* tab);
  void CloseOthers(tabs::TabInterface* tab);
  // 无法休眠时向用户说明原因。
  void ShowSleepFailed(const std::u16string& message);

  const raw_ptr<BrowserWindowInterface> browser_;
  const tabs::TabHandle tab_;
  ui::SimpleMenuModel model_{this};
  // 「与当前标签页分屏」的子菜单。
  ui::SimpleMenuModel split_model_{this};
  ui::SimpleMenuModel folder_model_{this};
  std::vector<std::string> folders_;
  // 「移到常用文件夹」的子菜单：常用图块里的文件夹。
  ui::SimpleMenuModel favorite_model_{this};
  std::vector<std::string> favorite_folders_;
  std::set<int> disabled_;
  // 固定标签在文件夹里：「这个网站的新标签提示放进这里」的网站、文件夹、现在有没有这条规则。
  std::string rule_site_;
  std::string rule_folder_;
  bool rule_on_ = false;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_TAB_MENU_H_
