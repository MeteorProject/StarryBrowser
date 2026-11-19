// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/assistant_handler.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/assistant/assistant_settings.h"
#include "starry/browser/ui/assistant/assistant_island.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/motion/layer_loop.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "url/gurl.h"

namespace starry {

AssistantHandler::AssistantHandler() = default;

AssistantHandler::~AssistantHandler() {
  if (AssistantController* controller = Controller()) {
    controller->DetachPage(this);
  }
}

AssistantIsland* AssistantHandler::Island() {
  StarryBrowserView* view =
      AssistantController::ViewForPage(web_ui()->GetWebContents());
  return view ? view->assistant_island() : nullptr;
}

AssistantController* AssistantHandler::Controller() {
  AssistantIsland* island = Island();
  return island ? island->controller() : nullptr;
}

void AssistantHandler::RegisterMessages() {
  const auto add = [this](const char* name,
                          void (AssistantHandler::*method)(
                              const base::ListValue&)) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("ready", &AssistantHandler::HandleReady);
  add("send", &AssistantHandler::HandleSend);
  add("mentionTabs", &AssistantHandler::HandleMentionTabs);
  add("stop", &AssistantHandler::HandleStop);
  add("newChat", &AssistantHandler::HandleNewChat);
  add("close", &AssistantHandler::HandleClose);
  add("resize", &AssistantHandler::HandleResize);
  add("approve", &AssistantHandler::HandleApprove);
  add("answer", &AssistantHandler::HandleAnswer);
  add("setMode", &AssistantHandler::HandleSetMode);
  add("openSettings", &AssistantHandler::HandleOpenSettings);
  add("openLink", &AssistantHandler::HandleOpenLink);
  add("composerFocus", &AssistantHandler::HandleComposerFocus);
  add("setBackend", &AssistantHandler::HandleSetBackend);
  add("refused", &AssistantHandler::HandleRefused);
  add("collapse", &AssistantHandler::HandleCollapse);
  add("escape", &AssistantHandler::HandleEscape);
  add("activate", &AssistantHandler::HandleActivate);
  add("history", &AssistantHandler::HandleHistory);
  add("openRecord", &AssistantHandler::HandleOpenRecord);
  add("deleteRecord", &AssistantHandler::HandleDeleteRecord);
  add("clearHistory", &AssistantHandler::HandleClearHistory);
  add("capture", &AssistantHandler::HandleCapture);
  add("setReasoning", &AssistantHandler::HandleSetReasoning);
  add("setPanelStyle", &AssistantHandler::HandleSetPanelStyle);
  add("dragOver", &AssistantHandler::HandleDragOver);
  // 这一页提交时记下导航开始的时刻（DidFinishNavigation）。
  Observe(web_ui()->GetWebContents());
}

void AssistantHandler::OnJavascriptAllowed() {
  if (AssistantController* controller = Controller()) {
    controller->AttachPage(this);
  }
}

void AssistantHandler::OnJavascriptDisallowed() {
  if (AssistantController* controller = Controller()) {
    controller->DetachPage(this);
  }
}

void AssistantHandler::Fire(const std::string& event, base::Value payload) {
  if (IsJavascriptAllowed()) {
    FireWebUIListener(event, payload);
  }
}

void AssistantHandler::DidFinishNavigation(
    content::NavigationHandle* navigation) {
  if (!navigation->HasCommitted() || navigation->IsSameDocument() ||
      navigation->GetRenderFrameHost() != web_ui()->GetRenderFrameHost()) {
    return;
  }
  navigation_start_ = navigation->NavigationStart();
  SendLoopGrid();
}

void AssistantHandler::SendLoopGrid() {
  if (IsJavascriptAllowed() && !navigation_start_.is_null()) {
    FireWebUIListener("loopGrid",
                      base::Value(LayerLoop::GridPhaseMs(navigation_start_)));
  }
}

void AssistantHandler::HandleReady(const base::ListValue& args) {
  AllowJavascript();
  SendLoopGrid();
  if (AssistantController* controller = Controller()) {
    controller->AttachPage(this);
    controller->PageReady();
  }
}

void AssistantHandler::HandleSend(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (!controller || args.empty() || !args[0].is_string()) {
    return;
  }
  // 图片网页已经缩好、转好格式（PNG、JPEG、GIF、WebP，最长边 2048）。
  std::vector<agent::ToolImage> images;
  if (args.size() > 1 && args[1].is_list()) {
    for (const base::Value& value : args[1].GetList()) {
      if (!value.is_dict()) {
        continue;
      }
      const std::string* mime = value.GetDict().FindString("mime");
      const std::string* data = value.GetDict().FindString("data");
      if (mime && data && !data->empty() &&
          (*mime == "image/png" || *mime == "image/jpeg" ||
           *mime == "image/gif" || *mime == "image/webp")) {
        images.push_back(agent::ToolImage{*mime, *data});
      }
    }
  }
  // 用户 @ 的标签（短 ID）。
  std::vector<std::string> mentions;
  if (args.size() > 2 && args[2].is_list()) {
    for (const base::Value& value : args[2].GetList()) {
      if (value.is_string()) {
        mentions.push_back(value.GetString());
      }
    }
  }
  controller->Send(args[0].GetString(), std::move(images), mentions);
}

void AssistantHandler::HandleMentionTabs(const base::ListValue& args) {
  if (AssistantController* controller = Controller()) {
    controller->ListMentionTabs();
  }
}

void AssistantHandler::HandleStop(const base::ListValue& args) {
  if (AssistantController* controller = Controller()) {
    controller->Stop();
  }
}

void AssistantHandler::HandleNewChat(const base::ListValue& args) {
  if (AssistantController* controller = Controller()) {
    controller->NewChat();
  }
}

void AssistantHandler::HandleClose(const base::ListValue& args) {
  if (AssistantIsland* island = Island()) {
    island->Close();
  }
}

void AssistantHandler::HandleResize(const base::ListValue& args) {
  AssistantIsland* island = Island();
  if (island && !args.empty() && args[0].is_double()) {
    island->SetContentHeight(static_cast<int>(args[0].GetDouble()));
  } else if (island && !args.empty() && args[0].is_int()) {
    island->SetContentHeight(args[0].GetInt());
  }
}

void AssistantHandler::HandleApprove(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (!controller || args.size() < 2 || !args[0].is_string() ||
      !args[1].is_string()) {
    return;
  }
  controller->Approve(args[0].GetString(), args[1].GetString(),
                      args.size() > 2 && args[2].is_string()
                          ? args[2].GetString()
                          : std::string());
}

void AssistantHandler::HandleAnswer(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (!controller || args.empty() || !args[0].is_string()) {
    return;
  }
  controller->Answer(args[0].GetString(),
                     args.size() > 1 && args[1].is_string()
                         ? std::optional<std::string>(args[1].GetString())
                         : std::nullopt);
}

void AssistantHandler::HandleSetMode(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (controller && !args.empty() && args[0].is_string()) {
    controller->SetMode(args[0].GetString());
  }
}

void AssistantHandler::HandleOpenSettings(const base::ListValue& args) {
  StarryBrowserView* view =
      AssistantController::ViewForPage(web_ui()->GetWebContents());
  if (view) {
    OpenStarrySettings(view->browser(), "ai");
  }
}

void AssistantHandler::HandleOpenLink(const base::ListValue& args) {
  StarryBrowserView* view =
      AssistantController::ViewForPage(web_ui()->GetWebContents());
  if (!view || args.empty() || !args[0].is_string()) {
    return;
  }
  const GURL url(args[0].GetString());
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  // 回答里的链接在这个窗口的新标签里打开。
  NavigateParams params(view->browser(), url, ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
}

void AssistantHandler::HandleComposerFocus(const base::ListValue& args) {
  AssistantIsland* island = Island();
  if (island && !args.empty() && args[0].is_bool()) {
    island->SetComposerFocused(args[0].GetBool());
  }
}

void AssistantHandler::HandleSetBackend(const base::ListValue& args) {
  StarryBrowserView* view =
      AssistantController::ViewForPage(web_ui()->GetWebContents());
  if (!view || args.empty() || !args[0].is_string()) {
    return;
  }
  // 输入框「+」菜单里换后端：同设置页，所有窗口跟着变（下一条消息起）。
  assistant::SetBackend(view->browser()->GetProfile(), args[0].GetString());
  if (AssistantController* controller = Controller()) {
    controller->SettingsChanged();
  }
}

void AssistantHandler::HandleRefused(const base::ListValue& args) {
  if (AssistantIsland* island = Island()) {
    island->Shake();
  }
}

void AssistantHandler::HandleCollapse(const base::ListValue& args) {
  if (AssistantIsland* island = Island()) {
    island->Collapse();
  }
}

void AssistantHandler::HandleEscape(const base::ListValue& args) {
  AssistantIsland* island = Island();
  if (!island) {
    return;
  }
  // 侧栏：直接关掉。
  if (island->docked() || island->collapsed() || !island->Collapse()) {
    island->Close();
  }
}

namespace {

std::optional<std::string> StringArg(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return std::nullopt;
  }
  return args[0].GetString();
}

}  // namespace

void AssistantHandler::HandleActivate(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (std::optional<std::string> id = StringArg(args); controller && id) {
    controller->Activate(*id, /*show_tab=*/true);
  }
}

void AssistantHandler::HandleHistory(const base::ListValue& args) {
  if (AssistantController* controller = Controller()) {
    controller->ShowHistory(StringArg(args));
  }
}

void AssistantHandler::HandleOpenRecord(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (std::optional<std::string> id = StringArg(args); controller && id) {
    controller->OpenRecord(*id);
  }
}

void AssistantHandler::HandleDeleteRecord(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (std::optional<std::string> id = StringArg(args); controller && id) {
    controller->DeleteRecord(*id);
  }
}

void AssistantHandler::HandleClearHistory(const base::ListValue& args) {
  if (AssistantController* controller = Controller()) {
    controller->ClearHistory();
  }
}

void AssistantHandler::HandleCapture(const base::ListValue& args) {
  if (AssistantController* controller = Controller()) {
    controller->CapturePage();
  }
}

void AssistantHandler::HandleSetReasoning(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (std::optional<std::string> level = StringArg(args); controller && level) {
    controller->SetReasoning(*level);
  }
}

void AssistantHandler::HandleSetPanelStyle(const base::ListValue& args) {
  AssistantController* controller = Controller();
  if (std::optional<std::string> style = StringArg(args); controller && style) {
    controller->SetPanelStyle(*style);
  }
}

void AssistantHandler::HandleDragOver(const base::ListValue& args) {
  AssistantIsland* island = Island();
  if (island && !args.empty() && args[0].is_bool()) {
    island->SetDragOver(args[0].GetBool());
  }
}

}  // namespace starry
