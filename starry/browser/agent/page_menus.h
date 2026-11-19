// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_PAGE_MENUS_H_
#define STARRY_BROWSER_AGENT_PAGE_MENUS_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"

class RenderViewContextMenuBase;

namespace content {
class WebContents;
}

// 智能体的右键（browser_click 的 button "right"）：网页收到真的 contextmenu；
// 它没有拦下时 Chrome 建好的右键菜单先交给这里，不显示出来，读出智能体能选的项，
// 给了 menuItem 就在这时执行它。
namespace starry::agent {

struct PageMenu {
  // 能选的项（菜单上的名字）。扩展的子菜单展开成「扩展名 › 菜单项」。
  std::vector<std::string> items;
  // 选了的那一项（菜单上的名字）。
  std::optional<std::string> chosen;
  // 要选的项菜单里没有：给模型的说明（带上有哪些项）。
  std::string error;
};

// 接住 contents 的下一个右键菜单（返回的东西在的时候）：item 不空时选它，然后关掉。
// 菜单打开过才回调。
base::ScopedClosureRunner InterceptPageMenu(
    content::WebContents* contents,
    std::optional<std::string> item,
    base::OnceCallback<void(PageMenu)> callback);

// starry::ShowWebContextMenu 先问这里：智能体在等这个网页的菜单时交给它，返回 true（不显示）。
bool MaybeInterceptPageMenu(RenderViewContextMenuBase& menu);

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_PAGE_MENUS_H_
