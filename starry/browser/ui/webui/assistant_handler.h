// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_ASSISTANT_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_ASSISTANT_HANDLER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/ui/assistant/assistant_controller.h"

namespace starry {

class AssistantIsland;

// 岛里的网页（starry://assistant，pages/assistant/assistant.js）和窗口的
// AssistantController 之间转消息：
//   网页 → 浏览器：ready、send [文字, [{mime, data}]]、stop、newChat、close、
//     resize [高度]、approve [id, allow|site|deny, 补充]、answer [id, 回答]、
//     setMode [ask|agent|auto]、setReasoning [等级|""]、setPanelStyle [float|dock]、
//     openSettings、openLink [网址]、composerFocus [有没有焦点]、
//     setBackend [chat|claudeCode|codex]、refused（做事时又按了发送：岛晃一下）、
//     collapse（标题行的 ∨：收成一条）、escape（Esc：有对话时先收起，再按一次关掉）、
//     activate [对话]、history [搜索词|null]、openRecord [id]、deleteRecord [id]、
//     clearHistory、capture（截取页面）、dragOver [是不是]
//   浏览器 → 网页：见 AssistantController 的说明；另外 loopGrid [毫秒]：浏览器的循环动画
//     （LayerLoop）的网格落在这一页时间线的哪里，网页把自己的循环动画对齐到它上面。
class AssistantHandler : public content::WebUIMessageHandler,
                         public content::WebContentsObserver,
                         public AssistantController::Page {
 public:
  AssistantHandler();
  AssistantHandler(const AssistantHandler&) = delete;
  AssistantHandler& operator=(const AssistantHandler&) = delete;
  ~AssistantHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // AssistantController::Page:
  void Fire(const std::string& event, base::Value payload) override;

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* navigation) override;

 private:
  // 网页的时间线从导航开始算：知道了就告诉网页网格在哪里。
  void SendLoopGrid();

  AssistantIsland* Island();
  AssistantController* Controller();
  void HandleReady(const base::ListValue& args);
  void HandleSend(const base::ListValue& args);
  void HandleMentionTabs(const base::ListValue& args);
  void HandleStop(const base::ListValue& args);
  void HandleNewChat(const base::ListValue& args);
  void HandleClose(const base::ListValue& args);
  void HandleResize(const base::ListValue& args);
  void HandleApprove(const base::ListValue& args);
  void HandleAnswer(const base::ListValue& args);
  void HandleSetMode(const base::ListValue& args);
  void HandleOpenSettings(const base::ListValue& args);
  void HandleOpenLink(const base::ListValue& args);
  void HandleComposerFocus(const base::ListValue& args);
  void HandleSetBackend(const base::ListValue& args);
  void HandleRefused(const base::ListValue& args);
  void HandleCollapse(const base::ListValue& args);
  void HandleEscape(const base::ListValue& args);
  void HandleActivate(const base::ListValue& args);
  void HandleHistory(const base::ListValue& args);
  void HandleOpenRecord(const base::ListValue& args);
  void HandleDeleteRecord(const base::ListValue& args);
  void HandleClearHistory(const base::ListValue& args);
  void HandleCapture(const base::ListValue& args);
  void HandleSetReasoning(const base::ListValue& args);
  void HandleSetPanelStyle(const base::ListValue& args);
  void HandleDragOver(const base::ListValue& args);

  // 这一页的导航开始的时刻（网页时间线的 0）。
  base::TimeTicks navigation_start_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_ASSISTANT_HANDLER_H_
