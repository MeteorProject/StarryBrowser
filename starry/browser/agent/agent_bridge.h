// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_AGENT_BRIDGE_H_
#define STARRY_BROWSER_AGENT_AGENT_BRIDGE_H_

// `Starry --mcp [--user-data-dir=<目录>]`：MCP 客户端（Claude Code、Codex……）启动
// 的 stdio 桥接服务。它只是一根管子：标准输入的每一行原样转给正在运行的浏览器（数据目录
// 里的套接字，agent_socket.h），浏览器的回应原样写到标准输出。浏览器没在运行就在后台把
// 它启动起来；浏览器没打开外部智能体、也不是内置助手启动的时，对每个请求回一个说明原因的
// 错误。
//
// 内置助手启动本机智能体时，在给它的 MCP 配置的环境变量 STARRY_ASSISTANT_TOKEN 里放一个
// 一次性的令牌：桥连上浏览器后先报这一行（{"starryAssistantToken": …}），浏览器把这条连接
// 归到那个对话。
//
// 这个头文件不能带别的依赖：上游的 chrome/app/chrome_main.cc 直接包含它（补丁）。
namespace starry {

// 在 ChromeMain 的最前面调用（命令行刚准备好）：是 --mcp 的浏览器进程就进入桥，不返回；
// 否则什么也不做。
void MaybeRunAgentBridge();

}  // namespace starry

#endif  // STARRY_BROWSER_AGENT_AGENT_BRIDGE_H_
