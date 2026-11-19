// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/agent_bridge.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/common/content_switches.h"
#include "starry/browser/agent/agent_bridge_platform.h"
#include "starry/browser/agent/agent_socket.h"
#include "starry/browser/agent/line_channel.h"
#include "starry/browser/agent/mcp_session.h"
#if BUILDFLAG(IS_POSIX)
#include <signal.h>
#endif

namespace starry {

namespace {

using agent::ConnectSocket;
using agent::SocketPath;

// 浏览器不在运行时启动它，最多等这么久；在运行（只是没打开外部智能体）时等一小会儿。
constexpr base::TimeDelta kLaunchWait = base::Seconds(25);
constexpr base::TimeDelta kRunningWait = base::Seconds(3);

// 浏览器的数据目录：同浏览器自己的算法（--user-data-dir 解开符号链接，否则是默认的）。
base::FilePath UserDataDir(bool* custom) {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  base::FilePath dir = command_line.GetSwitchValuePath(switches::kUserDataDir);
  *custom = !dir.empty();
  if (dir.empty()) {
    chrome::GetDefaultUserDataDirectory(&dir);
    return dir;
  }
  dir = dir.StripTrailingSeparators();
  const base::FilePath resolved = base::MakeAbsoluteFilePath(dir);
  if (!resolved.empty()) {
    return resolved;
  }
#if BUILDFLAG(IS_POSIX)
  return base::MakeAbsoluteFilePathNoResolveSymbolicLinks(dir).value_or(dir);
#else
  return dir;
#endif
}

// 连不上浏览器：读标准输入，对每个请求回一个错误（客户端会把它显示给用户），直到客户端
// 关掉。
[[noreturn]] void ServeUnavailable(const std::string& message) {
  agent::bridge::WriteOutput("starry-browser: " + message + "\n", true);
  std::string buffer;
  char chunk[65536];
  while (true) {
    const int count = agent::bridge::ReadInput(chunk);
    if (count <= 0) {
      std::_Exit(1);
    }
    buffer.append(chunk, static_cast<size_t>(count));
    size_t newline;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      if (newline > agent::LineChannel::kMaxLineLength) {
        std::_Exit(1);
      }
      const std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      std::optional<base::Value> request =
          base::JSONReader::Read(line, base::JSON_PARSE_RFC);
      if (!request || !request->is_dict() ||
          !request->GetDict().FindString("method")) {
        continue;
      }
      const base::Value* id = request->GetDict().Find("id");
      if (!id) {
        continue;
      }
      agent::bridge::WriteOutput(
          agent::McpSession::ErrorLine(*id, -32000, message) + "\n", false);
    }
    if (buffer.size() > agent::LineChannel::kMaxLineLength) {
      std::_Exit(1);
    }
  }
}

}  // namespace

void MaybeRunAgentBridge() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (!command_line.HasSwitch(agent::kMcpSwitch) ||
      command_line.HasSwitch(switches::kProcessType)) {
    return;
  }
  // 客户端关掉读的一端时不要被 SIGPIPE 杀掉，写失败自己退出。
#if BUILDFLAG(IS_POSIX)
  signal(SIGPIPE, SIG_IGN);
#endif
  bool custom = false;
  base::FilePath dir = UserDataDir(&custom);
  base::ScopedPlatformFile fd = ConnectSocket(SocketPath(dir));
  if (!fd.is_valid()) {
    const bool running = agent::bridge::BrowserRunning(dir);
    if (!running) {
      agent::bridge::LaunchBrowser(dir, custom);
    }
    const base::TimeTicks deadline =
        base::TimeTicks::Now() + (running ? kRunningWait : kLaunchWait);
    while (!fd.is_valid() && base::TimeTicks::Now() < deadline) {
      base::PlatformThread::Sleep(base::Milliseconds(200));
      // 刚启动的浏览器才建好数据目录：重新算一次（符号链接解开后的路径）。
      dir = UserDataDir(&custom);
      fd = ConnectSocket(SocketPath(dir));
    }
    if (!fd.is_valid()) {
      ServeUnavailable(
          running ? agent::kDisabledMessage
                  : "Starry Browser did not start accepting agent "
                    "connections in time. Make sure \"Allow AI agents to "
                    "control the browser\" is on in Settings › AI Agents, "
                    "then reconnect.");
    }
  }
  const std::optional<std::string> token =
      base::Environment::Create()->GetVar(agent::kAssistantTokenVariable);
  if (token && !token->empty()) {
    const std::string hello =
        base::WriteJson(
            base::DictValue().Set(agent::kAssistantTokenKey, *token))
            .value_or("{}");
    if (!agent::bridge::WriteSocket(fd.get(), hello + "\n")) {
      std::_Exit(1);
    }
  }
  agent::bridge::Pump(fd.get());
}

}  // namespace starry
