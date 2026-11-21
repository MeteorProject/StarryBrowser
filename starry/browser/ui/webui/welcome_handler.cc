// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/welcome_handler.h"

#include <optional>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/assistant/local_agent.h"
#include "starry/browser/onboarding/onboarding.h"
#include "starry/browser/ui/frame/starry_browser_view.h"

namespace starry {

namespace {

StarryBrowserView* BrowserViewFor(content::WebContents* contents) {
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab || !tab->GetBrowserWindowInterface()) {
    return nullptr;
  }
  return StarryBrowserView::From(tab->GetBrowserWindowInterface());
}

// 后台线程检测本机智能体（会跑子进程、查询登录 shell）。
base::DictValue DetectLocalAgent(assistant::LocalAgentKind kind) {
  base::DictValue result;
  const base::FilePath path = assistant::LocateLocalAgent(kind, std::string());
  if (path.empty()) {
    return result;
  }
  result.Set("found", true);
  if (std::optional<bool> logged_in =
          assistant::LocalAgentLoggedIn(kind, path)) {
    result.Set("loggedIn", *logged_in);
  }
  return result;
}

}  // namespace

WelcomeHandler::WelcomeHandler(Profile* profile) : profile_(profile) {}

WelcomeHandler::~WelcomeHandler() = default;

void WelcomeHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("detectLocalAgents", &WelcomeHandler::HandleDetectLocalAgents);
  add("finishOnboarding", &WelcomeHandler::HandleFinishOnboarding);
}

void WelcomeHandler::HandleDetectLocalAgents(const base::ListValue& args) {
  AllowJavascript();
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce([] {
        return base::DictValue()
            .Set("claudeCode",
                 DetectLocalAgent(assistant::LocalAgentKind::kClaudeCode))
            .Set("codex", DetectLocalAgent(assistant::LocalAgentKind::kCodex));
      }),
      base::BindOnce(&WelcomeHandler::OnLocalAgentsDetected,
                     weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void WelcomeHandler::OnLocalAgentsDetected(std::string callback_id,
                                           base::DictValue result) {
  if (IsJavascriptAllowed()) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }
}

void WelcomeHandler::HandleFinishOnboarding(const base::ListValue& args) {
  content::WebContents* contents = web_ui()->GetWebContents();
  if (StarryBrowserView* view = BrowserViewFor(contents)) {
    view->FinishOnboarding(contents);
    return;
  }
  onboarding::MarkFinished(profile_);
}

}  // namespace starry
