// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/chat_backend.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "starry/browser/l10n/l10n.h"

namespace starry::assistant {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("starry_assistant_chat", R"(
        semantics {
          sender: "Starry Browser AI assistant"
          description:
            "Sends the conversation with the built-in AI assistant to the "
            "chat-completions service the user configured."
          trigger: "The user sends a message to the AI assistant."
          data:
            "The conversation, page content the assistant read with its "
            "tools, and the title and URL of the current tab."
          destination: OTHER
          destination_other: "The service the user configured in settings."
        }
        policy {
          cookies_allowed: NO
          setting: "Settings > AI agent > Assistant."
          policy_exception_justification: "Not implemented."
        })");

constexpr char kOmitted[] =
    "[Earlier tool output removed to save space. Call the tool again if you "
    "need it.]";

const char* RoleName(ChatMessage::Role role) {
  switch (role) {
    case ChatMessage::Role::kSystem:
      return "system";
    case ChatMessage::Role::kUser:
      return "user";
    case ChatMessage::Role::kAssistant:
      return "assistant";
    case ChatMessage::Role::kTool:
      return "tool";
  }
}

bool IsCompleteEvent(std::string_view value) {
  if (value == "[DONE]") {
    return true;
  }
  if (value.empty() || (value[0] != '{' && value[0] != '[')) {
    return false;
  }
  return base::JSONReader::Read(value, base::JSON_PARSE_RFC).has_value();
}

}  // namespace

ChatProvider::ChatProvider() = default;
ChatProvider::ChatProvider(const ChatProvider&) = default;
ChatProvider& ChatProvider::operator=(const ChatProvider&) = default;
ChatProvider::~ChatProvider() = default;

ChatMessage::ChatMessage() = default;
ChatMessage::ChatMessage(const ChatMessage&) = default;
ChatMessage& ChatMessage::operator=(const ChatMessage&) = default;
ChatMessage::~ChatMessage() = default;

size_t ChatMessage::Weight() const {
  size_t weight = (content ? content->size() : 0) + reasoning.size() +
                  images.size() * 3000;
  for (const ToolCall& call : tool_calls) {
    weight += call.name.size() + call.arguments.size();
  }
  return weight;
}

ChatBackend::Config::Config() = default;
ChatBackend::Config::Config(const Config& other)
    : provider(other.provider),
      api_key(other.api_key),
      model(other.model),
      system_prompt(other.system_prompt),
      tools(other.tools.Clone()),
      max_steps(other.max_steps),
      history_budget(other.history_budget) {}
ChatBackend::Config& ChatBackend::Config::operator=(const Config& other) {
  provider = other.provider;
  api_key = other.api_key;
  model = other.model;
  system_prompt = other.system_prompt;
  tools = other.tools.Clone();
  max_steps = other.max_steps;
  history_budget = other.history_budget;
  return *this;
}
ChatBackend::Config::~Config() = default;

