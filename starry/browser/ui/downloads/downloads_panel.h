// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DOWNLOADS_DOWNLOADS_PANEL_H_
#define STARRY_BROWSER_UI_DOWNLOADS_DOWNLOADS_PANEL_H_

#include <memory>
#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/events/event_observer.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

class BrowserWindowInterface;

namespace views {
class EventMonitor;
}

namespace starry {

class Download;
class DownloadsPanelCard;

// 下载按钮弹出的小列表：最近 30 个下载（DownloadService 的列表），进行中的带圆形进度、
// 细进度条（分段下载时按段画）和暂停、取消；结束的悬停时可以在访达中显示，双击打开，可以拖
// 出去。顶上是进行中的数量和总速度、打开下载文件夹、清除已结束的；底下是「显示全部下载」
// （下载页）。只列这个窗口这一类（普通或无痕）的下载。
//
// 和菜单一样是盖在整个窗口上的一层（StarryBrowserView 的子视图），只有卡片接收鼠标；在外面
// 按下鼠标、按 Esc 时关掉，那一下照常给下面的东西（同 macOS 的 transient 弹出框）。卡片是
// 实时的 Liquid Glass，从按钮那个角弹出来。
class DownloadsPanelHost : public views::View,
                           public views::ViewTargeterDelegate,
                           public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(DownloadsPanelHost, views::View)

 public:
  explicit DownloadsPanelHost(BrowserWindowInterface* browser);
  DownloadsPanelHost(const DownloadsPanelHost&) = delete;
  DownloadsPanelHost& operator=(const DownloadsPanelHost&) = delete;
  ~DownloadsPanelHost() override;

  // anchor 是下载按钮（本视图的坐标）：卡片在按钮上面、左边对齐，放不下时贴着窗口边。
  void Show(const gfx::Rect& anchor);
  void Close();
  bool is_showing() const { return card_ != nullptr && !closing_; }
  void SetTheme(const Theme& theme);
  // 打开、关上时调用（下载按钮常亮、侧栏不收起）。
  base::CallbackListSubscription AddShowingChangedCallback(
      base::RepeatingClosure callback);

  // 一行的右键菜单（菜单在窗口的菜单层，模型归这里，行删掉了也不悬空）。
  void ShowItemMenu(Download* item, const gfx::Point& point);
  BrowserWindowInterface* browser() { return browser_; }
  const Theme& theme() const { return theme_; }

  // 调试用：卡片里各行的文件名和状态。
  std::vector<std::u16string> RowTextsForTesting() const;
  gfx::Rect CardBoundsForTesting() const;

  // views::View:
  void Layout(PassKey) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // 窗口里任何地方按下鼠标、按键（views::EventMonitor 转过来的）。
  void OnWindowEvent(const ui::Event& event);

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdEnabled(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  void PlaceCard();
  void RemoveCard();
  Download* MenuItem() const;

  const raw_ptr<BrowserWindowInterface> browser_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<DownloadsPanelCard> card_ = nullptr;
  bool closing_ = false;
  gfx::Rect anchor_;
  class WindowWatcher;
  std::unique_ptr<WindowWatcher> watcher_;
  std::unique_ptr<views::EventMonitor> event_monitor_;
  base::RepeatingClosureList showing_changed_;
  std::unique_ptr<ui::SimpleMenuModel> menu_model_;
  std::string menu_guid_;

  base::WeakPtrFactory<DownloadsPanelHost> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DOWNLOADS_DOWNLOADS_PANEL_H_
