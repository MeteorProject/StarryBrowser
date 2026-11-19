// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/codex_backend.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "starry/browser/assistant/line_process.h"

namespace starry::assistant {

namespace {

// 停下时 turn/interrupt 之后最多等这么久，再结束进程。
constexpr base::TimeDelta kStopTimeout = base::Seconds(6);
constexpr size_t kMaxOptions = 4;

// 接在系统提示后面：codex 0.166 的模型用 code mode，MCP 工具不直接给模型，只能在 exec 的
// JavaScript 里调用，而且是「延迟」的（exec 的说明里不列出来）。告诉它名字。
constexpr char kToolNote[] =
    "\n\n# Calling the browser tools from Codex\n\n"
    "The browser tools come from the MCP server named `starry`. If you call "
    "tools through the `exec` JavaScript tool, call them there as `await "
    "tools.mcp__starry__<tool name>({...})`, for example `text(await "
    "tools.mcp__starry__browser_snapshot({}))`. They may be missing from the "
    "`exec` description; call them by these names anyway. Your shell, file "
    "and web tools are turned off here, and the sandbox is read-only.";

std::string TomlString(const std::string& text) {
  std::string escaped = "\"";
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        if (c < 0x20 || c == 0x7F) {
          escaped += base::StringPrintf("\\u%04X", c);
        } else {
          escaped += static_cast<char>(c);
        }
    }
  }
  return escaped + "\"";
}

std::string TomlKey(const std::string& key) {
  const bool bare = !key.empty() && std::ranges::all_of(key, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-';
  });
  return bare ? key : TomlString(key);
}

std::string Json(const base::DictValue& value) {
  return base::WriteJson(value).value_or("{}");
}

std::string Text(const base::DictValue& dict, const char* key) {
  const std::string* text = dict.FindString(key);
  return text ? *text : std::string();
}

FailureKind KindFor(const std::string& message) {
  return LooksLikeAuthentication(message) ? FailureKind::kAuthentication
                                          : FailureKind::kOther;
}

}  // namespace

CodexBackend::Config::Config() = default;
CodexBackend::Config::Config(const Config&) = default;
CodexBackend::Config& CodexBackend::Config::operator=(const Config&) = default;
CodexBackend::Config::~Config() = default;
CodexBackend::Message::Message() = default;
CodexBackend::Message::Message(Message&&) = default;
CodexBackend::Message& CodexBackend::Message::operator=(Message&&) = default;
CodexBackend::Message::~Message() = default;
CodexBackend::PendingInput::PendingInput() = default;
CodexBackend::PendingInput::PendingInput(PendingInput&&) = default;
CodexBackend::PendingInput& CodexBackend::PendingInput::operator=(
    PendingInput&&) = default;
CodexBackend::PendingInput::~PendingInput() = default;

// static
const std::vector<std::string>& CodexBackend::UnwantedFeatures() {
  static const base::NoDestructor<std::vector<std::string>> features(
      std::vector<std::string>{
          "shell_tool", "unified_exec", "view_image", "image_generation",
          "browser_use", "browser_use_external", "in_app_browser",
          "computer_use", "apps", "plugins", "skill_search", "tool_suggest",
          "sleep_tool", "multi_agent", "multi_agent_v2", "memories", "goals"});
  return *features;
}

// static
std::vector<std::string> CodexBackend::Arguments(const Config& config) {
  std::vector<std::string> arguments = {"app-server"};
  for (const std::string& feature : config.disabled_features) {
    arguments.insert(arguments.end(), {"--disable", feature});
  }
  arguments.insert(arguments.end(), {"-c", "web_search=\"disabled\"", "-c",
                                     "include_apply_patch_tool=false"});
  for (const std::string& name : config.disabled_servers) {
    if (name != kBrowserMcpServerName) {
      arguments.insert(arguments.end(),
                       {"-c", base::StrCat({"mcp_servers.", TomlKey(name),
                                            ".enabled=false"})});
    }
  }
  const std::string prefix =
      base::StrCat({"mcp_servers.", kBrowserMcpServerName});
  if (!config.server) {
    arguments.insert(arguments.end(), {"-c", prefix + ".enabled=false"});
    return arguments;
  }
  std::vector<std::string> args;
  for (const std::string& arg : config.server->arguments) {
    args.push_back(TomlString(arg));
  }
  std::vector<std::string> env;
  for (const auto& [key, value] : config.server->environment) {
    env.push_back(base::StrCat({TomlKey(key), " = ", TomlString(value)}));
  }
  arguments.insert(
      arguments.end(),
      {
          "-c",
          base::StrCat(
              {prefix, ".command=", TomlString(config.server->command)}),
          "-c",
          base::StrCat({prefix, ".args=[", base::JoinString(args, ", "), "]"}),
          "-c",
          base::StrCat({prefix, ".env={", base::JoinString(env, ", "), "}"}),
          "-c",
          prefix + ".enabled=true",
          // 它默认按工具的标注问用户要不要调用 MCP 工具，approvalPolicy 是 never 时问不了
          // 就直接拒绝。浏览器的工具在浏览器那边确认，这里一律放行。
          "-c",
          prefix + ".default_tools_approval_mode=\"approve\"",
          // 等用户在面板里确认时不能超时。
          "-c",
          prefix + ".tool_timeout_sec=900",
          "-c",
          prefix + ".startup_timeout_sec=30",
      });
  return arguments;
}

