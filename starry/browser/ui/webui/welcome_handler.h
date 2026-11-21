// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_WELCOME_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_WELCOME_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace starry {

// 设置向导（pages/welcome/）专用的 WebUI 消息；设置读写复用设置页的各 Handler。
//
//   detectLocalAgents(callbackId)：后台检测本机 Claude Code 和 Codex，返回
//     {claudeCode: {found, loggedIn}, codex: {…}}（未找到时为空对象）。
//   finishOnboarding()：标记向导完成，恢复浏览器界面并替换/关闭向导标签。
class WelcomeHandler : public content::WebUIMessageHandler {
 public:
  explicit WelcomeHandler(Profile* profile);
  WelcomeHandler(const WelcomeHandler&) = delete;
  WelcomeHandler& operator=(const WelcomeHandler&) = delete;
  ~WelcomeHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  void HandleDetectLocalAgents(const base::ListValue& args);
  void HandleFinishOnboarding(const base::ListValue& args);
  void OnLocalAgentsDetected(std::string callback_id, base::DictValue result);

  const raw_ptr<Profile> profile_;
  base::WeakPtrFactory<WelcomeHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_WELCOME_HANDLER_H_
