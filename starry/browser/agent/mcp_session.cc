// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/mcp_session.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "starry/common/version.h"

namespace starry::agent {

namespace {

constexpr char kServerName[] = "starry-browser";
constexpr char kServerTitle[] = "Starry Browser";

std::string Write(const base::DictValue& message) {
  return base::WriteJson(message).value_or("{}");
}

base::DictValue Result(const base::Value& id, base::DictValue result) {
  return base::DictValue()
      .Set("jsonrpc", "2.0")
      .Set("id", id.Clone())
      .Set("result", std::move(result));
}

base::DictValue Error(const base::Value& id,
                      int code,
                      const std::string& message) {
  return base::DictValue()
      .Set("jsonrpc", "2.0")
      .Set("id", id.Clone())
      .Set("error",
           base::DictValue().Set("code", code).Set("message", message));
}

// 工具结果写成 MCP 的 content：文字一段，截图一张张跟在后面。
base::DictValue ResultJson(const ToolResult& result) {
  base::ListValue content;
  content.Append(
      base::DictValue().Set("type", "text").Set("text", result.text));
  for (const ToolImage& image : result.images) {
    content.Append(base::DictValue()
                       .Set("type", "image")
                       .Set("data", image.base64)
                       .Set("mimeType", image.mime_type));
  }
  return base::DictValue()
      .Set("content", std::move(content))
      .Set("isError", result.is_error);
}

}  // namespace

// static
const std::vector<std::string>& McpSession::SupportedProtocolVersions() {
  static const base::NoDestructor<std::vector<std::string>> versions(
      std::vector<std::string>{"2025-11-25", "2025-06-18", "2025-03-26",
                               "2024-11-05"});
  return *versions;
}

// static
std::string McpSession::ErrorLine(const base::Value& id,
                                  int code,
                                  const std::string& message) {
  return Write(Error(id, code, message));
}

McpSession::McpSession(Provider* provider,
                       base::RepeatingCallback<void(std::string)> send)
    : provider_(provider), send_(std::move(send)) {}

McpSession::~McpSession() = default;

void McpSession::Receive(std::string_view line) {
  if (closed_) {
    return;
  }
  const std::string_view text = base::TrimWhitespaceASCII(line, base::TRIM_ALL);
  if (text.empty()) {
    return;
  }
  std::optional<base::Value> message =
      base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  if (!message) {
    Reply(Error(base::Value(), -32700, "Parse error"));
    return;
  }
  // 2025-06-18 起去掉了批量请求，这里统一拒绝。
  if (message->is_list()) {
    Reply(Error(base::Value(), -32600, "Batch requests are not supported"));
    return;
  }
  if (!message->is_dict()) {
    Reply(Error(base::Value(), -32600, "Invalid Request"));
    return;
  }
  Handle(std::move(*message).TakeDict());
}

void McpSession::Handle(base::DictValue message) {
  const base::Value* id = message.Find("id");
  const std::string* jsonrpc = message.FindString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0") {
    Reply(Error(id ? id->Clone() : base::Value(), -32600, "Invalid Request"));
    return;
  }
  // 没有 method 的是客户端对我们请求的回应：我们不发请求，忽略。
  const std::string* method = message.FindString("method");
  if (!method) {
    return;
  }
  const base::DictValue empty;
  const base::DictValue* params = message.FindDict("params");
  if (!params) {
    params = &empty;
  }
  if (*method == "initialize") {
    if (id) {
      Initialize(*id, *params);
    }
  } else if (*method == "notifications/initialized") {
    initialized_ = true;
  } else if (*method == "ping") {
    if (id) {
      Reply(Result(*id, base::DictValue()));
    }
  } else if (*method == "tools/list") {
    if (id) {
      Reply(
          Result(*id, base::DictValue().Set("tools", provider_->Tools(this))));
    }
  } else if (*method == "tools/call") {
    if (id) {
      Call(*id, *params);
    }
  } else if (*method == "notifications/cancelled") {
    if (const base::Value* request = params->Find("requestId")) {
      const std::string key = base::WriteJson(*request).value_or("");
      if (running_.contains(key)) {
        cancelled_.insert(key);
      }
    }
  } else if (id) {
    // 其他通知（notifications/roots/list_changed 这类）直接忽略；请求回「没有这个方法」。
    Reply(Error(*id, -32601, "Method not found: " + *method));
  }
}

void McpSession::Initialize(const base::Value& id,
                            const base::DictValue& params) {
  const std::vector<std::string>& versions = SupportedProtocolVersions();
  const std::string* requested = params.FindString("protocolVersion");
  const std::string version =
      requested && std::ranges::find(versions, *requested) != versions.end()
          ? *requested
          : versions.front();
  if (const base::DictValue* client = params.FindDict("clientInfo")) {
    if (const std::string* name = client->FindString("name")) {
      client_name_ = *name;
    }
    if (const std::string* title = client->FindString("title")) {
      client_title_ = *title;
    }
  }
  base::DictValue result;
  result.Set("protocolVersion", version);
  result.Set("capabilities",
             base::DictValue().Set("tools",
                                   base::DictValue().Set("listChanged", true)));
  result.Set("serverInfo", base::DictValue()
                               .Set("name", kServerName)
                               .Set("title", kServerTitle)
                               .Set("version", starry::kVersion));
  std::string instructions = provider_->Instructions(this);
  if (!instructions.empty()) {
    result.Set("instructions", std::move(instructions));
  }
  Reply(Result(id, std::move(result)));
}

void McpSession::Call(const base::Value& id, const base::DictValue& params) {
  const std::string* name = params.FindString("name");
  if (!name) {
    Reply(Error(id, -32602, "Missing tool name"));
    return;
  }
  bool known = false;
  for (const base::Value& tool : provider_->Tools(this)) {
    const std::string* tool_name =
        tool.is_dict() ? tool.GetDict().FindString("name") : nullptr;
    if (tool_name && *tool_name == *name) {
      known = true;
      break;
    }
  }
  if (!known) {
    Reply(Error(id, -32602, "Unknown tool: " + *name));
    return;
  }
  base::DictValue arguments;
  if (const base::Value* value = params.Find("arguments");
      value && !value->is_none()) {
    if (!value->is_dict()) {
      Reply(Error(id, -32602, "Tool arguments must be an object"));
      return;
    }
    arguments = value->GetDict().Clone();
  }
  const std::string key = base::WriteJson(id).value_or("");
  running_.insert(key);
  provider_->CallTool(
      this, *name, std::move(arguments),
      base::BindOnce(&McpSession::CallDone, weak_factory_.GetWeakPtr(), key,
                     id.Clone()));
}

void McpSession::CallDone(std::string key, base::Value id, ToolResult result) {
  running_.erase(key);
  // 被取消的请求不回应（规范的要求），连接断了也不回。
  if (cancelled_.erase(key) || closed_) {
    return;
  }
  Reply(Result(id, ResultJson(result)));
}

void McpSession::Close() {
  closed_ = true;
  running_.clear();
  cancelled_.clear();
}

void McpSession::ToolsChanged() {
  if (!initialized_) {
    return;
  }
  Reply(base::DictValue()
            .Set("jsonrpc", "2.0")
            .Set("method", "notifications/tools/list_changed"));
}

void McpSession::Reply(base::DictValue message) {
  if (closed_) {
    return;
  }
  send_.Run(Write(message));
}

}  // namespace starry::agent