CodexBackend::CodexBackend(Config config, Delegate* delegate)
    : config_(std::move(config)),
      delegate_(delegate),
      thread_id_(config_.thread_id) {}

bool CodexBackend::running() const {
  return running_;
}

CodexBackend::~CodexBackend() {
  if (process_) {
    process_->CloseInput();
    process_->Terminate();
  }
}

void CodexBackend::Send(UserMessage user) {
  if (running_) {
    return;
  }
  running_ = true;
  stopping_ = false;
  streamed_messages_.clear();
  Message message;
  message.text = std::move(user.text);
  message.images = std::move(user.images);
  message.reasoning = std::move(user.reasoning);
  if (!process_) {
    queued_ = std::move(message);
    delegate_->OnStarting();
    Launch();
    return;
  }
  if (ready_) {
    StartTurn(std::move(message));
  } else {
    queued_ = std::move(message);
  }
}

void CodexBackend::Stop() {
  if (!running_) {
    return;
  }
  stopping_ = true;
  if (!thread_id_.empty() && !turn_id_.empty()) {
    Request(
        "turn/interrupt",
        base::DictValue().Set("threadId", thread_id_).Set("turnId", turn_id_),
        base::DoNothing());
  } else {
    // 还没开始这一轮（进程在启动）：直接结束。
    queued_.reset();
    Finish(StopReason::kStopped);
  }
  kill_timer_.Start(
      FROM_HERE, kStopTimeout,
      base::BindOnce(
          [](base::WeakPtr<CodexBackend> self) {
            if (self && self->running_ && self->stopping_ && self->process_) {
              self->process_->Terminate();
            }
          },
          weak_factory_.GetWeakPtr()));
}

void CodexBackend::Launch() {
  ready_ = false;
  LineProcess::Options options;
  options.executable = config_.executable;
  options.arguments = Arguments(config_);
  options.environment = config_.environment;
  options.directory = config_.working_directory;
  process_ = std::make_unique<LineProcess>(
      base::BindRepeating(&CodexBackend::HandleLine,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&CodexBackend::Exited, weak_factory_.GetWeakPtr()));
  process_->Start(std::move(options),
                  base::BindOnce(
                      [](base::WeakPtr<CodexBackend> self, std::string error) {
                        if (!self || error.empty()) {
                          return;
                        }
                        self->process_.reset();
                        self->Finish(StopReason::kFailed, FailureKind::kProcess,
                                     error);
                      },
                      weak_factory_.GetWeakPtr()));
  Request(
      "initialize",
      base::DictValue()
          .Set("clientInfo", base::DictValue()
                                 .Set("name", "starry_browser")
                                 .Set("title", "Starry Browser")
                                 .Set("version", "1.0"))
          .Set("capabilities", base::DictValue().Set("experimentalApi", false)),
      base::BindOnce(
          [](base::WeakPtr<CodexBackend> self,
             base::expected<base::Value, std::string> result) {
            if (!self) {
              return;
            }
            if (!result.has_value()) {
              self->Finish(StopReason::kFailed, KindFor(result.error()),
                           result.error());
              return;
            }
            self->Notify("initialized");
            self->LoadCatalog();
            self->OpenThread();
          },
          weak_factory_.GetWeakPtr()));
}

