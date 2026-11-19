// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_CHAT_BACKEND_H_
#define STARRY_BROWSER_ASSISTANT_CHAT_BACKEND_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "starry/browser/agent/browser_tools.h"
#include "starry/browser/assistant/assistant_backend.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
namespace mojom {
class URLResponseHead;
}
}  // namespace network

// OpenAI 兼容服务（Chat Completions，流式）的后端：浏览器自己跑智能体循环——发请求、
// 边收边显示、模型要调用工具就在进程里执行，把结果接在历史后面再发，直到模型不再调用
// 工具。
namespace starry::assistant {

// 服务怎么调思考程度（参数按各家文档）。
enum class ReasoningStyle {
  // 调不了：不显示、不发。
  kUnsupported,
  // reasoning_effort: "<等级>"：OpenAI 的写法，Ollama、LM Studio 和多数兼容服务也认。
  kEffort,
  // OpenRouter：reasoning: {effort: "<等级>"}，它再换成各家模型的写法。
  kOpenRouter,
  // 通义（百炼）、硅基流动：enable_thinking 开关，thinking_budget 限制思考的长度。
  kEnableThinking,
  // 智谱、Kimi、火山方舟：thinking: {type: "enabled" | "disabled"}，只能开关。
  kThinkingType,
  // DeepSeek：thinking.type 开关，再加 reasoning_effort（low、high、max）。
  kDeepSeek,
};

// 一家服务。内置的几家在 ProviderCatalog()；「自定义」的地址由用户填。
struct ChatProvider {
  ChatProvider();
  ChatProvider(const ChatProvider&);
  ChatProvider& operator=(const ChatProvider&);
  ~ChatProvider();

  std::string id;
  std::string name;
  // 到 /v1 这一级：后面接 /chat/completions、/models。
  std::string base_url;
  bool requires_key = true;
  bool is_local = false;
  std::string key_page;
  // 回答长度上限的字段（OpenAI 的推理模型要 max_completion_tokens）。
  std::string max_tokens_field;
  // 流式回答的最后附上用量（stream_options.include_usage）。
  bool stream_usage = true;
  // 带工具调用的回答把思考原样发回去（DeepSeek、Kimi 的思考模型要）。
  bool echo_reasoning = false;
  // 之前每一轮的回答都把思考发回去（DeepSeek 的思考模式带工具时）。
  bool echo_all_reasoning = false;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> suggested_models;
  // 自定义服务用 OpenAI 的写法、几个最常见的等级。kEffort、kOpenRouter 的等级在
  // effort_levels 里，别的是固定的（ReasoningLevels）。
  ReasoningStyle reasoning = ReasoningStyle::kEffort;
  std::vector<std::string> effort_levels = {"none", "low", "medium", "high"};
};

// 菜单里给用户选的思考程度（不含「默认」）；空的是调不了。
std::vector<std::string> ReasoningLevels(const ChatProvider& provider);
// 把等级写进请求体。这一家不认识的等级不写。
void ApplyReasoning(const ChatProvider& provider,
                    const std::string& level,
                    base::DictValue& body);

const std::vector<ChatProvider>& ProviderCatalog();
const ChatProvider* FindProvider(std::string_view id);
// 拼出 base_url 下面的地址；用户把完整的地址贴进来时去掉后面的部分。
GURL ProviderEndpoint(const std::string& base_url, std::string_view path);

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
};

// 历史里的一条消息。
struct ChatMessage {
  enum class Role { kSystem, kUser, kAssistant, kTool };
  Role role = Role::kUser;
  std::optional<std::string> content;
  std::vector<agent::ToolImage> images;
  std::vector<ToolCall> tool_calls;
  std::string tool_call_id;
  std::string reasoning;

  ChatMessage();
  ChatMessage(const ChatMessage&);
  ChatMessage& operator=(const ChatMessage&);
  ~ChatMessage();
  size_t Weight() const;
};

class ChatBackend : public Backend,
                    public network::SimpleURLLoaderStreamConsumer {
 public:
  using Delegate = Backend::Delegate;

  struct Config {
    Config();
    Config(const Config&);
    Config& operator=(const Config&);
    ~Config();

    ChatProvider provider;
    std::string api_key;
    std::string model;
    std::string system_prompt;
    base::ListValue tools;
    int max_steps = 40;
    // 历史超过这么多字就把旧的工具结果换成一句「已省略」。
    size_t history_budget = 192000;
  };

  ChatBackend(Config config,
              scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
              Delegate* delegate);
  ChatBackend(const ChatBackend&) = delete;
  ChatBackend& operator=(const ChatBackend&) = delete;
  ~ChatBackend() override;

  bool running() const override;
  const std::vector<ChatMessage>& history() const { return history_; }
  const Config& config() const { return config_; }
  // 换服务、模型（下一轮生效）。
  void SetConfig(Config config);
  // 从聊天记录打开的对话：前文（第一次 Send 之前）。
  void SetHistory(std::vector<ChatMessage> history);

  void Send(UserMessage message) override;
  void Stop() override;

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view data, base::OnceClosure resume) override;
  void OnComplete(bool success) override;
  void OnRetry(base::OnceClosure start_retry) override;

 private:
  // 发一次请求。
  void Request();
  void OnResponseStarted(const GURL& final_url,
                         const network::mojom::URLResponseHead& head);
  void HandleLine(std::string_view line);
  void HandleEvent(const std::string& data);
  void Consume(const base::DictValue& chunk);
  // 一次回答收完：有工具调用就去执行，否则这一轮结束。
  void ResponseDone();
  void RunTool(size_t index);
  void ToolDone(size_t index, agent::ToolResult result);
  void Fail(FailureKind kind, const std::string& message, bool retryable);
  void Finish(StopReason reason,
              FailureKind failure = FailureKind::kNone,
              const std::string& message = std::string());
  std::string RequestBody() const;
  void Compact();

  Config config_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const raw_ptr<Delegate> delegate_;

  std::vector<ChatMessage> history_;
  bool running_ = false;
  int steps_ = 0;
  // 这一轮的第一条（用户刚发的）消息：从它以后带工具调用的回答要把思考发回去。
  size_t turn_start_ = 0;
  bool images_disabled_ = false;
  // 这一轮的思考程度。
  std::string reasoning_level_;

  // 正在收的这一次回答。
  std::unique_ptr<network::SimpleURLLoader> loader_;
  int http_status_ = 0;
  std::string retry_after_;
  int attempt_ = 0;
  bool streamed_ = false;
  bool saw_event_ = false;
  std::string line_buffer_;
  std::vector<std::string> event_lines_;
  std::string error_body_;
  std::string content_;
  std::string reasoning_;
  std::string finish_reason_;
  std::string stream_error_;
  std::map<int, ToolCall> calls_;
  std::vector<ToolCall> pending_calls_;
  std::vector<agent::ToolImage> pending_images_;

  base::WeakPtrFactory<ChatBackend> weak_factory_{this};
};

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_CHAT_BACKEND_H_
