// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/claude_code_backend.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "starry/browser/assistant/line_process.h"

namespace starry::assistant {

namespace {

// 等用户在面板里确认时工具调用不能超时：MCP 工具的超时放宽到 15 分钟。
constexpr base::FilePath::CharType kToolTimeout[] = FILE_PATH_LITERAL("900000");
// 停下：先按协议请它中断，不理就 SIGINT，再不理就结束进程。
constexpr base::TimeDelta kEscalation = base::Seconds(4);

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("{}");
}

std::string McpConfig(const BrowserMcpServer& server) {
  base::ListValue args;
  for (const std::string& arg : server.arguments) {
    args.Append(arg);
  }
  base::DictValue env;
  for (const auto& [key, value] : server.environment) {
    env.Set(key, value);
  }
  base::DictValue entry;
  entry.Set("type", "stdio");
  entry.Set("command", server.command);
  entry.Set("args", std::move(args));
  entry.Set("env", std::move(env));
  return Json(base::Value(base::DictValue().Set(
      "mcpServers",
      base::DictValue().Set(kBrowserMcpServerName, std::move(entry)))));
}

std::string Text(const base::DictValue& block, const char* key) {
  const std::string* text = block.FindString(key);
  return text ? *text : std::string();
}

}  // namespace

ClaudeCodeBackend::Config::Config() = default;
ClaudeCodeBackend::Config::Config(const Config&) = default;
ClaudeCodeBackend::Config& ClaudeCodeBackend::Config::operator=(const Config&) =
    default;
ClaudeCodeBackend::Config::~Config() = default;

ClaudeCodeBackend::ClaudeCodeBackend(Config config, Delegate* delegate)
    : config_(std::move(config)),
      delegate_(delegate),
      session_id_(config_.session_id) {}

bool ClaudeCodeBackend::running() const {
  return running_;
}

ClaudeCodeBackend::~ClaudeCodeBackend() {
  if (process_) {
    process_->CloseInput();
    process_->Terminate();
  }
}

// static
std::vector<std::string> ClaudeCodeBackend::Arguments(
    const Config& config,
    const std::string& resume,
    const std::string& effort) {
  std::vector<std::string> arguments = {
      "-p",
      "--input-format",
      "stream-json",
      "--output-format",
      "stream-json",
      "--verbose",
      "--include-partial-messages",
      "--mcp-config",
      config.server ? McpConfig(*config.server) : "{\"mcpServers\":{}}",
      "--strict-mcp-config",
      // 关掉它自带的所有工具（读写文件、Shell、联网……），只用浏览器的。
      "--tools",
      "",
      // 没有人回答它自己的权限询问：浏览器工具已经放行，确认在浏览器那边做。
      "--permission-prompts",
      "none",
      // 审核用的（没有工具）：换掉它自己的系统提示，只做审核。
      config.server ? "--append-system-prompt" : "--system-prompt",
      config.system_prompt,
  };
  if (config.server) {
    arguments.insert(
        arguments.end(),
        {"--allowedTools", std::string("mcp__") + kBrowserMcpServerName});
  }
  if (!config.server || !config.persists_session) {
    arguments.push_back("--no-session-persistence");
  }
  if (!config.model.empty()) {
    arguments.insert(arguments.end(), {"--model", config.model});
  }
  if (!effort.empty() &&
      std::ranges::find(ClaudeCodeReasoningLevels(), effort) !=
          ClaudeCodeReasoningLevels().end()) {
    arguments.insert(arguments.end(), {"--effort", effort});
  }
  if (!resume.empty()) {
    arguments.insert(arguments.end(), {"--resume", resume});
  }
  return arguments;
}