const std::vector<ChatProvider>& ProviderCatalog() {
  // 地址和写法按各家文档。
  static const base::NoDestructor<std::vector<ChatProvider>> catalog([] {
    std::vector<ChatProvider> list;
    ChatProvider openrouter;
    openrouter.id = "openrouter";
    openrouter.name = "OpenRouter";
    openrouter.base_url = "https://openrouter.ai/api/v1";
    openrouter.key_page = "https://openrouter.ai/keys";
    openrouter.headers = {{"X-Title", "Starry Browser"}};
    openrouter.reasoning = ReasoningStyle::kOpenRouter;
    openrouter.effort_levels = {"none", "low", "medium", "high", "xhigh"};
    list.push_back(openrouter);

    ChatProvider openai;
    openai.id = "openai";
    openai.name = "OpenAI";
    openai.base_url = "https://api.openai.com/v1";
    openai.key_page = "https://platform.openai.com/api-keys";
    openai.effort_levels = {"none", "low", "medium", "high", "xhigh"};
    list.push_back(openai);

    ChatProvider deepseek;
    deepseek.id = "deepseek";
    deepseek.name = "DeepSeek";
    deepseek.base_url = "https://api.deepseek.com/v1";
    deepseek.key_page = "https://platform.deepseek.com/api_keys";
    deepseek.echo_reasoning = true;
    deepseek.echo_all_reasoning = true;
    deepseek.reasoning = ReasoningStyle::kDeepSeek;
    deepseek.suggested_models = {"deepseek-chat", "deepseek-reasoner"};
    list.push_back(deepseek);

    ChatProvider dashscope;
    dashscope.id = "dashscope";
    dashscope.name = "Alibaba Cloud Model Studio";
    dashscope.base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1";
    dashscope.key_page =
        "https://bailian.console.aliyun.com/?tab=model#/api-key";
    dashscope.echo_reasoning = true;
    dashscope.reasoning = ReasoningStyle::kEnableThinking;
    dashscope.suggested_models = {"qwen-max", "qwen-plus"};
    list.push_back(dashscope);

    ChatProvider moonshot;
    moonshot.id = "moonshot";
    moonshot.name = "Moonshot AI";
    moonshot.base_url = "https://api.moonshot.cn/v1";
    moonshot.key_page = "https://platform.moonshot.cn/console/api-keys";
    moonshot.echo_reasoning = true;
    moonshot.reasoning = ReasoningStyle::kThinkingType;
    list.push_back(moonshot);

    ChatProvider zhipu;
    zhipu.id = "zhipu";
    zhipu.name = "Zhipu AI";
    zhipu.base_url = "https://open.bigmodel.cn/api/paas/v4";
    zhipu.key_page = "https://bigmodel.cn/usercenter/proj-mgmt/apikeys";
    zhipu.stream_usage = false;
    zhipu.reasoning = ReasoningStyle::kThinkingType;
    list.push_back(zhipu);

    ChatProvider siliconflow;
    siliconflow.id = "siliconflow";
    siliconflow.name = "SiliconFlow";
    siliconflow.base_url = "https://api.siliconflow.cn/v1";
    siliconflow.key_page = "https://cloud.siliconflow.cn/account/ak";
    siliconflow.echo_reasoning = true;
    siliconflow.reasoning = ReasoningStyle::kEnableThinking;
    list.push_back(siliconflow);

    ChatProvider volcengine;
    volcengine.id = "volcengine";
    volcengine.name = "Volcengine Ark";
    volcengine.base_url = "https://ark.cn-beijing.volces.com/api/v3";
    volcengine.key_page =
        "https://console.volcengine.com/ark/region:ark+cn-beijing/apiKey";
    volcengine.reasoning = ReasoningStyle::kThinkingType;
    list.push_back(volcengine);

    ChatProvider ollama;
    ollama.id = "ollama";
    ollama.name = "Ollama";
    ollama.base_url = "http://localhost:11434/v1";
    ollama.requires_key = false;
    ollama.is_local = true;
    ollama.stream_usage = false;
    list.push_back(ollama);

    ChatProvider lmstudio;
    lmstudio.id = "lmstudio";
    lmstudio.name = "LM Studio";
    lmstudio.base_url = "http://localhost:1234/v1";
    lmstudio.requires_key = false;
    lmstudio.is_local = true;
    lmstudio.effort_levels = {"low", "medium", "high"};
    list.push_back(lmstudio);
    return list;
  }());
  return *catalog;
}

std::vector<std::string> ReasoningLevels(const ChatProvider& provider) {
  switch (provider.reasoning) {
    case ReasoningStyle::kUnsupported:
      return {};
    case ReasoningStyle::kEffort:
    case ReasoningStyle::kOpenRouter:
      return provider.effort_levels;
    case ReasoningStyle::kEnableThinking:
      return {"none", "low", "medium", "high"};
    case ReasoningStyle::kThinkingType:
      return {"none", "on"};
    case ReasoningStyle::kDeepSeek:
      return {"none", "low", "high", "max"};
  }
  return {};
}

