// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_AGENT_SOCKET_H_
#define STARRY_BROWSER_AGENT_AGENT_SOCKET_H_

#include <string>

#include "base/files/file_path.h"
#include "base/files/platform_file.h"
#include "base/functional/callback.h"
#include "base/threading/sequence_bound.h"

// 浏览器和 --mcp 桥之间的本机通道。POSIX 用 Unix 域套接字，Windows
// 用当前用户的命名管道。
namespace starry::agent {

inline constexpr char kSocketName[] = "AgentControl.sock";
// 桥的命令行开关（Starry --mcp）。
inline constexpr char kMcpSwitch[] = "mcp";
// 内置助手给本机智能体的令牌：放在 MCP 配置的环境变量里，桥连上后先报一行
// {"starryAssistantToken": "…"}。
inline constexpr char kAssistantTokenVariable[] = "STARRY_ASSISTANT_TOKEN";
inline constexpr char kAssistantTokenKey[] = "starryAssistantToken";
// 外部智能体没打开时给客户端的说明。
inline constexpr char kDisabledMessage[] =
    "Starry Browser is not accepting agent connections. Turn on Settings › "
    "AI Agents › \"Allow AI agents to control the browser\", then reconnect.";

// 数据目录对应的本机端点名。Windows 用命名管道，POSIX 用套接字文件。
base::FilePath SocketPath(const base::FilePath& user_data_dir);

// 连接到当前用户的服务，没人在听或身份不符时返回空句柄。
base::ScopedPlatformFile ConnectSocket(const base::FilePath& path);

// 套接字的服务端（浏览器）：在后台序列上监听，新连接交给 on_connection（UI 线程）。
class SocketServer {
 public:
  explicit SocketServer(
      base::RepeatingCallback<void(base::ScopedPlatformFile)> on_connection);
  SocketServer(const SocketServer&) = delete;
  SocketServer& operator=(const SocketServer&) = delete;
  // 停止监听，删掉套接字文件。
  ~SocketServer();

  // 开始监听。done 的参数是出错的原因，空的是成功。旧文件有人在听说明另一个实例（同一个
  // 数据目录）占着。
  void Start(const base::FilePath& user_data_dir,
             base::OnceCallback<void(std::string)> done);

 private:
  class Core;
  base::SequenceBound<Core> core_;
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_AGENT_SOCKET_H_
