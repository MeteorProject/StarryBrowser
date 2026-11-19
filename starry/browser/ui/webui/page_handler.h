// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_PAGE_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_PAGE_HANDLER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "ui/menus/simple_menu_model.h"

namespace starry {

// 每个 Starry 页面都有的消息（页面里 common/browser.js 调用）：用 Starry 自己的菜单、对话框
// 显示页面要的东西（和侧栏、标签用的是同一个玻璃菜单和对话框层），按修饰键打开网址，拷贝。
//
//   showMenu(callbackId, items, anchor)
//     items：[{id, title, icon?, enabled?, checked?, children?} |
//             {separator: true} | {header}]；children 是同样格式的子菜单。
//     anchor：页面里的 {x, y}（鼠标处）或者 {x, y, width, height, below: true}（按钮）。
//     选了一项回 id，没选回 null。
//   showDialog(callbackId, {icon?, tone?, title, message?, field?, buttons})
//     buttons：[{title, variant?, cancel?}]，最后一个是默认按钮。回 {button, input}。
//   openUrl(url, target)：target 是 current、foreground、background、window。
//   copyText(text)
//
// 图标按字符串名称索引，映射表见 page_handler.cc 的 IconByName。
class StarryPageHandler : public content::WebUIMessageHandler,
                          public ui::SimpleMenuModel::Delegate {
 public:
  StarryPageHandler();
  StarryPageHandler(const StarryPageHandler&) = delete;
  StarryPageHandler& operator=(const StarryPageHandler&) = delete;
  ~StarryPageHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdChecked(int command_id) const override;
  bool IsCommandIdEnabled(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  void HandleShowMenu(const base::ListValue& args);
  void BuildMenu(const base::ListValue& items, ui::SimpleMenuModel* model);
  void HandleShowDialog(const base::ListValue& args);
  void HandleOpenUrl(const base::ListValue& args);
  void HandleCopyText(const base::ListValue& args);
  void OnMenuClosed();

  // 开着的菜单：模型、每一项的 id、选了哪个。
  std::unique_ptr<ui::SimpleMenuModel> menu_;
  std::vector<std::unique_ptr<ui::SimpleMenuModel>> submenus_;
  std::vector<std::string> menu_ids_;
  std::vector<bool> menu_checked_;
  std::vector<bool> menu_enabled_;
  std::optional<std::string> picked_;
  std::string menu_callback_;

  base::WeakPtrFactory<StarryPageHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_PAGE_HANDLER_H_