void ApplyReasoning(const ChatProvider& provider,
                    const std::string& level,
                    base::DictValue& body) {
  const std::vector<std::string> levels = ReasoningLevels(provider);
  if (level.empty() || std::ranges::find(levels, level) == levels.end()) {
    return;
  }
  switch (provider.reasoning) {
    case ReasoningStyle::kUnsupported:
      break;
    case ReasoningStyle::kEffort:
      body.Set("reasoning_effort", level);
      break;
    case ReasoningStyle::kOpenRouter:
      body.Set("reasoning", base::DictValue().Set("effort", level));
      break;
    case ReasoningStyle::kEnableThinking:
      // 低、中两档用 thinking_budget 限制长度，高不限（各模型的上限不一样，
      // 不发就是上限）。
      body.Set("enable_thinking", level != "none");
      if (level == "low") {
        body.Set("thinking_budget", 4096);
      } else if (level == "medium") {
        body.Set("thinking_budget", 16384);
      }
      break;
    case ReasoningStyle::kThinkingType:
      body.Set("thinking",
               base::DictValue().Set("type",
                                     level == "none" ? "disabled" : "enabled"));
      break;
    case ReasoningStyle::kDeepSeek:
      body.Set("thinking",
               base::DictValue().Set("type",
                                     level == "none" ? "disabled" : "enabled"));
      if (level != "none") {
        body.Set("reasoning_effort", level);
      }
      break;
  }
}

const ChatProvider* FindProvider(std::string_view id) {
  for (const ChatProvider& provider : ProviderCatalog()) {
    if (provider.id == id) {
      return &provider;
    }
  }
  return nullptr;
}

GURL ProviderEndpoint(const std::string& base_url, std::string_view path) {
  std::string text(base::TrimWhitespaceASCII(base_url, base::TRIM_ALL));
  while (!text.empty() && text.back() == '/') {
    text.pop_back();
  }
  for (std::string_view suffix : {"/chat/completions", "/models"}) {
    if (base::EndsWith(text, suffix)) {
      text.resize(text.size() - suffix.size());
    }
  }
  const GURL url(base::StrCat({text, "/", path}));
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !url.has_host()) {
    return GURL();
  }
  return url;
}

ChatBackend::ChatBackend(
    Config config,
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    Delegate* delegate)
    : config_(std::move(config)),
      loader_factory_(std::move(loader_factory)),
      delegate_(delegate) {}

ChatBackend::~ChatBackend() = default;

bool ChatBackend::running() const {
  return running_;
}

void ChatBackend::SetConfig(Config config) {
  config_ = std::move(config);
}

void ChatBackend::SetHistory(std::vector<ChatMessage> history) {
  if (!running_ && history_.empty()) {
    history_ = std::move(history);
  }
}

void ChatBackend::Send(UserMessage user) {
  if (running_) {
    return;
  }
  ChatMessage message;
  message.role = ChatMessage::Role::kUser;
  message.content = std::move(user.text);
  message.images = std::move(user.images);
  reasoning_level_ = std::move(user.reasoning);
  history_.push_back(std::move(message));
  turn_start_ = history_.size() - 1;
  running_ = true;
  steps_ = 0;
  attempt_ = 0;
  if (ProviderEndpoint(config_.provider.base_url, "chat/completions")
          .is_empty()) {
    Finish(StopReason::kFailed, FailureKind::kNotConfigured,
           "The service address is not a valid URL.");
    return;
  }
  if (config_.model.empty()) {
    Finish(StopReason::kFailed, FailureKind::kNotConfigured,
           "No model is selected.");
    return;
  }
  if (config_.provider.requires_key && config_.api_key.empty()) {
    Finish(StopReason::kFailed, FailureKind::kNotConfigured,
           base::StrCat({"No API key is set for ", config_.provider.name, "."}));
    return;
  }
  Request();
}