void CodexBackend::LoadCatalog() {
  Request("model/list", base::DictValue().Set("includeHidden", true),
          base::BindOnce(
              [](base::WeakPtr<CodexBackend> self,
                 base::expected<base::Value, std::string> result) {
                // 取不到也不要紧。
                if (!self || !result.has_value() || !result->is_dict()) {
                  return;
                }
                const base::ListValue* data =
                    result->GetDict().FindList("data");
                self->catalog_ = data ? data->Clone() : base::ListValue();
                self->PublishLevels();
              },
              weak_factory_.GetWeakPtr()));
}

void CodexBackend::PublishLevels() {
  if (!catalog_ || thread_model_.empty()) {
    return;
  }
  for (const base::Value& value : *catalog_) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& entry = value.GetDict();
    if (Text(entry, "model") != thread_model_ &&
        Text(entry, "id") != thread_model_) {
      continue;
    }
    std::vector<std::string> levels;
    if (const base::ListValue* efforts =
            entry.FindList("supportedReasoningEfforts")) {
      for (const base::Value& effort : *efforts) {
        if (effort.is_dict()) {
          const std::string level = Text(effort.GetDict(), "reasoningEffort");
          if (!level.empty()) {
            levels.push_back(level);
          }
        }
      }
    }
    default_level_ = Text(entry, "defaultReasoningEffort");
    supported_levels_ = levels;
    delegate_->OnReasoningLevels(levels);
    return;
  }
}

std::string CodexBackend::TurnEffort(const std::string& requested) const {
  std::string level = requested;
  if (!level.empty() && supported_levels_ &&
      std::ranges::find(*supported_levels_, level) ==
          supported_levels_->end()) {
    level.clear();
  }
  if (level.empty() && !sent_level_.empty() && sent_level_ != default_level_) {
    level = default_level_;
  }
  return level;
}

void CodexBackend::OpenThread() {
  base::DictValue params;
  params.Set("cwd", config_.working_directory.AsUTF8Unsafe());
  params.Set("approvalPolicy", "never");
  params.Set("sandbox", "read-only");
  params.Set("developerInstructions", config_.server
                                          ? config_.system_prompt + kToolNote
                                          : config_.system_prompt);
  // 审核用的：换掉它自己的系统提示，thread 不存。
  if (!config_.server) {
    params.Set("baseInstructions", config_.system_prompt);
  }
  if (config_.ephemeral || !config_.server) {
    params.Set("ephemeral", true);
  }
  if (!config_.model.empty()) {
    params.Set("model", config_.model);
  }
  const bool resume =
      !thread_id_.empty() && !config_.ephemeral && config_.server;
  if (resume) {
    params.Set("threadId", thread_id_);
  }
  Request(
      resume ? "thread/resume" : "thread/start", std::move(params),
      base::BindOnce(
          [](base::WeakPtr<CodexBackend> self, bool resume,
             base::expected<base::Value, std::string> result) {
            if (!self) {
              return;
            }
            if (!result.has_value()) {
              // 旧的 thread 接不上（被删了、换了机器）：开一个新的。
              if (resume) {
                self->thread_id_.clear();
                self->OpenThread();
                return;
              }
              self->Finish(StopReason::kFailed, KindFor(result.error()),
                           result.error());
              return;
            }
            if (result->is_dict()) {
              const base::DictValue& value = result->GetDict();
              if (const base::DictValue* thread = value.FindDict("thread")) {
                if (const std::string* id = thread->FindString("id")) {
                  self->thread_id_ = *id;
                  self->delegate_->OnSession(*id);
                }
              }
              if (const std::string* model = value.FindString("model")) {
                self->thread_model_ = *model;
                self->delegate_->OnModel(*model);
                self->PublishLevels();
              }
            }
            self->ready_ = true;
            if (self->queued_) {
              Message message = std::move(*self->queued_);
              self->queued_.reset();
              if (self->running_ && !self->stopping_) {
                self->StartTurn(std::move(message));
              }
            }
          },
          weak_factory_.GetWeakPtr(), resume));
}

