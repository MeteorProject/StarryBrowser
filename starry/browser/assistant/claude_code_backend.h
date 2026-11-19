// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_CLAUDE_CODE_BACKEND_H_
#define STARRY_BROWSER_ASSISTANT_CLAUDE_CODE_BACKEND_H_

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "starry/browser/assistant/assistant_backend.h"
#include "starry/browser/assistant/local_agent.h"

// 本机的 Claude Code 后端：`claude -p`，标准输入、输出都是 stream-json（每行一个
// JSON），一个对话一个常驻进程。用它自己的登录和模型；它自带的工具全部关掉
// （--tools ""），只有浏览器的工具（--mcp-config 只有 starry 一个服务器 +
// --strict-mcp-config）。见 https://code.claude.com/docs/en/headless 。
namespace starry::assistant {

class LineProcess;

class ClaudeCodeBackend : public Backend {
 public:
  struct Config {
    Config();
    Config(const Config&);
    Config& operator=(const Config&);
    ~Config();

    base::FilePath executable;
    // 空的用 Claude Code 自己设的模型。
    std::string model;
    std::string system_prompt;
    // 没有：自动审核用的，没有浏览器的工具，system_prompt 换掉它自己的、不存会话。
    std::optional<BrowserMcpServer> server;
    // 空文件夹：不让它读到某个项目的 .mcp.json、hooks、CLAUDE.md。
    base::FilePath working_directory;
    base::EnvironmentMap environment;
    // 接着上次的会话。
    std::string session_id;
    // false：不把会话写到磁盘（无痕窗口，--no-session-persistence）。
    bool persists_session = true;
  };

  ClaudeCodeBackend(Config config, Delegate* delegate);
  ClaudeCodeBackend(const ClaudeCodeBackend&) = delete;
  ClaudeCodeBackend& operator=(const ClaudeCodeBackend&) = delete;
  // 结束进程，之后不再发事件。
  ~ClaudeCodeBackend() override;

  // 启动的参数。effort：思考程度（ClaudeCodeReasoningLevels），空的用它自己的默认。
  static std::vector<std::string> Arguments(const Config& config,
                                            const std::string& resume,
                                            const std::string& effort);
  const std::string& session_id() const { return session_id_; }
  const Config& config() const { return config_; }

  // Backend:
  bool running() const override;
  void Send(UserMessage message) override;
  void Stop() override;

 private:
  bool Launch(const std::string& effort);
  void HandleLine(std::string line);
  void HandleSystem(const base::DictValue& value);
  void HandleStreamEvent(const base::DictValue& event);
  void HandleAssistant(const base::DictValue& message);
  void HandleToolResults(const base::DictValue& message);
  void HandleResult(const base::DictValue& value);
  void HandleControlRequest(const base::DictValue& value);
  void Exited(int status, std::string errors);
  void Finish(StopReason reason,
              FailureKind failure = FailureKind::kNone,
              const std::string& message = std::string());

  Config config_;
  const raw_ptr<Delegate> delegate_;
  std::unique_ptr<LineProcess> process_;
  bool running_ = false;
  bool stopping_ = false;
  std::string session_id_;
  // 进程启动时的思考程度（--effort）：只能在启动时给，换了就重新启动、--resume 接着聊。
  std::string launched_effort_;
  // 已经一段段流过文字、思考的消息：完整的 assistant 消息到了不再重复显示。
  std::set<std::string> streamed_text_;
  std::set<std::string> streamed_thinking_;
  std::string current_message_;
  base::OneShotTimer escalation_;
  base::WeakPtrFactory<ClaudeCodeBackend> weak_factory_{this};
};

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_CLAUDE_CODE_BACKEND_H_