void ChatBackend::Stop() {
  if (!running_) {
    return;
  }
  loader_.reset();
  // 工具正在做：它的结果不再要了。每个工具调用都要有结果，不然下一次请求会被拒绝。
  weak_factory_.InvalidateWeakPtrs();
  if (!history_.empty() &&
      history_.back().role != ChatMessage::Role::kUser) {
    for (const ToolCall& call : pending_calls_) {
      const bool answered = std::any_of(
          history_.begin(), history_.end(), [&](const ChatMessage& message) {
            return message.role == ChatMessage::Role::kTool &&
                   message.tool_call_id == call.id;
          });
      if (!answered) {
        ChatMessage cancelled;
        cancelled.role = ChatMessage::Role::kTool;
        cancelled.tool_call_id = call.id;
        cancelled.content = "Cancelled: the user stopped this turn.";
        history_.push_back(std::move(cancelled));
      }
    }
  }
  pending_calls_.clear();
  Finish(StopReason::kStopped);
}

void ChatBackend::Compact() {
  size_t total = 0;
  for (const ChatMessage& message : history_) {
    total += message.Weight();
  }
  if (total <= config_.history_budget) {
    return;
  }
  // 最近 4 个工具结果之前的：换成一句说明，图片去掉。
  std::vector<size_t> tool_indices;
  for (size_t i = 0; i < history_.size(); ++i) {
    if (history_[i].role == ChatMessage::Role::kTool) {
      tool_indices.push_back(i);
    }
  }
  const size_t keep_from = tool_indices.size() > 4
                               ? tool_indices[tool_indices.size() - 4]
                               : (tool_indices.empty() ? history_.size()
                                                       : tool_indices.front());
  for (size_t i = 0; i < keep_from && total > config_.history_budget / 2; ++i) {
    ChatMessage& message = history_[i];
    if (message.role == ChatMessage::Role::kTool && message.content &&
        message.content->size() > sizeof(kOmitted)) {
      total -= message.content->size() - sizeof(kOmitted);
      message.content = kOmitted;
    } else if (message.role == ChatMessage::Role::kUser &&
               !message.images.empty()) {
      total -= message.images.size() * 3000;
      message.images.clear();
      message.content =
          message.content.value_or("") + "\n[Image removed to save space.]";
    }
  }
}

std::string ChatBackend::RequestBody() const {
  base::ListValue messages;
  messages.Append(base::DictValue()
                      .Set("role", "system")
                      .Set("content", config_.system_prompt));
  for (size_t i = 0; i < history_.size(); ++i) {
    const ChatMessage& message = history_[i];
    base::DictValue object;
    object.Set("role", RoleName(message.role));
    switch (message.role) {
      case ChatMessage::Role::kUser:
        if (!message.images.empty()) {
          base::ListValue parts;
          if (message.content && !message.content->empty()) {
            parts.Append(base::DictValue()
                             .Set("type", "text")
                             .Set("text", *message.content));
          }
          for (const agent::ToolImage& image : message.images) {
            if (images_disabled_) {
              parts.Append(base::DictValue().Set("type", "text").Set(
                  "text",
                  "[An image was attached here, but this model cannot read "
                  "images.]"));
            } else {
              parts.Append(base::DictValue()
                               .Set("type", "image_url")
                               .Set("image_url",
                                    base::DictValue().Set(
                                        "url", base::StrCat({"data:",
                                                             image.mime_type,
                                                             ";base64,",
                                                             image.base64}))));
            }
          }
          object.Set("content", std::move(parts));
        } else {
          object.Set("content", message.content.value_or(""));
        }
        break;
      case ChatMessage::Role::kAssistant: {
        if (message.content) {
          object.Set("content", *message.content);
        } else if (message.tool_calls.empty()) {
          object.Set("content", "");
        } else {
          object.Set("content", base::Value());
        }
        if (!message.tool_calls.empty()) {
          base::ListValue calls;
          for (const ToolCall& call : message.tool_calls) {
            calls.Append(
                base::DictValue()
                    .Set("id", call.id)
                    .Set("type", "function")
                    .Set("function",
                         base::DictValue()
                             .Set("name", call.name)
                             .Set("arguments", call.arguments.empty()
                                                   ? std::string("{}")
                                                   : call.arguments)));
          }
          object.Set("tool_calls", std::move(calls));
        }
        const bool echo =
            config_.provider.echo_all_reasoning ||
            (config_.provider.echo_reasoning && !message.tool_calls.empty() &&
             i >= turn_start_);
        if (echo && !message.reasoning.empty()) {
          object.Set("reasoning_content", message.reasoning);
        }
        break;
      }
      case ChatMessage::Role::kTool:
        object.Set("tool_call_id", message.tool_call_id);
        object.Set("content", message.content.value_or(""));
        break;
      case ChatMessage::Role::kSystem:
        object.Set("content", message.content.value_or(""));
        break;
    }
    messages.Append(std::move(object));
  }
  base::DictValue body;
  body.Set("model", config_.model);
  body.Set("stream", true);
  body.Set("messages", std::move(messages));
  if (!config_.tools.empty()) {
    body.Set("tools", config_.tools.Clone());
    body.Set("tool_choice", "auto");
  }
  if (config_.provider.stream_usage) {
    body.Set("stream_options", base::DictValue().Set("include_usage", true));
  }
  if (config_.provider.id == "openrouter") {
    body.Set("usage", base::DictValue().Set("include", true));
  }
  ApplyReasoning(config_.provider, reasoning_level_, body);
  std::string json;
  base::JSONWriter::Write(body, &json);
  return json;
}

