// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_MCP_SESSION_H_
#define STARRY_BROWSER_AGENT_MCP_SESSION_H_

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "starry/browser/agent/browser_tools.h"

// MCP（Model Context Protocol）服务端的一条会话：
// 收 JSON-RPC 2.0 消息，回应 initialize、ping、tools/list、tools/call，认
// notifications/cancelled。和传输无关：一行一条消息（stdio 传输的格式），由
// AgentService 从套接字的连接送进来，回应经 send 写回。规范见
// https://modelcontextprotocol.io/specification 。
namespace starry::agent {

class McpSession {
 public:
  // 实现工具的一方（AgentService 的连接）。
  class Provider {
   public:
    // 给模型的使用说明（initialize 的 instructions，McpInstructions）：要看是谁连进来、
    // 它的用户资料允许什么，收到 initialize 时才问。
    virtual std::string Instructions(McpSession* session) = 0;
    // 这个会话能用的工具（MCP 的格式，McpToolDefinitions）。
    virtual base::ListValue Tools(McpSession* session) = 0;
    virtual void CallTool(McpSession* session,
                          const std::string& name,
                          base::DictValue arguments,
                          ToolCallback done) = 0;

   protected:
    virtual ~Provider() = default;
  };

  // 从新到旧。客户端要的版本在里面就照用，否则回最新的，由客户端决定要不要继续。
  static const std::vector<std::string>& SupportedProtocolVersions();
  // JSON-RPC 的错误回应。
  static std::string ErrorLine(const base::Value& id,
                               int code,
                               const std::string& message);

  McpSession(Provider* provider,
             base::RepeatingCallback<void(std::string)> send);
  McpSession(const McpSession&) = delete;
  McpSession& operator=(const McpSession&) = delete;
  ~McpSession();

  // 收到一行。
  void Receive(std::string_view line);
  // 连接断了：之后不再发任何东西。
  void Close();
  // 可用的工具变了（设置里打开了执行脚本）：请客户端重新取列表。
  void ToolsChanged();

  // 客户端在 initialize 里报的名字（claude-code）和可读的标题（新版规范的 title）。
  const std::string& client_name() const { return client_name_; }
  const std::string& client_title() const { return client_title_; }
  bool initialized() const { return initialized_; }
  int running_calls() const { return static_cast<int>(running_.size()); }

 private:
  void Handle(base::DictValue message);
  void Initialize(const base::Value& id, const base::DictValue& params);
  void Call(const base::Value& id, const base::DictValue& params);
  void CallDone(std::string key, base::Value id, ToolResult result);
  void Reply(base::DictValue message);

  const raw_ptr<Provider> provider_;
  base::RepeatingCallback<void(std::string)> send_;
  std::string client_name_;
  std::string client_title_;
  bool initialized_ = false;
  bool closed_ = false;
  // 正在执行的调用和被取消的调用，按请求 ID 的 JSON 文本记。取消的不回应（规范的要求）；
  // 工具本身照常做完（一步做到一半停下比做完更糟）。
  std::set<std::string> running_;
  std::set<std::string> cancelled_;
  base::WeakPtrFactory<McpSession> weak_factory_{this};
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_MCP_SESSION_H_
