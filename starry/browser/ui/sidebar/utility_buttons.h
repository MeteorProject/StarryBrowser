// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_SIDEBAR_UTILITY_BUTTONS_H_
#define STARRY_BROWSER_UI_SIDEBAR_UTILITY_BUTTONS_H_

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/frame/chrome_layout.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

class BrowserWindowInterface;

namespace starry {

class DownloadsButton;
class IconButton;
enum class IconBadge;
class PrivateBadge;

// 侧栏底部的工具按钮（枚举顺序即显示顺序）。
enum class UtilityButton {
  kDownloads,
  kHistory,
  kBookmarks,
  kExtensions,
  kAssistant,
  kLayout,
  kSettings,
};
inline constexpr size_t kUtilityButtonCount = 7;

// 存在设置里的名字（prefs::kHiddenUtilityButtons）。
const char* UtilityButtonKey(UtilityButton button);

// 工具按钮组。侧栏布局下位于侧栏底部：下载、历史记录、书签、扩展靠左，布局和设置靠右，
// 无痕窗口在左侧组后显示无痕标记。顶部标签布局下位于工具栏右端排成一行（无痕标记在
// 最前），紧挨扩展菜单时不再重复放置「管理扩展」。AI 助手按钮在有横向顶栏时位于右上角
// （AssistantButton），不在此组中。
// - 每个按钮可在右键菜单（「显示的按钮」，勾选时不关闭菜单）中隐藏，配置持久化并同步到
//   所有窗口；出现、消失时缩放淡入淡出，其余按钮平滑补位。
// - 下载按钮在有进行中的下载时强制显示，外圈绘制同类（普通或无痕）下载的总进度，完成
//   时箭头向下回弹。
// - 无痕窗口不显示扩展按钮。
// - 布局按钮打开布局菜单：固定侧栏、浮动侧栏、顶部标签，侧栏布局下还可配置地址栏与
//   扩展按钮置顶、紧凑地址栏及外观设置入口。
// - 当前打开对应内置页面时，按钮常亮。
class UtilityButtons : public views::View,
                       public views::ContextMenuController,
                       public views::ViewTargeterDelegate,
                       public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(UtilityButtons, views::View)

 public:
  UtilityButtons(BrowserWindowInterface* browser, ChromePlacement placement);
  UtilityButtons(const UtilityButtons&) = delete;
  UtilityButtons& operator=(const UtilityButtons&) = delete;
  ~UtilityButtons() override;

  void SetTheme(const Theme& theme);
  // 窗口的布局（AI
  // 助手的按钮在不在这一组、布局按钮的图标）和窗口里有没有带按钮的扩展（
  // 工具栏里紧挨扩展菜单时不放「管理扩展」）。
  void SetChromeLayout(const ChromeLayout& layout, bool has_extensions);
  ChromePlacement placement() const { return placement_; }
  // 工具栏里这一组占的宽度。
  int PreferredWidth() const;
  // 在 point（浏览器视图的坐标）打开「显示的按钮」菜单（右上角的 AI
  // 助手按钮也用它）。
  void ShowButtonsMenuAt(const gfx::Point& point);
  // 当前标签打开的是哪个整页界面。
  void SetActivePage(InternalPage page);
  // 这一行里有东西可显示（按钮或无痕标记）。全部藏起来时侧栏底部这一行整个收起。
  bool HasContent() const;
  // 显示的内容变了（HasContent 可能变了）：侧栏重新排版。
  void SetContentChangedCallback(base::RepeatingClosure callback);

  // 下载弹出框开着：下载按钮常亮，用户藏起来的也先显示出来（弹出框挂在它上面）。
  void SetDownloadsPanelOpen(bool open);
  // AI 助手开着：✦ 按钮常亮。
  void SetAssistantOpen(bool open);
  // 助手关着、还在做事或者等用户时 ✦ 右上角的小点。
  void SetAssistantBadge(IconBadge badge);
  // 下载按钮（本视图的坐标）；没显示时是空的。
  gfx::Rect DownloadsButtonBounds() const;
  void BounceDownloadsButton();

  // 调试用：显示着的按钮（UtilityButtonKey）。
  std::vector<std::string> VisibleButtonsForTesting() const;
  // 调试用：打开布局菜单、工具按钮的右键菜单。
  void OpenLayoutMenuForTesting();
  void OpenButtonsMenuForTesting();
  double DownloadProgressForTesting() const;

  // views::View:
  void Layout(PassKey) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override;

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdChecked(int command_id) const override;
  bool IsCommandIdEnabled(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  struct Slot {
    UtilityButton kind;
    raw_ptr<IconButton> button = nullptr;
    bool shown = false;
    LayerMotion motion;
  };

  // 按设置和下载状态决定显示哪些；animated 时出现、消失、挪动都有动画。
  void Update(bool animated);
  bool ShouldShow(UtilityButton button) const;
  // 把显示着的按钮排到位置上；animated 时用变换从原来的位置滑过去。
  void Arrange(bool animated);
  void ShowButtonsMenu(const gfx::Point& point);
  void ShowLayoutMenu();
  void RunCommand(int command);
  void ToggleButton(UtilityButton button);

  const raw_ptr<BrowserWindowInterface> browser_;
  const ChromePlacement placement_;
  const bool incognito_;
  ChromeLayout layout_;
  bool has_extensions_ = false;
  Theme theme_ = Theme::DarkBase();
  std::array<std::unique_ptr<Slot>, kUtilityButtonCount> slots_;
  raw_ptr<DownloadsButton> downloads_ = nullptr;
  raw_ptr<PrivateBadge> private_badge_ = nullptr;
  InternalPage active_page_ = InternalPage::kNone;
  bool had_content_ = true;
  bool downloads_panel_open_ = false;
  bool assistant_open_ = false;
  base::RepeatingClosure content_changed_;
  PrefChangeRegistrar prefs_;
  std::unique_ptr<ui::SimpleMenuModel> menu_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_SIDEBAR_UTILITY_BUTTONS_H_