void ChatBackend::Request() {
  if (!running_) {
    return;
  }
  Compact();
  const GURL url =
      ProviderEndpoint(config_.provider.base_url, "chat/completions");
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader("Content-Type", "application/json");
  request->headers.SetHeader("Accept", "text/event-stream");
  if (!config_.api_key.empty()) {
    request->headers.SetHeader("Authorization",
                               base::StrCat({"Bearer ", config_.api_key}));
  }
  for (const auto& [name, value] : config_.provider.headers) {
    request->headers.SetHeader(name, value);
  }
  http_status_ = 0;
  retry_after_.clear();
  streamed_ = false;
  saw_event_ = false;
  line_buffer_.clear();
  event_lines_.clear();
  error_body_.clear();
  content_.clear();
  reasoning_.clear();
  finish_reason_.clear();
  stream_error_.clear();
  calls_.clear();
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->AttachStringForUpload(RequestBody(), "application/json");
  loader_->SetAllowHttpErrorResults(true);
  // 回答可能要等很久才开始（思考模型）。
  loader_->SetTimeoutDuration(base::Seconds(300));
  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &ChatBackend::OnResponseStarted, weak_factory_.GetWeakPtr()));
  loader_->DownloadAsStream(loader_factory_.get(), this);
}

void ChatBackend::OnResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  if (head.headers) {
    http_status_ = head.headers->response_code();
    retry_after_ = head.headers->GetNormalizedHeader("Retry-After").value_or("");
  }
}

void ChatBackend::OnDataReceived(std::string_view data,
                                 base::OnceClosure resume) {
  const bool ok = http_status_ == 0 || (http_status_ >= 200 && http_status_ < 300);
  if (!ok) {
    if (error_body_.size() < 65536) {
      error_body_.append(data.substr(0, 65536 - error_body_.size()));
    }
    std::move(resume).Run();
    return;
  }
  base::WeakPtr<ChatBackend> weak = weak_factory_.GetWeakPtr();
  line_buffer_.append(data);
  size_t start = 0;
  while (true) {
    const size_t end = line_buffer_.find('\n', start);
    if (end == std::string::npos) {
      break;
    }
    HandleLine(std::string_view(line_buffer_).substr(start, end - start));
    if (!weak) {
      return;  // 停下了（Finish 里删掉了 loader）。
    }
    start = end + 1;
  }
  line_buffer_.erase(0, start);
  if (!saw_event_ && error_body_.size() < 1 << 20) {
    // 不支持流式的服务直接回整个 JSON：先留着原文。
    error_body_.append(data);
  }
  if (loader_) {
    std::move(resume).Run();
  }
}