void CodexBackend::StartTurn(Message message) {
  if (thread_id_.empty()) {
    Finish(StopReason::kFailed, FailureKind::kProcess,
           "Codex did not start a conversation.");
    return;
  }
  base::ListValue input;
  input.Append(base::DictValue()
                   .Set("type", "text")
                   .Set("text", message.text)
                   .Set("text_elements", base::ListValue()));
  for (const agent::ToolImage& image : message.images) {
    input.Append(base::DictValue()
                     .Set("type", "image")
                     .Set("url", base::StrCat({"data:", image.mime_type,
                                               ";base64,", image.base64})));
  }
  base::DictValue params;
  params.Set("threadId", thread_id_);
  params.Set("input", std::move(input));
  if (const std::string effort = TurnEffort(message.reasoning);
      !effort.empty()) {
    params.Set("effort", effort);
    sent_level_ = effort;
  }
  Request("turn/start", std::move(params),
          base::BindOnce(
              [](base::WeakPtr<CodexBackend> self,
                 base::expected<base::Value, std::string> result) {
                if (!self) {
                  return;
                }
                if (!result.has_value()) {
                  self->Finish(StopReason::kFailed, KindFor(result.error()),
                               result.error());
                  return;
                }
                if (result->is_dict()) {
                  if (const base::DictValue* turn =
                          result->GetDict().FindDict("turn")) {
                    self->turn_id_ = Text(*turn, "id");
                  }
                }
                if (self->stopping_ && !self->turn_id_.empty()) {
                  self->Request("turn/interrupt",
                                base::DictValue()
                                    .Set("threadId", self->thread_id_)
                                    .Set("turnId", self->turn_id_),
                                base::DoNothing());
                }
              },
              weak_factory_.GetWeakPtr()));
}

// ---- JSON-RPC（没有 jsonrpc 字段）----

void CodexBackend::Request(const std::string& method,
                           base::DictValue params,
                           Response response) {
  const int id = next_id_++;
  responses_[id] = std::move(response);
  if (process_) {
    process_->Write(Json(base::DictValue()
                             .Set("id", id)
                             .Set("method", method)
                             .Set("params", std::move(params))));
  }
}

void CodexBackend::Notify(const std::string& method) {
  if (process_) {
    process_->Write(Json(base::DictValue().Set("method", method)));
  }
}

void CodexBackend::Respond(const base::Value& id, base::DictValue result) {
  if (process_) {
    process_->Write(Json(base::DictValue()
                             .Set("id", id.Clone())
                             .Set("result", std::move(result))));
  }
}

void CodexBackend::RespondError(const base::Value& id,
                                const std::string& message) {
  if (process_) {
    process_->Write(Json(base::DictValue()
                             .Set("id", id.Clone())
                             .Set("error", base::DictValue()
                                               .Set("code", -32601)
                                               .Set("message", message))));
  }
}

void CodexBackend::HandleLine(std::string line) {
  std::optional<base::Value> value =
      base::JSONReader::Read(line, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return;
  }
  const base::DictValue& dict = value->GetDict();
  const std::string* method = dict.FindString("method");
  const base::Value* id = dict.Find("id");
  if (id && !method) {
    // 对我们请求的回应。
    if (!id->is_int()) {
      return;
    }
    auto it = responses_.find(id->GetInt());
    if (it == responses_.end()) {
      return;
    }
    Response response = std::move(it->second);
    responses_.erase(it);
    if (const base::DictValue* error = dict.FindDict("error")) {
      const std::string message = Text(*error, "message");
      std::move(response).Run(
          base::unexpected(message.empty() ? Json(*error) : message));
      return;
    }
    const base::Value* result = dict.Find("result");
    std::move(response).Run(result ? result->Clone() : base::Value());
    return;
  }
  if (!method) {
    return;
  }
  const base::DictValue empty;
  const base::DictValue* params = dict.FindDict("params");
  if (id) {
    AnswerServerRequest(*id, *method, params ? *params : empty);
    return;
  }
  HandleNotification(*method, params ? *params : empty);
}

