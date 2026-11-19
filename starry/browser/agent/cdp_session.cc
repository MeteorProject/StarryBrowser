// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/cdp_session.h"

#include <utility>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"

namespace starry::agent {

CdpSession::CdpSession(content::WebContents* contents)
    : CdpSession(content::DevToolsAgentHost::GetOrCreateFor(contents)) {}

CdpSession::CdpSession(scoped_refptr<content::DevToolsAgentHost> host)
    : host_(std::move(host)) {
  attached_ = host_ && host_->AttachClient(this);
}

CdpSession::~CdpSession() {
  if (attached_ && host_) {
    host_->DetachClient(this);
  }
  for (auto& [id, callback] : pending_) {
    std::move(callback).Run(base::unexpected("The tab closed."));
  }
}

void CdpSession::Send(const std::string& method,
                      base::DictValue params,
                      Callback callback) {
  if (!attached_) {
    std::move(callback).Run(base::unexpected("Cannot control this tab."));
    return;
  }
  const int id = next_id_++;
  base::DictValue message;
  message.Set("id", id);
  message.Set("method", method);
  message.Set("params", std::move(params));
  std::string json;
  base::JSONWriter::Write(message, &json);
  pending_[id] = std::move(callback);
  host_->DispatchProtocolMessage(this, base::as_byte_span(json));
}

base::CallbackListSubscription CdpSession::AddEventCallback(
    EventCallback callback) {
  return events_.Add(std::move(callback));
}

void CdpSession::DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                                         base::span<const uint8_t> message) {
  std::optional<base::Value> parsed = base::JSONReader::Read(
      std::string_view(reinterpret_cast<const char*>(message.data()),
                       message.size()),
      base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;
  }
  base::DictValue& dict = parsed->GetDict();
  const std::optional<int> id = dict.FindInt("id");
  if (!id) {
    const std::string* method = dict.FindString("method");
    const base::DictValue* params = dict.FindDict("params");
    if (method) {
      const base::DictValue empty;
      events_.Notify(*method, params ? *params : empty);
    }
    return;
  }
  auto it = pending_.find(*id);
  if (it == pending_.end()) {
    return;
  }
  Callback callback = std::move(it->second);
  pending_.erase(it);
  if (const base::DictValue* error = dict.FindDict("error")) {
    const std::string* text = error->FindString("message");
    std::move(callback).Run(
        base::unexpected(text ? *text : std::string("Protocol error.")));
    return;
  }
  base::DictValue* result = dict.FindDict("result");
  std::move(callback).Run(result ? std::move(*result) : base::DictValue());
}

void CdpSession::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  attached_ = false;
  auto pending = std::move(pending_);
  for (auto& [id, callback] : pending) {
    std::move(callback).Run(base::unexpected("The tab closed."));
  }
}

bool CdpSession::IsTrusted() {
  return true;
}

bool CdpSession::MayReadLocalFiles() {
  // 上传文件（DOM.setFileInputFiles、拖进文件）要它。路径都是智能体给的、检查过的
  // （browser_file_upload），这条连接不会自己去读别的本机文件。
  return true;
}

}  // namespace starry::agent