void ChatBackend::HandleLine(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    if (!event_lines_.empty()) {
      const std::string data = base::JoinString(event_lines_, "\n");
      event_lines_.clear();
      HandleEvent(data);
    }
    return;
  }
  if (line[0] == ':' || !base::StartsWith(line, "data:")) {
    return;
  }
  std::string_view value = line.substr(5);
  if (!value.empty() && value[0] == ' ') {
    value.remove_prefix(1);
  }
  if (event_lines_.empty() && IsCompleteEvent(value)) {
    HandleEvent(std::string(value));
    return;
  }
  event_lines_.emplace_back(value);
  // 没有空行分隔的服务：拼起来已经是完整的 JSON 就不等空行了。
  const std::string joined = base::JoinString(event_lines_, "\n");
  if (IsCompleteEvent(joined)) {
    event_lines_.clear();
    HandleEvent(joined);
  }
}

void ChatBackend::HandleEvent(const std::string& data) {
  saw_event_ = true;
  if (data == "[DONE]") {
    return;
  }
  std::optional<base::Value> chunk =
      base::JSONReader::Read(data, base::JSON_PARSE_RFC);
  if (chunk && chunk->is_dict()) {
    Consume(chunk->GetDict());
  }
}

void ChatBackend::Consume(const base::DictValue& chunk) {
  if (const base::Value* error = chunk.Find("error")) {
    const std::string* message =
        error->is_dict() ? error->GetDict().FindString("message") : nullptr;
    stream_error_ = message ? *message
                            : (error->is_string() ? error->GetString()
                                                  : std::string("error"));
    return;
  }
  if (const base::DictValue* usage = chunk.FindDict("usage")) {
    Usage value;
    value.input_tokens = usage->FindInt("prompt_tokens").value_or(0);
    value.output_tokens = usage->FindInt("completion_tokens").value_or(0);
    value.cost = usage->FindDouble("cost").value_or(0);
    delegate_->OnUsage(value);
  }
  const base::ListValue* choices = chunk.FindList("choices");
  if (!choices || choices->empty() || !(*choices)[0].is_dict()) {
    return;
  }
  const base::DictValue& choice = (*choices)[0].GetDict();
  if (const std::string* reason = choice.FindString("finish_reason")) {
    finish_reason_ = *reason;
  }
  const base::DictValue* piece = choice.FindDict("delta");
  if (!piece) {
    piece = choice.FindDict("message");
  }
  if (!piece) {
    return;
  }
  if (const std::string* text = piece->FindString("content");
      text && !text->empty()) {
    content_ += *text;
    streamed_ = true;
    delegate_->OnTextDelta(*text);
  }
  // DeepSeek、通义、Kimi、vLLM 用 reasoning_content，OpenRouter 用 reasoning。
  const std::string* thought = piece->FindString("reasoning_content");
  if (!thought) {
    thought = piece->FindString("reasoning");
  }
  if (thought && !thought->empty()) {
    reasoning_ += *thought;
    streamed_ = true;
    delegate_->OnReasoningDelta(*thought);
  }
  if (const base::ListValue* calls = piece->FindList("tool_calls")) {
    int position = 0;
    for (const base::Value& entry : *calls) {
      if (!entry.is_dict()) {
        continue;
      }
      const base::DictValue& call = entry.GetDict();
      const int index = call.FindInt("index").value_or(position++);
      ToolCall& current = calls_[index];
      if (const std::string* id = call.FindString("id"); id && !id->empty()) {
        current.id = *id;
      }
      if (const base::DictValue* function = call.FindDict("function")) {
        // 名字只在第一块里；有的服务每一块都带完整的名字，不要拼重。
        if (const std::string* name = function->FindString("name");
            name && !name->empty() && current.name.empty()) {
          current.name = *name;
        }
        if (const std::string* arguments = function->FindString("arguments")) {
          current.arguments += *arguments;
        }
      }
      streamed_ = true;
    }
  }
}

