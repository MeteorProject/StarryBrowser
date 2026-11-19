// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_ASSISTANT_SETTINGS_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_ASSISTANT_SETTINGS_HANDLER_H_

#include <memory>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace network {
class SimpleURLLoader;
}

namespace starry {

// 设置页「AI 智能体」分类（settings.js 的 ai_）：内置助手用什么（OpenAI 兼容服务的服务、
// Key、模型，或者本机的 Claude Code、Codex 的路径和模型）、确认规则，外部智能体（MCP）
// 的开关、连接命令和已连接的，允不允许执行脚本。消息：
//   getAssistantSettings → 状态
//   setAssistantSetting [provider|customUrl|model|mode|allowScripts|
//                        allowUploads|backend|localPath|localModel|
//                        allowExternal, 值]
//   setAssistantKey [Key] → 状态（空的是移除）
//   fetchAssistantModels → {models: [{id, name, tools, vision}]} 或 {error}
//   checkLocalAgent       重新找本机智能体、看登没登录（结果在状态的 local 里）
//   disconnectAgents      断开外部智能体
// 改了之后发 assistant-settings-changed，各个窗口的助手也换成新的设置。
class AssistantSettingsHandler : public content::WebUIMessageHandler {
 public:
  explicit AssistantSettingsHandler(Profile* profile);
  AssistantSettingsHandler(const AssistantSettingsHandler&) = delete;
  AssistantSettingsHandler& operator=(const AssistantSettingsHandler&) = delete;
  ~AssistantSettingsHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

 private:
  base::DictValue State() const;
  void Changed();
  void HandleGet(const base::ListValue& args);
  void HandleSet(const base::ListValue& args);
  void HandleSetKey(const base::ListValue& args);
  void HandleFetchModels(const base::ListValue& args);
  void HandleCheckLocalAgent(const base::ListValue& args);
  void HandleDisconnectAgents(const base::ListValue& args);
  void CheckLocalAgent();
  void FetchModelsWithKey(std::string callback_id, std::string key);
  void ModelsFetched(std::string callback_id, std::optional<std::string> body);

  const raw_ptr<Profile> profile_;
  PrefChangeRegistrar prefs_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::CallbackListSubscription agent_status_;
  // 本机智能体的检测结果：{backend, checking, path, loggedIn}。
  base::DictValue local_;
  base::WeakPtrFactory<AssistantSettingsHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_ASSISTANT_SETTINGS_HANDLER_H_
