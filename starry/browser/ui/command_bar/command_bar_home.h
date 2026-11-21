// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_COMMAND_BAR_COMMAND_BAR_HOME_H_
#define STARRY_BROWSER_UI_COMMAND_BAR_COMMAND_BAR_HOME_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "starry/browser/new_tab/new_tab_data.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace starry {

// 全屏浮层风格（new_tab.h）的首页：命令栏输入框为空时展示常去网站、最近关闭与底部状态行。
// 铺满命令栏，仅交互项接收鼠标，空白区域点击透传给命令栏关闭。
class CommandBarHome : public views::View,
                       public views::ViewTargeterDelegate,
                       public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(CommandBarHome, views::View)

 public:
  class Delegate {
   public:
    virtual void HomeOpenUrl(const GURL& url) = 0;
    virtual void HomeDismiss() = 0;

   protected:
    virtual ~Delegate() = default;
  };

  CommandBarHome(BrowserWindowInterface* browser, Delegate* delegate);
  CommandBarHome(const CommandBarHome&) = delete;
  CommandBarHome& operator=(const CommandBarHome&) = delete;
  ~CommandBarHome() override;

  void SetTheme(const Theme& theme);
  void Refresh(bool reveal);
  void SetPanelBottom(int bottom);
  void SetShown(bool shown);

  // views::View:
  void Layout(PassKey) override;
  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;
  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  class Tile;
  class Card;
  class Footer;
  class SectionTitle;
  friend class Tile;
  friend class Card;
  friend class Footer;

  void RebuildTiles(bool reveal);
  void RebuildCards(bool reveal);
  void ShortcutsChanged();
  void LoadIcon(Tile* tile);
  void TileClicked(Tile* tile);
  void TileMenu(Tile* tile, const gfx::Point& screen_point);
  void CardClicked(Card* card);
  void FooterClicked(int item);

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<Delegate> delegate_;
  Theme theme_ = Theme::DarkBase();
  int panel_bottom_ = 0;
  bool shown_ = true;
  std::unique_ptr<new_tab::Shortcuts> shortcuts_;
  std::vector<raw_ptr<Tile, VectorExperimental>> tiles_;
  std::vector<raw_ptr<Card, VectorExperimental>> cards_;
  raw_ptr<SectionTitle> cards_title_ = nullptr;
  raw_ptr<Footer> footer_ = nullptr;
  LayerMotion motion_;
  std::unique_ptr<ui::SimpleMenuModel> menu_;
  GURL menu_url_;
  base::CancelableTaskTracker tracker_;
  base::WeakPtrFactory<CommandBarHome> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_COMMAND_BAR_COMMAND_BAR_HOME_H_