void ChatBackend::OnComplete(bool success) {
  const int net_error = loader_ ? loader_->NetError() : net::ERR_ABORTED;
  const bool http_ok = http_status_ >= 200 && http_status_ < 300;
  if (http_status_ == 0 && !success) {
    Fail(FailureKind::kNetwork,
         base::StrCat({net::ErrorToString(net_error),
                       config_.provider.is_local
                           ? " Is the local server running?"
                           : ""}),
         /*retryable=*/true);
    return;
  }
  if (!http_ok) {
    std::optional<base::Value> value =
        base::JSONReader::Read(error_body_, base::JSON_PARSE_RFC);
    std::string message;
    if (value && value->is_dict()) {
      const base::DictValue& dict = value->GetDict();
      const base::DictValue* error = dict.FindDict("error");
      const std::string* text =
          error ? error->FindString("message") : dict.FindString("error");
      if (!text) {
        text = dict.FindString("message");
      }
      if (!text) {
        text = dict.FindString("detail");
      }
      if (text) {
        message = *text;
      }
    }
    if (message.empty()) {
      message = std::string(base::TrimWhitespaceASCII(
          error_body_.substr(0, 500), base::TRIM_ALL));
    }
    if (message.empty()) {
      message = base::StrCat({"HTTP ", base::NumberToString(http_status_)});
    }
    // 模型不收图片：改成文字说明再发一次，之后不再发图片。
    const std::string lower = base::ToLowerASCII(message);
    const bool has_images = std::any_of(
        history_.begin(), history_.end(),
        [](const ChatMessage& message) { return !message.images.empty(); });
    if (http_status_ >= 400 && http_status_ < 500 && !images_disabled_ &&
        has_images &&
        (lower.find("image") != std::string::npos ||
         lower.find("vision") != std::string::npos ||
         lower.find("multimodal") != std::string::npos ||
         lower.find("modalit") != std::string::npos)) {
      images_disabled_ = true;
      delegate_->OnStatus(L("assistant.notice.imagesDisabled"));
      Request();
      return;
    }
    if (http_status_ == 401 || http_status_ == 403) {
      Fail(FailureKind::kAuthentication, message, false);
    } else if (http_status_ == 429) {
      Fail(FailureKind::kRateLimited, message, true);
    } else {
      Fail(FailureKind::kHttp, message,
           http_status_ >= 500 || http_status_ == 408);
    }
    return;
  }
  if (!line_buffer_.empty()) {
    const std::string rest = std::move(line_buffer_);
    line_buffer_.clear();
    HandleLine(rest);
  }
  if (!event_lines_.empty()) {
    const std::string data = base::JoinString(event_lines_, "\n");
    event_lines_.clear();
    HandleEvent(data);
  }
  if (!saw_event_) {
    // 不支持流式的服务回的整个 JSON。
    HandleEvent(error_body_);
  }
  if (!stream_error_.empty() && content_.empty() && calls_.empty()) {
    Fail(FailureKind::kOther, stream_error_, false);
    return;
  }
  if (!success && !streamed_) {
    Fail(FailureKind::kNetwork, net::ErrorToString(net_error), true);
    return;
  }
  if (content_.empty() && calls_.empty() && reasoning_.empty()) {
    Fail(FailureKind::kInvalidResponse,
         "The service returned an empty response.", false);
    return;
  }
  ResponseDone();
}

void ChatBackend::OnRetry(base::OnceClosure start_retry) {
  std::move(start_retry).Run();
}

void ChatBackend::Fail(FailureKind kind,
                       const std::string& message,
                       bool retryable) {
  loader_.reset();
  if (retryable && !streamed_ && attempt_ < 3) {
    attempt_++;
    double delay = 0;
    if (!base::StringToDouble(retry_after_, &delay) || delay <= 0) {
      delay = std::min(20.0, static_cast<double>(1 << (attempt_ - 1)));
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ChatBackend::Request, weak_factory_.GetWeakPtr()),
        base::Seconds(delay));
    return;
  }
  Finish(StopReason::kFailed, kind, message);
}