void ClaudeCodeBackend::Send(UserMessage message) {
  if (running_) {
    return;
  }
  running_ = true;
  stopping_ = false;
  const std::vector<std::string>& levels = ClaudeCodeReasoningLevels();
  const std::string effort =
      std::ranges::find(levels, message.reasoning) != levels.end()
          ? message.reasoning
          : std::string();
  if (process_ && effort != launched_effort_ && config_.persists_session &&
      config_.server) {
    // 思考程度只能在启动时给：结束这个进程（它的退出不再报给面板），用新的等级接着
    // 这个会话。不写磁盘的会话接不上，等级留到下个对话。
    weak_factory_.InvalidateWeakPtrs();
    process_->CloseInput();
    process_->Terminate();
    process_.reset();
  }
  if (!process_) {
    delegate_->OnStarting();
    if (!Launch(effort)) {
      return;
    }
  }
  base::ListValue content;
  content.Append(
      base::DictValue().Set("type", "text").Set("text", message.text));
  for (const agent::ToolImage& image : message.images) {
    content.Append(base::DictValue()
                       .Set("type", "image")
                       .Set("source", base::DictValue()
                                          .Set("type", "base64")
                                          .Set("media_type", image.mime_type)
                                          .Set("data", image.base64)));
  }
  base::DictValue line;
  line.Set("type", "user");
  line.Set(
      "message",
      base::DictValue().Set("role", "user").Set("content", std::move(content)));
  line.Set("parent_tool_use_id", base::Value());
  if (!session_id_.empty()) {
    line.Set("session_id", session_id_);
  }
  process_->Write(Json(base::Value(std::move(line))));
}

void ClaudeCodeBackend::Stop() {
  if (!running_ || !process_) {
    return;
  }
  stopping_ = true;
  const std::string id =
      base::ToLowerASCII(base::HexEncode(base::RandBytesAsVector(8)));
  process_->Write(Json(base::Value(
      base::DictValue()
          .Set("type", "control_request")
          .Set("request_id", "starry-" + id)
          .Set("request", base::DictValue().Set("subtype", "interrupt")))));
  escalation_.Start(
      FROM_HERE, kEscalation,
      base::BindOnce(
          [](base::WeakPtr<ClaudeCodeBackend> self) {
            if (!self || !self->running_ || !self->process_) {
              return;
            }
            self->process_->Interrupt();
            self->escalation_.Start(
                FROM_HERE, kEscalation,
                base::BindOnce(
                    [](base::WeakPtr<ClaudeCodeBackend> self) {
                      if (self && self->running_ && self->process_) {
                        self->process_->Terminate();
                      }
                    },
                    self));
          },
          weak_factory_.GetWeakPtr()));
}

bool ClaudeCodeBackend::Launch(const std::string& effort) {
  LineProcess::Options options;
  options.executable = config_.executable;
  options.arguments =
      Arguments(config_, config_.persists_session ? session_id_ : "", effort);
  launched_effort_ = effort;
  options.environment = config_.environment;
  options.environment[FILE_PATH_LITERAL("MCP_TOOL_TIMEOUT")] = kToolTimeout;
  options.directory = config_.working_directory;
  streamed_text_.clear();
  streamed_thinking_.clear();
  current_message_.clear();
  process_ = std::make_unique<LineProcess>(
      base::BindRepeating(&ClaudeCodeBackend::HandleLine,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&ClaudeCodeBackend::Exited, weak_factory_.GetWeakPtr()));
  process_->Start(
      std::move(options),
      base::BindOnce(
          [](base::WeakPtr<ClaudeCodeBackend> self, std::string error) {
            if (!self || error.empty()) {
              return;
            }
            self->process_.reset();
            self->Finish(StopReason::kFailed, FailureKind::kProcess, error);
          },
          weak_factory_.GetWeakPtr()));
  return true;
}

void ClaudeCodeBackend::HandleLine(std::string line) {
  std::optional<base::Value> value =
      base::JSONReader::Read(line, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return;
  }
  const base::DictValue& dict = value->GetDict();
  // 子智能体的消息（parent_tool_use_id 不是空的）不显示：工具都关了，本来也不该有。
  if (const base::Value* parent = dict.Find("parent_tool_use_id");
      parent && !parent->is_none()) {
    return;
  }
  const std::string type = Text(dict, "type");
  const base::DictValue empty;
  if (type == "system") {
    HandleSystem(dict);
  } else if (type == "stream_event") {
    const base::DictValue* event = dict.FindDict("event");
    HandleStreamEvent(event ? *event : empty);
  } else if (type == "assistant") {
    const base::DictValue* message = dict.FindDict("message");
    HandleAssistant(message ? *message : empty);
  } else if (type == "user") {
    const base::DictValue* message = dict.FindDict("message");
    HandleToolResults(message ? *message : empty);
  } else if (type == "result") {
    HandleResult(dict);
  } else if (type == "control_request") {
    HandleControlRequest(dict);
  }
}

