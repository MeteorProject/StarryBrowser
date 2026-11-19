// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_AGENT_SERVICE_H_
#define STARRY_BROWSER_AGENT_AGENT_SERVICE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/files/platform_file.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "starry/browser/agent/browser_tools.h"
#include "starry/browser/agent/mcp_session.h"

// 智能体经 MCP 连进浏览器：数据目录里的套接字（agent_socket.h）、每条连接一个
// McpSession。
//
// 连接有两种：
// - 内置助手启动的本机智能体（Claude Code、Codex）：桥先报助手登记的令牌，连接归到那个对话
//   （ConnectionOwner），工具在对话自己的那套工具上执行，改变网页的一步经对话确认。令牌
//   作废（智能体退出、对话关掉）时连接一起断开。
// - 外部智能体（用户自己配的 MCP 客户端）：设置里打开「允许 AI 智能体控制浏览器」时才接，
//   否则对每个请求回一个说明怎么打开的错误。它们的客户端自己会请用户确认，这里不问。
//
// 套接字服务在有令牌或者打开了外部智能体时开着。整个浏览器进程一个，只在 UI 线程上用。
namespace starry::agent {

class SocketServer;

// 连接归谁：内置助手里的一个对话。
class ConnectionOwner {
 public:
  // 这个对话能用的工具（MCP 的格式，带 ask_user）和给模型的使用说明。
  virtual base::ListValue AgentTools() = 0;
  virtual std::string AgentInstructions() = 0;
  // 在对话自己的那套工具上执行（确认、网页上的提示都和自己调模型时一样）。
  virtual void RunAgentTool(const std::string& name,
                            base::DictValue arguments,
                            ToolCallback done) = 0;

 protected:
  virtual ~ConnectionOwner() = default;
};

class AgentService : public McpSession::Provider {
 public:
  struct Status {
    bool listening = false;
    // 套接字起不来的原因（另一个实例占着……）。
    std::string error;
    // 已连接的外部智能体（客户端报的名字）。
    std::vector<std::string> clients;
  };

  static AgentService* Get();

  AgentService(const AgentService&) = delete;
  AgentService& operator=(const AgentService&) = delete;

  // 浏览器退出前：断开所有连接，删掉套接字文件。
  void Shutdown();

  // 内置助手要启动本机智能体：登记一个一次性的随机令牌（24 字节），套接字服务随之打开。
  std::string RegisterToken(base::WeakPtr<ConnectionOwner> owner);
  // 本机智能体退出了、对话关掉了：令牌作废，经它连进来的连接断开。
  void RevokeToken(const std::string& token);

  // 设置改了（外部智能体的开关、执行脚本、无痕标签）：开关套接字服务，关掉不再允许的
  // 无痕窗口，请客户端重新取工具列表。
  void SettingsChanged();
  Status GetStatus() const;
  // 设置页的「全部断开」：只断外部智能体（助手的不断）。
  void DisconnectExternal();
  // 状态变了（设置页显示的那些）。
  base::CallbackListSubscription AddStatusObserver(
      base::RepeatingClosure callback);

  // McpSession::Provider:
  std::string Instructions(McpSession* session) override;
  base::ListValue Tools(McpSession* session) override;
  void CallTool(McpSession* session,
                const std::string& name,
                base::DictValue arguments,
                ToolCallback done) override;

 private:
  class ExternalAgent;
  struct Connection;
  struct PendingCall;
  friend class base::NoDestructor<AgentService>;

  AgentService();
  ~AgentService() override;

  bool ExternalAllowed() const;
  bool WantsServer() const;
  // 按现在要不要开着，开、关套接字服务。
  void Refresh();
  void ServerStarted(std::string error);
  void OnConnection(base::ScopedPlatformFile fd);
  void OnLine(int id, std::string line);
  void OnClosed(int id);
  void Drop(int id);
  // 网页上的提示条写的名字：客户端报的 title（没有时是 name）。
  std::u16string ClientName(int id) const;
  Connection* ForSession(McpSession* session);
  void RunNext(Connection* connection);
  void CallDone(int id, ToolResult result);
  void NotifyStatus();

  std::unique_ptr<SocketServer> server_;
  bool listening_ = false;
  std::string error_;
  std::map<std::string, base::WeakPtr<ConnectionOwner>> tokens_;
  std::map<int, std::unique_ptr<Connection>> connections_;
  int next_id_ = 1;
  base::RepeatingClosureList status_observers_;
  base::WeakPtrFactory<AgentService> server_callbacks_{this};
  base::WeakPtrFactory<AgentService> weak_factory_{this};
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_AGENT_SERVICE_H_