void ChatBackend::ResponseDone() {
  loader_.reset();
  attempt_ = 0;
  steps_++;
  ChatMessage message;
  message.role = ChatMessage::Role::kAssistant;
  if (!content_.empty()) {
    message.content = content_;
  }
  message.reasoning = reasoning_;
  for (auto& [index, call] : calls_) {
    if (call.name.empty()) {
      continue;
    }
    if (call.id.empty()) {
      call.id = base::StrCat({"call_", base::NumberToString(index)});
    }
    message.tool_calls.push_back(call);
  }
  pending_calls_ = message.tool_calls;
  history_.push_back(std::move(message));
  if (pending_calls_.empty()) {
    if (finish_reason_ == "length") {
      Finish(StopReason::kTruncated);
    } else if (finish_reason_ == "content_filter") {
      Finish(StopReason::kFailed, FailureKind::kRefused,
             "The service filtered the response.");
    } else {
      Finish(StopReason::kCompleted);
    }
    return;
  }
  pending_images_.clear();
  RunTool(0);
}

void ChatBackend::RunTool(size_t index) {
  if (!running_) {
    return;
  }
  if (index >= pending_calls_.size()) {
    pending_calls_.clear();
    if (!pending_images_.empty()) {
      ChatMessage shot;
      shot.role = ChatMessage::Role::kUser;
      shot.content = "Screenshot returned by the tool call above.";
      shot.images = std::move(pending_images_);
      pending_images_.clear();
      history_.push_back(std::move(shot));
    }
    if (steps_ >= config_.max_steps) {
      Finish(StopReason::kMaxSteps);
      return;
    }
    Request();
    return;
  }
  const ToolCall& call = pending_calls_[index];
  std::string trimmed(base::TrimWhitespaceASCII(call.arguments, base::TRIM_ALL));
  base::DictValue arguments;
  bool valid = true;
  if (!trimmed.empty() && trimmed != "null") {
    std::optional<base::Value> parsed =
        base::JSONReader::Read(trimmed, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) {
      arguments = std::move(parsed->GetDict());
    } else {
      valid = false;
    }
  }
  delegate_->OnToolStarted(call.id, call.name, arguments);
  if (!valid) {
    ToolDone(index,
             agent::ToolResult{
                 base::StrCat({"The arguments for ", call.name,
                               " were not a valid JSON object. Call the tool "
                               "again with valid JSON."}),
                 true});
    return;
  }
  delegate_->ExecuteTool(call.name, std::move(arguments),
                         base::BindOnce(&ChatBackend::ToolDone,
                                        weak_factory_.GetWeakPtr(), index));
}

void ChatBackend::ToolDone(size_t index, agent::ToolResult result) {
  if (!running_ || index >= pending_calls_.size()) {
    return;
  }
  std::string text = result.text;
  if (!result.images.empty()) {
    if (images_disabled_) {
      text += "\n(The screenshot is not shown: this model cannot read images.)";
    } else {
      text += "\n(The screenshot follows in the next message.)";
      for (agent::ToolImage& image : result.images) {
        pending_images_.push_back(image);
      }
    }
  }
  ChatMessage message;
  message.role = ChatMessage::Role::kTool;
  message.tool_call_id = pending_calls_[index].id;
  message.content = result.is_error ? "Error: " + text : text;
  history_.push_back(std::move(message));
  agent::ToolResult shown = result;
  shown.text = text;
  delegate_->OnToolFinished(pending_calls_[index].id, shown);
  RunTool(index + 1);
}

void ChatBackend::Finish(StopReason reason,
                         FailureKind failure,
                         const std::string& message) {
  loader_.reset();
  running_ = false;
  delegate_->OnFinished(reason, failure, message);
}

}  // namespace starry::assistant