// Codex 来问的事：它自己的命令、改文件都拒绝（本来就关掉了），要用户回答的问题转给面板。
void CodexBackend::AnswerServerRequest(const base::Value& id,
                                       const std::string& method,
                                       const base::DictValue& params) {
  if (method == "item/commandExecution/requestApproval" ||
      method == "item/fileChange/requestApproval") {
    Respond(id, base::DictValue().Set("decision", "decline"));
  } else if (method == "execCommandApproval" ||
             method == "applyPatchApproval") {
    Respond(id, base::DictValue().Set("decision", "denied"));
  } else if (method == "mcpServer/elicitation/request") {
    Respond(id, base::DictValue()
                    .Set("action", "decline")
                    .Set("content", base::Value())
                    .Set("_meta", base::Value()));
  } else if (method == "item/tool/requestUserInput") {
    PendingInput input;
    input.request_id = id.Clone();
    if (const base::ListValue* questions = params.FindList("questions")) {
      for (const base::Value& value : *questions) {
        if (!value.is_dict()) {
          continue;
        }
        const base::DictValue& question = value.GetDict();
        const std::string question_id = Text(question, "id");
        const std::string text = Text(question, "question");
        if (question_id.empty() || text.empty()) {
          continue;
        }
        agent::Question entry;
        entry.question = text;
        entry.detail = Text(question, "header");
        if (const base::ListValue* options = question.FindList("options")) {
          for (const base::Value& option : *options) {
            if (option.is_dict() && entry.options.size() < kMaxOptions) {
              const std::string label = Text(option.GetDict(), "label");
              if (!label.empty()) {
                entry.options.push_back(label);
              }
            }
          }
        }
        input.questions.emplace_back(question_id, std::move(entry));
      }
    }
    if (input.questions.empty()) {
      Respond(id, base::DictValue().Set("answers", base::DictValue()));
      return;
    }
    pending_inputs_.push_back(std::move(input));
    if (pending_inputs_.size() == 1) {
      AskNext();
    }
  } else {
    RespondError(id, "Not supported by Starry Browser.");
  }
}

void CodexBackend::AskNext() {
  if (pending_inputs_.empty() || pending_inputs_.front().questions.empty()) {
    return;
  }
  const auto& [id, question] = pending_inputs_.front().questions.front();
  delegate_->OnQuestion(id, question);
}

void CodexBackend::Answer(const std::string& id,
                          std::optional<std::string> answer) {
  if (pending_inputs_.empty() || pending_inputs_.front().questions.empty() ||
      pending_inputs_.front().questions.front().first != id) {
    return;
  }
  PendingInput& input = pending_inputs_.front();
  input.questions.erase(input.questions.begin());
  if (answer) {
    input.answers.Set(id, base::DictValue().Set(
                              "answers", base::ListValue().Append(*answer)));
  }
  if (!input.questions.empty()) {
    AskNext();
    return;
  }
  PendingInput done = std::move(input);
  pending_inputs_.erase(pending_inputs_.begin());
  Respond(done.request_id,
          base::DictValue().Set("answers", std::move(done.answers)));
  AskNext();
}

void CodexBackend::HandleNotification(const std::string& method,
                                      const base::DictValue& params) {
  if (method == "item/agentMessage/delta") {
    const std::string delta = Text(params, "delta");
    if (!delta.empty()) {
      streamed_messages_.insert(Text(params, "itemId"));
      delegate_->OnTextDelta(delta);
    }
  } else if (method == "item/reasoning/summaryTextDelta") {
    const std::string delta = Text(params, "delta");
    if (!delta.empty()) {
      delegate_->OnReasoningDelta(delta);
    }
  } else if (method == "item/started") {
    if (const base::DictValue* item = params.FindDict("item")) {
      ItemStarted(*item);
    }
  } else if (method == "item/completed") {
    if (const base::DictValue* item = params.FindDict("item")) {
      ItemCompleted(*item);
    }
  } else if (method == "thread/tokenUsage/updated") {
    const base::DictValue* usage = params.FindDict("tokenUsage");
    const base::DictValue* last = usage ? usage->FindDict("last") : nullptr;
    if (last) {
      Usage total;
      total.input_tokens = last->FindInt("inputTokens").value_or(0);
      total.output_tokens = last->FindInt("outputTokens").value_or(0);
      delegate_->OnUsage(total);
    }
  } else if (method == "error") {
    if (params.FindBool("willRetry").value_or(false)) {
      return;
    }
    const base::DictValue* error = params.FindDict("error");
    std::string message = error ? Text(*error, "message") : std::string();
    if (message.empty()) {
      message = "Codex reported an error.";
    }
    Finish(StopReason::kFailed, KindFor(message), message);
  } else if (method == "turn/completed") {
    const base::DictValue* turn = params.FindDict("turn");
    const std::string status = turn ? Text(*turn, "status") : std::string();
    turn_id_.clear();
    if (status == "interrupted") {
      Finish(StopReason::kStopped);
    } else if (status == "failed") {
      const base::DictValue* error = turn->FindDict("error");
      std::string message = error ? Text(*error, "message") : std::string();
      if (message.empty()) {
        message = "The turn failed.";
      }
      Finish(StopReason::kFailed, KindFor(message), message);
    } else {
      Finish(StopReason::kCompleted);
    }
  } else if (method == "mcpServer/startupStatus/updated") {
    if (Text(params, "name") != kBrowserMcpServerName &&
        Text(params, "server") != kBrowserMcpServerName) {
      return;
    }
    std::string status = Text(params, "status");
    if (status.empty()) {
      if (const base::DictValue* state = params.FindDict("status")) {
        status = Text(*state, "state");
      }
    }
    status = base::ToLowerASCII(status);
    if (status == "failed" || status == "error" || status == "cancelled") {
      const std::string error = Text(params, "error");
      delegate_->OnToolsUnavailable(error.empty() ? status : error);
    }
  }
}

