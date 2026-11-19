// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SIDEBAR_EXTENSION_BAR_H_
#define STARRY_BROWSER_UI_SIDEBAR_EXTENSION_BAR_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/toolbar/toolbar_action_view_model.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/frame/chrome_layout.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/view.h"

class BrowserWindowInterface;
class ExtensionActionViewModel;

namespace starry {

class ExtensionActionButton;
class IconButton;

// 侧栏扩展栏：位于地址栏下方一行，已固定的扩展按钮靠左排列，最右侧为扩展菜单（列出
// 所有带按钮的扩展，支持打开或固定到侧栏）。没有带按钮的扩展时不占空间。数据来自
// Chrome 的 ToolbarActionsModel（「固定」对应固定到工具栏），每个扩展由一个
// ExtensionActionViewModel 驱动（图标、徽标、点击动作、右键菜单）。
//
// 点击行为：扩展带弹出页时在其右侧（未固定则挂在扩展菜单按钮上）弹出 Chrome 扩展
// 弹出框；否则派发 action.onClicked。右键菜单沿用 Chrome 扩展菜单并通过 Starry 菜单
// 展示。无痕窗口仅显示允许在无痕模式下运行的扩展。
//
// 扩展快捷键（_execute_action）和 chrome.action.openPopup() 打开的弹出框仍挂在隐藏
// 的原生工具栏上。
//
// 移到顶部时（kToolbar）位于工具栏右侧：已固定按钮后紧跟扩展菜单，弹出框向下展开。
class ExtensionBar : public views::View,
                     public ToolbarActionsModel::Observer,
                     public TabStripModelObserver,
                     public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(ExtensionBar, views::View)

 public:
  static constexpr int kHeight = 28;

  ExtensionBar(BrowserWindowInterface* browser,
               ChromePlacement placement = ChromePlacement::kSidebar);
  ExtensionBar(const ExtensionBar&) = delete;
  ExtensionBar& operator=(const ExtensionBar&) = delete;
  ~ExtensionBar() override;

  bool HasContent() const { return !entries_.empty(); }
  ChromePlacement placement() const { return placement_; }
  // 工具栏里占的宽度（固定的按钮和扩展菜单）。
  int PreferredWidth() const;
  bool IsPopupShowing() const { return !popup_id_.empty(); }
  void SetTheme(const Theme& theme);
  // 有没有这一行变了（侧栏重新排版）。
  void SetContentChangedCallback(base::RepeatingClosure callback);

  // ExtensionActionDelegate（extension_bar.cc 里的 BarActionDelegate）用的。
  views::View* PopupAnchorFor(const std::string& id);
  void HideActivePopup();
  void SetPopupOwner(const std::string& id);
  void ShowContextMenuFor(const std::string& id);
  void CloseMenu();

  // 调试步骤：「pinned=a,b all=a,b,c popup=id」。
  std::string DescribeForTesting() const;
  // 点第 index 个扩展（按扩展菜单里的顺序）；固定的点按钮，没固定的从扩展菜单点。
  bool ActivateForTesting(size_t index);
  void ShowExtensionsMenuForTesting() { ShowExtensionsMenu(); }

  // views::View:
  void Layout(PassKey) override;

  // ToolbarActionsModel::Observer:
  void OnToolbarActionAdded(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarActionRemoved(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarActionUpdated(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarModelInitialized() override;
  void OnToolbarPinnedActionsChanged() override;
  void OnToolbarActionsModelShutdown() override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

  // ui::SimpleMenuModel::Delegate（扩展菜单）:
  bool IsCommandIdChecked(int command_id) const override;
  bool IsCommandIdEnabled(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  struct Entry;

  // 和 ToolbarActionsModel 对上：加上新的、去掉没有的，按固定的顺序排按钮。
  void Sync(bool animated);
  void RefreshButtons();
  // 按名字排的所有扩展（扩展菜单的顺序）。
  std::vector<Entry*> SortedEntries() const;
  Entry* Find(const std::string& id) const;
  void Activate(const std::string& id,
                ToolbarActionViewModel::InvocationSource source);
  void ShowExtensionsMenu();
  void ShowMenu(ui::MenuModel* model,
                views::View* anchor,
                MenuOptions options);
  void UpdateActiveStates();

  const raw_ptr<BrowserWindowInterface> browser_;
  const ChromePlacement placement_;
  raw_ptr<ToolbarActionsModel> model_ = nullptr;
  std::map<std::string, std::unique_ptr<Entry>> entries_;
  // 固定的按钮，从左到右。
  std::vector<raw_ptr<ExtensionActionButton>> pinned_;
  raw_ptr<IconButton> menu_button_ = nullptr;
  Theme theme_ = Theme::DarkBase();
  base::RepeatingClosure content_changed_;
  // 扩展菜单：所有扩展、「固定到侧栏」子菜单、管理扩展。
  std::unique_ptr<ui::SimpleMenuModel> menu_model_;
  std::unique_ptr<ui::SimpleMenuModel> pin_menu_model_;
  std::vector<std::string> menu_ids_;
  bool menu_showing_ = false;
  // 弹出框开着的扩展（按钮常亮）。
  std::string popup_id_;
  base::ScopedObservation<ToolbarActionsModel, ToolbarActionsModel::Observer>
      model_observation_{this};
  base::WeakPtrFactory<ExtensionBar> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SIDEBAR_EXTENSION_BAR_H_