void ClaudeCodeBackend::HandleSystem(const base::DictValue& value) {
  if (Text(value, "subtype") != "init") {
    return;
  }
  if (const std::string* id = value.FindString("session_id")) {
    session_id_ = *id;
    delegate_->OnSession(session_id_);
  }
  if (const std::string* model = value.FindString("model")) {
    delegate_->OnModel(*model);
  }
  // 审核用的本来就没有浏览器的工具。
  if (!config_.server) {
    return;
  }
  if (const base::ListValue* errors = value.FindList("mcp_server_errors")) {
    for (const base::Value& error : *errors) {
      if (error.is_dict() &&
          Text(error.GetDict(), "name") == kBrowserMcpServerName) {
        const std::string message = Text(error.GetDict(), "message");
        delegate_->OnToolsUnavailable(
            message.empty() ? "invalid MCP configuration" : message);
        return;
      }
    }
  }
  if (const base::ListValue* servers = value.FindList("mcp_servers")) {
    std::string status = "missing";
    for (const base::Value& server : *servers) {
      if (server.is_dict() &&
          Text(server.GetDict(), "name") == kBrowserMcpServerName) {
        status = Text(server.GetDict(), "status");
      }
    }
    if (status != "connected" && status != "pending") {
      delegate_->OnToolsUnavailable(status);
    }
  }
}

void ClaudeCodeBackend::HandleStreamEvent(const base::DictValue& event) {
  const std::string type = Text(event, "type");
  if (type == "message_start") {
    const base::DictValue* message = event.FindDict("message");
    current_message_ = message ? Text(*message, "id") : std::string();
    return;
  }
  if (type != "content_block_delta") {
    return;
  }
  const base::DictValue* delta = event.FindDict("delta");
  if (!delta) {
    return;
  }
  const std::string kind = Text(*delta, "type");
  if (kind == "text_delta") {
    const std::string text = Text(*delta, "text");
    if (!text.empty()) {
      streamed_text_.insert(current_message_);
      delegate_->OnTextDelta(text);
    }
  } else if (kind == "thinking_delta") {
    const std::string text = Text(*delta, "thinking");
    if (!text.empty()) {
      streamed_thinking_.insert(current_message_);
      delegate_->OnReasoningDelta(text);
    }
  }
}

void ClaudeCodeBackend::HandleAssistant(const base::DictValue& message) {
  const std::string id = Text(message, "id");
  const base::ListValue* content = message.FindList("content");
  if (!content) {
    return;
  }
  for (const base::Value& value : *content) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& block = value.GetDict();
    const std::string type = Text(block, "type");
    if (type == "text" && !streamed_text_.contains(id)) {
      const std::string text = Text(block, "text");
      if (!text.empty()) {
        delegate_->OnTextDelta(text);
      }
    } else if (type == "thinking" && !streamed_thinking_.contains(id)) {
      const std::string text = Text(block, "thinking");
      if (!text.empty()) {
        delegate_->OnReasoningDelta(text);
      }
    } else if (type == "tool_use") {
      const std::string tool_id = Text(block, "id");
      const std::string name = Text(block, "name");
      if (tool_id.empty() || name.empty()) {
        continue;
      }
      const base::DictValue* input = block.FindDict("input");
      delegate_->OnToolStarted(tool_id, BrowserToolName(name),
                               input ? input->Clone() : base::DictValue());
    }
  }
}

