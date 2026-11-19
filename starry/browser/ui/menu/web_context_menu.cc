// Copyright 2026 The Starry Browser Authors

// 网页的右键菜单：Chrome 建好的 RenderViewContextMenu（菜单项、扩展加的项、执行都是
// Chrome 的）交给 Starry 的菜单显示（starry_hooks.h 的 ShowWebContextMenu）。

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/renderer_context_menu/render_view_context_menu_base.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "starry/browser/agent/page_menus.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "ui/menus/simple_menu_model.h"

namespace starry {

namespace {

// 菜单开着的时候拿着 Chrome 的菜单；网页没了就关掉菜单。
class WebMenuHolder : public content::WebContentsObserver {
 public:
  WebMenuHolder(std::unique_ptr<RenderViewContextMenuBase> menu,
                content::WebContents* contents,
                MenuHostView* host)
      : content::WebContentsObserver(contents),
        menu_(std::move(menu)),
        close_menu_(host->GetCloseCallbackFor(model())) {}

  static void OnMenuClosed(std::unique_ptr<WebMenuHolder> holder) {
    // 菜单命令已经执行完。网页还在时让 Chrome 排队的 MenuClosed 先通知它；
    // 网页正在销毁时随 holder 销毁模型，取消模型投递的关闭通知。
    holder->close_menu_.Reset();
    if (holder->web_contents() && !holder->web_contents()->IsBeingDestroyed()) {
      base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
          FROM_HERE, std::move(holder));
    }
  }

  ui::MenuModel* model() {
    // Chrome 只给只读的；菜单项执行（ActivatedAt）不改模型本身。
    return const_cast<ui::SimpleMenuModel*>(&menu_->menu_model());
  }

  // content::WebContentsObserver:
  void WebContentsDestroyed() override {
    if (close_menu_) {
      // 如果正在执行一项命令，卡片已经收起，模型要活到 OnMenuClosed。
      std::move(close_menu_).Run();
    } else {
      // 菜单已关，网页在排队的 MenuClosed 之前销毁。
      menu_.reset();
    }
  }

 private:
  std::unique_ptr<RenderViewContextMenuBase> menu_;
  base::OnceClosure close_menu_;
};

}  // namespace

bool ShowWebContextMenu(std::unique_ptr<RenderViewContextMenuBase>& menu) {
  // 智能体右键点的：菜单交给它，不显示。
  if (agent::MaybeInterceptPageMenu(*menu)) {
    return true;
  }
  content::RenderFrameHost* frame = menu->GetRenderFrameHost();
  content::WebContents* contents =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  if (!contents) {
    return false;
  }
  // 扩展的弹出页、开发者工具这些不在标签里的网页：照 Chrome 的做。
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  StarryBrowserView* view = browser ? StarryBrowserView::From(browser) : nullptr;
  if (!view || !view->menu_host()) {
    return false;
  }
  if (menu->menu_model().GetItemCount() == 0 ||
      contents->IsShowingContextMenu()) {
    return true;
  }

  // 菜单的位置：鼠标在网页里的位置换成窗口里的。
  gfx::Point point(menu->params().x, menu->params().y);
  if (content::RenderWidgetHostView* widget_view = frame->GetView()) {
    point += widget_view->GetViewBounds().OffsetFromOrigin();
  }
  views::View::ConvertPointFromScreen(view, &point);

  auto holder = std::make_unique<WebMenuHolder>(std::move(menu), contents,
                                                view->menu_host());
  ui::MenuModel* model = holder->model();
  view->ShowMenu(
      model, point,
      base::BindOnce(&WebMenuHolder::OnMenuClosed, std::move(holder)));
  return true;
}

}  // namespace starry
