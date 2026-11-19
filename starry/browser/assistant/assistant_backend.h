// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_ASSISTANT_BACKEND_H_
#define STARRY_BROWSER_ASSISTANT_ASSISTANT_BACKEND_H_

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "starry/browser/agent/browser_tools.h"

// 内置助手的后端：
// - OpenAI 兼容服务（ChatBackend）：浏览器自己跑智能体循环，工具在进程里直接调用
//   （Delegate::ExecuteTool）。
// - Claude Code（ClaudeCodeBackend）：本机的 claude -p 跑循环，它启动 `Starry --mcp`，
//   桥带着令牌连回来调用工具。
// - Codex（CodexBackend）：本机的 codex app-server，工具同上。
// 三种都转成同一套事件交给 Delegate，面板只认这套事件。
namespace starry::assistant {

enum class StopReason {
  kCompleted,
  // 用户停下的。
  kStopped,
  // 回答被截断（输出长度上限）。
  kTruncated,
  // 一轮里的步数到了上限。
  kMaxSteps,
  kFailed,
};

// 出错的种类（界面上说法不同）。
enum class FailureKind {
  kNone,
  // 还没配置好（没有 Key、没有选模型、找不到可执行文件）。
  kNotConfigured,
  // Key 不对、没有权限，或者本机智能体没登录。
  kAuthentication,
  kRateLimited,
  kNetwork,
  kHttp,
  kInvalidResponse,
  kRefused,
  // 本机智能体的进程没起来、或者中途退出了。
  kProcess,
  kOther,
};

struct Usage {
  int input_tokens = 0;
  int output_tokens = 0;
  double cost = 0;
};

// 思考程度：模型先想多久再回答。各家的写法不一样，等级统一用这几个词：
// none（不思考）、minimal、low、medium、high、xhigh、max，只能开关的服务用 on。
// 空的是默认：不发，用服务或模型自己的。
// claude --effort（Claude Code 2.1 的 --help）。
const std::vector<std::string>& ClaudeCodeReasoningLevels();
// Codex 还没告诉我们这个模型支持哪些（model/list，第一次发消息时取）时先给的。
const std::vector<std::string>& CodexFallbackReasoningLevels();

// 用户发的一条。
struct UserMessage {
  UserMessage();
  UserMessage(UserMessage&&);
  UserMessage& operator=(UserMessage&&);
  ~UserMessage();

  std::string text;
  std::vector<agent::ToolImage> images;
  // 思考程度，空的是默认。
  std::string reasoning;
};

class Backend {
 public:
  // 回调都在 UI 线程上。
  class Delegate {
   public:
    virtual void OnTextDelta(const std::string& text) = 0;
    virtual void OnReasoningDelta(const std::string& text) = 0;
    // name 是浏览器工具的名字（去掉了 mcp__starry__ 这类前缀）。
    virtual void OnToolStarted(const std::string& id,
                               const std::string& name,
                               const base::DictValue& arguments) = 0;
    virtual void OnToolFinished(const std::string& id,
                                const agent::ToolResult& result) = 0;
    virtual void OnUsage(const Usage& usage) {}
    // 重试、模型不收图片这类提示。
    virtual void OnStatus(const std::u16string& status) {}
    // 本机智能体：在启动；实际在用的模型；没连上浏览器的工具（只能回答问题）。
    virtual void OnStarting() {}
    virtual void OnModel(const std::string& model) {}
    virtual void OnToolsUnavailable(const std::string& detail) {}
    // 本机智能体的会话（Claude Code 的 session、Codex 的 thread）：存进聊天记录，接着聊时
    // 用它接上。
    virtual void OnSession(const std::string& id) {}
    // Codex 说了这个模型支持哪些思考程度（model/list）。
    virtual void OnReasoningLevels(const std::vector<std::string>& levels) {}
    // 后端自己要问用户（Codex 的 request_user_input）：回答经 Backend::Answer 交回去。
    virtual void OnQuestion(const std::string& id,
                            const agent::Question& question) {}
    // 这一轮结束。之后可以再 Send。
    virtual void OnFinished(StopReason reason,
                            FailureKind failure,
                            const std::string& message) = 0;
    // OpenAI 兼容服务：浏览器自己执行工具。本机智能体经 MCP 调用，不走这里。
    virtual void ExecuteTool(const std::string& name,
                             base::DictValue arguments,
                             agent::ToolCallback done) {}

   protected:
    virtual ~Delegate() = default;
  };

  virtual ~Backend() = default;

  // 正在跑一轮（发出去了、还没 OnFinished）。
  virtual bool running() const = 0;
  // 发一条消息、开始一轮。正在跑的时候不要调用。
  virtual void Send(UserMessage message) = 0;
  // 停下这一轮：之后会收到 OnFinished(kStopped)。
  virtual void Stop() = 0;
  // 回答 OnQuestion 问的；nullopt 是没有回答（停止了）。
  virtual void Answer(const std::string& id,
                      std::optional<std::string> answer) {}
};

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_ASSISTANT_BACKEND_H_
