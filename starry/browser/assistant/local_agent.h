// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_LOCAL_AGENT_H_
#define STARRY_BROWSER_ASSISTANT_LOCAL_AGENT_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"

// 本机的智能体：Claude Code（claude）和 Codex（codex）。找可执行文件、看它登没登录，
// 不调用模型。这里的函数都会阻塞（跑子进程、问登录 shell），只在后台线程上调用。
namespace starry::assistant {

enum class LocalAgentKind { kClaudeCode, kCodex };

// 浏览器的工具经哪个 MCP 服务器交给本机智能体：浏览器自己的 --mcp 桥，环境变量里带着
// 令牌（agent_bridge.h），连进来的连接就归面板里的这个对话。
struct BrowserMcpServer {
  BrowserMcpServer();
  BrowserMcpServer(const BrowserMcpServer&);
  BrowserMcpServer& operator=(const BrowserMcpServer&);
  ~BrowserMcpServer();

  std::string command;
  std::vector<std::string> arguments;
  std::map<std::string, std::string> environment;
};

// MCP 配置里的名字，工具在智能体那边叫 mcp__starry__browser_click 之类。
inline constexpr char kBrowserMcpServerName[] = "starry";
// 去掉 mcp__starry__ 前缀。
std::string BrowserToolName(const std::string& name);
// 从文字里看是不是没登录（「Not logged in · Please run /login」这类）。
bool LooksLikeAuthentication(const std::string& message);

// 设置里存的后端名（"claudeCode"、"codex"）。
std::optional<LocalAgentKind> LocalAgentFromBackend(const std::string& backend);
const char* BackendName(LocalAgentKind kind);
const char* ExecutableName(LocalAgentKind kind);
// 界面上的名字（产品名，不翻译）。
std::u16string DisplayName(LocalAgentKind kind);

// 可执行文件。configured 是设置里手填的路径（优先，~ 开头的展开）。先找常见的安装位置
// （~/.local/bin、Homebrew、npm、bun、volta、pnpm、nvm），再问一次登录 shell
// （从访达打开的 App 拿不到 shell 的 PATH）。找不到返回空的。
base::FilePath LocateLocalAgent(LocalAgentKind kind,
                                const std::string& configured);

// 登录状态。nullopt：看不出来（命令失败、输出读不懂）。只是提示，不拦着。
std::optional<bool> LocalAgentLoggedIn(LocalAgentKind kind,
                                       const base::FilePath& executable);

// Codex 认得的功能开关（`codex features list` 的第一列）：只关它认得的，免得不同版本
// 报未知的开关。
std::set<std::string> CodexFeatures(const base::FilePath& executable);
// 用户在 Codex 配置（$CODEX_HOME/config.toml）里加的 MCP 服务器：内置助手用 Codex 时把它们
// 关掉，只留浏览器的工具。
std::vector<std::string> CodexMcpServers(const std::string& config_text);
base::FilePath CodexConfigFile();

// 启动本机智能体要准备的（在后台线程上：找可执行文件会跑子进程、问登录 shell）。
// 内置助手的对话、标签管理的 AI 整理共用。
struct LocalLaunch {
  LocalLaunch();
  LocalLaunch(LocalLaunch&&);
  LocalLaunch& operator=(LocalLaunch&&);
  ~LocalLaunch();

  // 找不到时是空的（别的也都是空的）。
  base::FilePath executable;
  base::EnvironmentMap environment;
  // 数据目录下的空文件夹 Assistant/Workspace：不会读到哪个项目的 .mcp.json、hooks。
  base::FilePath workspace;
  // Codex：要关掉的自带功能（它认得的）、用户配置里的 MCP 服务器。
  std::vector<std::string> disabled_features;
  std::vector<std::string> disabled_servers;
};
LocalLaunch PrepareLocalLaunch(LocalAgentKind kind,
                               std::string configured,
                               base::FilePath user_data);

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_LOCAL_AGENT_H_