void ClaudeCodeBackend::HandleToolResults(const base::DictValue& message) {
  const base::ListValue* content = message.FindList("content");
  if (!content) {
    return;
  }
  for (const base::Value& value : *content) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& block = value.GetDict();
    if (Text(block, "type") != "tool_result") {
      continue;
    }
    agent::ToolResult result;
    if (const std::string* text = block.FindString("content")) {
      result.text = *text;
    } else if (const base::ListValue* parts = block.FindList("content")) {
      std::vector<std::string> texts;
      for (const base::Value& part : *parts) {
        if (part.is_dict() && Text(part.GetDict(), "type") == "text") {
          texts.push_back(Text(part.GetDict(), "text"));
        }
      }
      result.text = base::JoinString(texts, "\n");
    }
    result.is_error = block.FindBool("is_error").value_or(false);
    delegate_->OnToolFinished(Text(block, "tool_use_id"), result);
  }
}

void ClaudeCodeBackend::HandleResult(const base::DictValue& value) {
  if (const std::string* id = value.FindString("session_id");
      id && *id != session_id_) {
    session_id_ = *id;
    delegate_->OnSession(session_id_);
  }
  if (const base::DictValue* usage = value.FindDict("usage")) {
    Usage total;
    total.input_tokens = usage->FindInt("input_tokens").value_or(0);
    total.output_tokens = usage->FindInt("output_tokens").value_or(0);
    total.cost = value.FindDouble("total_cost_usd").value_or(0);
    delegate_->OnUsage(total);
  }
  const std::string subtype = Text(value, "subtype");
  if (subtype == "success" && !value.FindBool("is_error").value_or(false)) {
    Finish(StopReason::kCompleted);
    return;
  }
  if (subtype == "error_max_turns") {
    Finish(StopReason::kMaxSteps);
    return;
  }
  std::string message = Text(value, "result");
  if (message.empty()) {
    if (const base::ListValue* errors = value.FindList("errors")) {
      std::vector<std::string> lines;
      for (const base::Value& error : *errors) {
        if (error.is_string()) {
          lines.push_back(error.GetString());
        }
      }
      message = base::JoinString(lines, "\n");
    }
  }
  if (message.empty()) {
    message = subtype;
  }
  Finish(StopReason::kFailed,
         LooksLikeAuthentication(message) ? FailureKind::kAuthentication
                                          : FailureKind::kOther,
         message);
}

// 它来问权限：浏览器的工具已经放行，别的工具都关了，一律拒绝。
void ClaudeCodeBackend::HandleControlRequest(const base::DictValue& value) {
  const std::string request_id = Text(value, "request_id");
  if (request_id.empty() || !process_) {
    return;
  }
  const base::DictValue* request = value.FindDict("request");
  base::DictValue response;
  if (request && Text(*request, "subtype") == "can_use_tool") {
    response.Set("subtype", "success");
    response.Set("request_id", request_id);
    response.Set(
        "response",
        base::DictValue()
            .Set("behavior", "deny")
            .Set("message", "Only the browser tools are available here."));
  } else {
    response.Set("subtype", "error");
    response.Set("request_id", request_id);
    response.Set("error", "Not supported by Starry Browser.");
  }
  process_->Write(Json(base::Value(base::DictValue()
                                       .Set("type", "control_response")
                                       .Set("response", std::move(response)))));
}

void ClaudeCodeBackend::Exited(int status, std::string errors) {
  // 进程没了：下一条消息时重新启动，--resume 接着这个会话。
  process_.reset();
  escalation_.Stop();
  if (!running_) {
    return;
  }
  if (stopping_) {
    Finish(StopReason::kStopped);
    return;
  }
  const std::string message = errors.empty()
                                  ? "Claude Code exited with status " +
                                        base::NumberToString(status) + "."
                                  : errors;
  Finish(StopReason::kFailed,
         LooksLikeAuthentication(message) ? FailureKind::kAuthentication
                                          : FailureKind::kProcess,
         message);
}

void ClaudeCodeBackend::Finish(StopReason reason,
                               FailureKind failure,
                               const std::string& message) {
  if (!running_) {
    return;
  }
  running_ = false;
  escalation_.Stop();
  if (stopping_) {
    reason = StopReason::kStopped;
    failure = FailureKind::kNone;
  }
  delegate_->OnFinished(reason, failure, message);
}

}  // namespace starry::assistant
