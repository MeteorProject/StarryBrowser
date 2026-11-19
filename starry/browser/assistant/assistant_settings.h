// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_ASSISTANT_SETTINGS_H_
#define STARRY_BROWSER_ASSISTANT_ASSISTANT_SETTINGS_H_

#include <string>

#include "base/functional/callback.h"
#include "starry/browser/assistant/chat_backend.h"

class Profile;

// 内置助手的设置：用哪家服务、每家选的模型、自定义服务的地址、确认规则、允不允许执行
// 脚本。存在 Chrome 的配置里，无痕窗口用原来那个用户资料的（prefs::PrefsFor）。
//
// API Key 不明文存：用 Chrome 加密 Cookie、密码的那把钥匙（OSCryptAsync，macOS 上在钥匙串
// 里）加密后放进配置；读的时候解密。
namespace starry::assistant {

inline constexpr char kCustomProviderId[] = "custom";

// 确认规则：问答（每一步操作前都问）、代理（只在敏感操作前问）、自动（敏感操作
// 先让 AI 审核，拿不准的再问，AssistantReviewer）。
inline constexpr char kModeAsk[] = "ask";
inline constexpr char kModeAgent[] = "agent";
inline constexpr char kModeAuto[] = "auto";

// 助手的样子：网页下方的岛、右边的侧栏。
inline constexpr char kPanelFloat[] = "float";
inline constexpr char kPanelDock[] = "dock";

// 用什么跑助手（assistant_backend.h）。
inline constexpr char kBackendChat[] = "chat";
inline constexpr char kBackendClaudeCode[] = "claudeCode";
inline constexpr char kBackendCodex[] = "codex";

struct Settings {
  Settings();
  Settings(const Settings&);
  Settings& operator=(const Settings&);
  ~Settings();

  std::string backend = kBackendChat;
  // 本机智能体（backend 是 claudeCode、codex 时）的可执行文件和模型，空着是自动找、
  // 用它自己的默认。
  std::string local_path;
  std::string local_model;
  std::string provider_id;
  // 内置的那一家，或者自定义（名字、地址来自设置）。
  ChatProvider provider;
  std::string model;
  std::string mode = kModeAsk;
  bool allow_scripts = false;
  bool allow_uploads = true;
  bool allow_private_tabs = true;
  bool has_key = false;
  // 选的思考程度（现在选不了的当作默认，是空的）和能选的等级（空的：调不了）。
  std::string reasoning;
  std::vector<std::string> reasoning_levels;
  std::string panel_style = kPanelFloat;
  bool is_local() const { return backend != kBackendChat; }
  bool docked() const { return panel_style == kPanelDock; }
  // 思考程度按什么记：OpenAI 兼容服务按服务（各家的等级不一样），本机智能体各一个。
  std::string reasoning_key() const;
  // OpenAI 兼容服务：服务选了、模型填了、要 Key 的有 Key。本机智能体总是算设置好了
  // （找不到可执行文件在发消息时才知道）。
  bool Configured() const;
};

Settings ReadSettings(Profile* profile);
// 标签管理的「AI 整理」用的（prefs::kOrganizerBackend……）：没单独选时和 AI 助手相同；选了
// OpenAI 兼容服务时换成那个服务和模型（Key 是助手存的那份），选了本机智能体时是它的
// 可执行文件和模型。思考程度总是默认的。
Settings ReadOrganizerSettings(Profile* profile);
void SetProvider(Profile* profile, const std::string& provider_id);
void SetModel(Profile* profile,
              const std::string& provider_id,
              const std::string& model);
void SetCustomBaseUrl(Profile* profile, const std::string& url);
std::string CustomBaseUrl(Profile* profile);
void SetMode(Profile* profile, const std::string& mode);
void SetBackend(Profile* profile, const std::string& backend);
std::string LocalPath(Profile* profile, const std::string& backend);
std::string LocalModel(Profile* profile, const std::string& backend);
void SetLocalPath(Profile* profile,
                  const std::string& backend,
                  const std::string& path);
void SetLocalModel(Profile* profile,
                   const std::string& backend,
                   const std::string& model);
void SetAllowScripts(Profile* profile, bool allow);
void SetAllowUploads(Profile* profile, bool allow);
void SetAllowPrivateTabs(Profile* profile, bool allow);
// 现在的后端（reasoning_key）用的思考程度，空的是默认。
void SetReasoning(Profile* profile, const std::string& level);
void SetPanelStyle(Profile* profile, const std::string& style);
// 总开关（prefs::kAssistantEnabled）：关掉时助手打不开、没有 ✦ 按钮。
bool Enabled(Profile* profile);
void SetEnabled(Profile* profile, bool enabled);
// Codex 的 model/list 说了它在用的模型支持哪些（configured_model：设置里填的模型，
// 空的是它的默认模型）。记在进程里，设置里换了模型就不算数。
void SetCodexReasoningLevels(const std::string& configured_model,
                             std::vector<std::string> levels);

// 读、存一家服务的 Key（异步：等加密的钥匙准备好）。读不出来时是空的。
void GetApiKey(Profile* profile,
               const std::string& provider_id,
               base::OnceCallback<void(std::string)> callback);
void SetApiKey(Profile* profile,
               const std::string& provider_id,
               const std::string& key,
               base::OnceClosure done);

// 界面上的服务名（中文的几家用本地化的名字）。
std::u16string ProviderDisplayName(const ChatProvider& provider);

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_ASSISTANT_SETTINGS_H_