void CodexBackend::ItemStarted(const base::DictValue& item) {
  const std::string id = Text(item, "id");
  if (id.empty()) {
    return;
  }
  const std::string type = Text(item, "type");
  if (type == "mcpToolCall" || type == "dynamicToolCall") {
    std::string name = Text(item, "tool");
    if (name.empty()) {
      name = "tool";
    }
    const base::DictValue* arguments = item.FindDict("arguments");
    delegate_->OnToolStarted(
        id, BrowserToolName(name),
        arguments ? arguments->Clone() : base::DictValue());
  } else if (type == "commandExecution") {
    const base::Value* command = item.Find("command");
    delegate_->OnToolStarted(
        id, "shell",
        base::DictValue().Set("command",
                              command ? command->Clone() : base::Value()));
  }
}

void CodexBackend::ItemCompleted(const base::DictValue& item) {
  const std::string id = Text(item, "id");
  if (id.empty()) {
    return;
  }
  const std::string type = Text(item, "type");
  if (type == "agentMessage") {
    const std::string text = Text(item, "text");
    if (!streamed_messages_.contains(id) && !text.empty()) {
      delegate_->OnTextDelta(text);
    }
    return;
  }
  agent::ToolResult result;
  if (type == "mcpToolCall") {
    const base::Value* error = item.Find("error");
    result.is_error =
        Text(item, "status") == "failed" || (error && !error->is_none());
    if (error && error->is_dict()) {
      result.text = Text(error->GetDict(), "message");
    } else if (const base::DictValue* value = item.FindDict("result")) {
      std::vector<std::string> texts;
      if (const base::ListValue* content = value->FindList("content")) {
        for (const base::Value& part : *content) {
          if (part.is_dict() && Text(part.GetDict(), "type") == "text") {
            texts.push_back(Text(part.GetDict(), "text"));
          }
        }
      }
      result.text = base::JoinString(texts, "\n");
    }
  } else if (type == "dynamicToolCall") {
    std::vector<std::string> texts;
    if (const base::ListValue* content = item.FindList("contentItems")) {
      for (const base::Value& part : *content) {
        if (part.is_dict()) {
          texts.push_back(Text(part.GetDict(), "text"));
        }
      }
    }
    result.text = base::JoinString(texts, "\n");
    result.is_error = item.FindBool("success") == false;
  } else if (type == "commandExecution") {
    result.text = Text(item, "aggregatedOutput");
    result.is_error = item.FindInt("exitCode").value_or(0) != 0;
  } else {
    return;
  }
  delegate_->OnToolFinished(id, result);
}

void CodexBackend::Exited(int status, std::string errors) {
  process_.reset();
  ready_ = false;
  pending_inputs_.clear();
  kill_timer_.Stop();
  auto responses = std::move(responses_);
  responses_.clear();
  for (auto& [id, response] : responses) {
    std::move(response).Run(base::unexpected(std::string("Codex exited.")));
  }
  if (!running_) {
    return;
  }
  const std::string message =
      errors.empty()
          ? "Codex exited with status " + base::NumberToString(status) + "."
          : errors;
  Finish(StopReason::kFailed,
         LooksLikeAuthentication(message) ? FailureKind::kAuthentication
                                          : FailureKind::kProcess,
         message);
}

void CodexBackend::Finish(StopReason reason,
                          FailureKind failure,
                          const std::string& message) {
  if (!running_) {
    return;
  }
  running_ = false;
  turn_id_.clear();
  kill_timer_.Stop();
  // 这一轮结束了：还没回答的问题不再等。
  auto inputs = std::move(pending_inputs_);
  pending_inputs_.clear();
  for (PendingInput& input : inputs) {
    Respond(input.request_id,
            base::DictValue().Set("answers", std::move(input.answers)));
  }
  if (stopping_) {
    reason = StopReason::kStopped;
    failure = FailureKind::kNone;
  }
  delegate_->OnFinished(reason, failure, message);
}

}  // namespace starry::assistant
