// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/agent_service.h"

#include <utility>

#include "base/callback_list.h"
#include "base/containers/circular_deque.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/common/chrome_paths.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/agent/agent_socket.h"
#include "starry/browser/agent/line_channel.h"
#include "starry/browser/agent/private_windows.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"

namespace starry::agent {

namespace {

bool ExternalAllowedFor(Profile* profile) {
  return profile && !profile->IsOffTheRecord() &&
         prefs::PrefsFor(profile)->GetBoolean(prefs::kAgentAllowExternal);
}

}  // namespace

// 外部智能体的那套工具：在最近用过的、打开了外部智能体的普通窗口上。它们的客户端自己会请
// 用户确认，这里每一步都直接做；网页上照样框出要操作的元素，描边和提示条（写着客户端报的
// 名字）一直在，点提示条的「停止」断开这条连接。窗口关了就换一个。
class AgentService::ExternalAgent : public ToolRunner::Delegate {
 public:
  ExternalAgent(base::RepeatingCallback<std::u16string()> name,
                base::RepeatingClosure stop)
      : name_(std::move(name)), stop_(std::move(stop)) {}
  ExternalAgent(const ExternalAgent&) = delete;
  ExternalAgent& operator=(const ExternalAgent&) = delete;
  ~ExternalAgent() override = default;

  Profile* profile() {
    Pick();
    return browser_ ? browser_->GetProfile() : nullptr;
  }

  void Run(const std::string& name, base::DictValue args, ToolCallback done) {
    Pick();
    if (!runner_) {
      std::move(done).Run(
          ToolResult{"There is no browser window to work in. Ask the user to "
                     "open a Starry Browser window.",
                     true});
      return;
    }
    runner_->Run(name, std::move(args),
                 base::BindOnce(&ExternalAgent::Done,
                                weak_factory_.GetWeakPtr(), std::move(done)));
  }

  // ToolRunner::Delegate:
  void AuthorizeStep(const ToolStep& step,
                     base::OnceCallback<void(Approval)> callback) override {
    Approval approval;
    approval.allowed = true;
    std::move(callback).Run(approval);
  }
  void AskUser(
      const Question& question,
      base::OnceCallback<void(std::optional<std::string>)> callback) override {
    std::move(callback).Run(std::nullopt);
  }
  std::u16string AgentName() override { return name_.Run(); }
  bool IsAssistant() override { return false; }
  // 断开这条连接（会把自己删掉）。
  void StopRequested() override { stop_.Run(); }

 private:
  void Pick() {
    if (runner_) {
      return;
    }
    BrowserWindowInterface* found = nullptr;
    ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
        [&found](BrowserWindowInterface* browser) {
          if (browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
              ExternalAllowedFor(browser->GetProfile())) {
            found = browser;
            return false;
          }
          return true;
        });
    if (!found) {
      return;
    }
    browser_ = found;
    runner_ = std::make_unique<ToolRunner>(found, this);
    closing_ = found->RegisterBrowserDidClose(base::BindRepeating(
        &ExternalAgent::BrowserClosed, base::Unretained(this)));
  }

  void BrowserClosed(BrowserWindowInterface* browser) {
    runner_.reset();
    browser_ = nullptr;
    closing_ = base::CallbackListSubscription();
  }

  void Done(ToolCallback done, ToolResult result) {
    std::move(done).Run(std::move(result));
  }

  base::RepeatingCallback<std::u16string()> name_;
  base::RepeatingClosure stop_;
  raw_ptr<BrowserWindowInterface> browser_ = nullptr;
  std::unique_ptr<ToolRunner> runner_;
  base::CallbackListSubscription closing_;
  base::WeakPtrFactory<ExternalAgent> weak_factory_{this};
};

struct AgentService::PendingCall {
  std::string name;
  base::DictValue arguments;
  ToolCallback done;
};

struct AgentService::Connection {
  enum class Kind {
    // 还没收到第一行。
    kUnknown,
    kAssistant,
    kExternal,
    // 外部智能体没打开：每个请求回一个错误。
    kRefused,
  };

  int id = 0;
  Kind kind = Kind::kUnknown;
  std::unique_ptr<LineChannel> channel;
  std::unique_ptr<McpSession> session;
  std::string token;
  base::WeakPtr<ConnectionOwner> owner;
  std::unique_ptr<ExternalAgent> external;
  // 一次一个调用：快照里的 ref、当前标签都是这一套工具的状态，交错着做会点错。
  base::circular_deque<PendingCall> queue;
  ToolCallback current;
  bool busy = false;
};

// static
AgentService* AgentService::Get() {
  static base::NoDestructor<AgentService> service;
  return service.get();
}

AgentService::AgentService() = default;
AgentService::~AgentService() = default;

void AgentService::Shutdown() {
  server_callbacks_.InvalidateWeakPtrs();
  connections_.clear();
  tokens_.clear();
  server_.reset();
  listening_ = false;
}

std::string AgentService::RegisterToken(base::WeakPtr<ConnectionOwner> owner) {
  std::string token =
      base::ToLowerASCII(base::HexEncode(base::RandBytesAsVector(24)));
  tokens_[token] = std::move(owner);
  Refresh();
  return token;
}

void AgentService::RevokeToken(const std::string& token) {
  if (!tokens_.erase(token)) {
    return;
  }
  std::vector<int> drop;
  for (const auto& [id, connection] : connections_) {
    if (connection->token == token) {
      drop.push_back(id);
    }
  }
  for (int id : drop) {
    Drop(id);
  }
  Refresh();
}

bool AgentService::ExternalAllowed() const {
  ProfileManager* manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  if (!manager) {
    return false;
  }
  for (Profile* profile : manager->GetLoadedProfiles()) {
    if (ExternalAllowedFor(profile)) {
      return true;
    }
  }
  return false;
}

bool AgentService::WantsServer() const {
  return !tokens_.empty() || ExternalAllowed();
}

void AgentService::SettingsChanged() {
  // 外部智能体关掉了：断开它们。
  if (!ExternalAllowed()) {
    DisconnectExternal();
  }
  // 不再允许临时无痕标签：已经开着的全部关掉。
  PrivateWindows::Get()->SettingsChanged();
  Refresh();
  for (const auto& [id, connection] : connections_) {
    if (connection->session) {
      connection->session->ToolsChanged();
    }
  }
}

void AgentService::Refresh() {
  // 对话没了（窗口关了）的令牌不算。
  std::erase_if(tokens_, [](const auto& entry) { return !entry.second; });
  if (WantsServer() == static_cast<bool>(server_)) {
    return;
  }
  if (server_) {
    server_callbacks_.InvalidateWeakPtrs();
    server_.reset();
    listening_ = false;
    connections_.clear();
    NotifyStatus();
    return;
  }
  base::FilePath dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &dir)) {
    return;
  }
  server_ = std::make_unique<SocketServer>(base::BindRepeating(
      &AgentService::OnConnection, server_callbacks_.GetWeakPtr()));
  server_->Start(dir, base::BindOnce(&AgentService::ServerStarted,
                                     server_callbacks_.GetWeakPtr()));
}

void AgentService::ServerStarted(std::string error) {
  error_ = error;
  listening_ = error.empty();
  if (!error.empty()) {
    LOG(ERROR) << "agent socket: " << error;
  }
  NotifyStatus();
}

void AgentService::OnConnection(base::ScopedPlatformFile endpoint) {
  if (!server_) {
    return;
  }
  const int id = next_id_++;
  auto connection = std::make_unique<Connection>();
  connection->id = id;
  base::File read(std::move(endpoint));
  base::File write = read.Duplicate();
  if (!write.IsValid()) {
    return;
  }
  connection->channel = std::make_unique<LineChannel>(
      base::ScopedPlatformFile(read.TakePlatformFile()),
      base::ScopedPlatformFile(write.TakePlatformFile()),
      base::BindRepeating(&AgentService::OnLine, weak_factory_.GetWeakPtr(),
                          id),
      base::BindOnce(&AgentService::OnClosed, weak_factory_.GetWeakPtr(), id));
  connection->session = std::make_unique<McpSession>(
      this,
      base::BindRepeating(
          [](base::WeakPtr<AgentService> service, int id, std::string line) {
            if (!service) {
              return;
            }
            auto it = service->connections_.find(id);
            if (it != service->connections_.end()) {
              it->second->channel->Send(std::move(line));
            }
          },
          weak_factory_.GetWeakPtr(), id));
  connections_[id] = std::move(connection);
}

void AgentService::OnLine(int id, std::string line) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  Connection* connection = it->second.get();
  if (connection->kind == Connection::Kind::kUnknown) {
    // 第一行：内置助手启动的智能体先报令牌（不是 MCP 消息）。
    std::optional<base::Value> hello =
        base::JSONReader::Read(line, base::JSON_PARSE_RFC);
    const std::string* token =
        hello && hello->is_dict() && !hello->GetDict().Find("jsonrpc")
            ? hello->GetDict().FindString(kAssistantTokenKey)
            : nullptr;
    if (token) {
      auto entry = tokens_.find(*token);
      if (entry == tokens_.end() || !entry->second) {
        LOG(ERROR) << "an agent connection presented an unknown assistant "
                      "token";
        Drop(id);
        return;
      }
      connection->kind = Connection::Kind::kAssistant;
      connection->token = *token;
      connection->owner = entry->second;
      return;
    }
    if (ExternalAllowed()) {
      connection->kind = Connection::Kind::kExternal;
      connection->external = std::make_unique<ExternalAgent>(
          base::BindRepeating(&AgentService::ClientName, base::Unretained(this),
                              id),
          base::BindRepeating(&AgentService::Drop, weak_factory_.GetWeakPtr(),
                              id));
    } else {
      connection->kind = Connection::Kind::kRefused;
    }
  }
  if (connection->kind == Connection::Kind::kRefused) {
    std::optional<base::Value> request =
        base::JSONReader::Read(line, base::JSON_PARSE_RFC);
    if (request && request->is_dict() &&
        request->GetDict().FindString("method")) {
      if (const base::Value* request_id = request->GetDict().Find("id")) {
        connection->channel->Send(
            McpSession::ErrorLine(*request_id, -32000, kDisabledMessage));
      }
    }
    return;
  }
  const bool was_initialized = connection->session->initialized();
  connection->session->Receive(line);
  if (connection->kind == Connection::Kind::kExternal && !was_initialized &&
      connection->session->initialized()) {
    NotifyStatus();
  }
}

void AgentService::OnClosed(int id) {
  Drop(id);
}

void AgentService::Drop(int id) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  const bool external = it->second->kind == Connection::Kind::kExternal;
  it->second->session->Close();
  connections_.erase(it);
  if (external) {
    NotifyStatus();
  }
}

std::u16string AgentService::ClientName(int id) const {
  auto it = connections_.find(id);
  if (it != connections_.end() && it->second->session) {
    const std::string& title = it->second->session->client_title();
    const std::string& name = it->second->session->client_name();
    if (!title.empty() || !name.empty()) {
      return base::UTF8ToUTF16(!title.empty() ? title : name);
    }
  }
  return L("agent.client.unknown");
}

AgentService::Connection* AgentService::ForSession(McpSession* session) {
  for (const auto& [id, connection] : connections_) {
    if (connection->session.get() == session) {
      return connection.get();
    }
  }
  return nullptr;
}

std::string AgentService::Instructions(McpSession* session) {
  Connection* connection = ForSession(session);
  if (!connection) {
    return std::string();
  }
  if (connection->kind == Connection::Kind::kAssistant) {
    return connection->owner ? connection->owner->AgentInstructions()
                             : std::string();
  }
  Profile* profile =
      connection->external ? connection->external->profile() : nullptr;
  return McpInstructions(profile ? ToolOptionsFor(profile, /*ask_user=*/false)
                                 : ToolOptions());
}

base::ListValue AgentService::Tools(McpSession* session) {
  Connection* connection = ForSession(session);
  if (!connection) {
    return base::ListValue();
  }
  if (connection->kind == Connection::Kind::kAssistant) {
    return connection->owner ? connection->owner->AgentTools()
                             : base::ListValue();
  }
  Profile* profile =
      connection->external ? connection->external->profile() : nullptr;
  return McpToolDefinitions(
      profile ? ToolOptionsFor(profile, /*ask_user=*/false) : ToolOptions());
}

void AgentService::CallTool(McpSession* session,
                            const std::string& name,
                            base::DictValue arguments,
                            ToolCallback done) {
  Connection* connection = ForSession(session);
  if (!connection) {
    std::move(done).Run(ToolResult{"The connection was closed.", true});
    return;
  }
  connection->queue.push_back(
      PendingCall{name, std::move(arguments), std::move(done)});
  RunNext(connection);
}

void AgentService::RunNext(Connection* connection) {
  if (connection->busy || connection->queue.empty()) {
    return;
  }
  PendingCall call = std::move(connection->queue.front());
  connection->queue.pop_front();
  connection->busy = true;
  connection->current = std::move(call.done);
  ToolCallback done = base::BindOnce(
      &AgentService::CallDone, weak_factory_.GetWeakPtr(), connection->id);
  if (connection->kind == Connection::Kind::kAssistant) {
    if (!connection->owner) {
      std::move(done).Run(
          ToolResult{"The assistant conversation was closed.", true});
      return;
    }
    connection->owner->RunAgentTool(call.name, std::move(call.arguments),
                                    std::move(done));
    return;
  }
  connection->external->Run(call.name, std::move(call.arguments),
                            std::move(done));
}

void AgentService::CallDone(int id, ToolResult result) {
  auto it = connections_.find(id);
  if (it == connections_.end()) {
    return;
  }
  Connection* connection = it->second.get();
  connection->busy = false;
  if (connection->current) {
    std::move(connection->current).Run(std::move(result));
  }
  RunNext(connection);
}

AgentService::Status AgentService::GetStatus() const {
  Status status;
  status.listening = listening_;
  status.error = error_;
  for (const auto& [id, connection] : connections_) {
    if (connection->kind != Connection::Kind::kExternal ||
        !connection->session->initialized()) {
      continue;
    }
    const std::string& title = connection->session->client_title();
    const std::string& name = connection->session->client_name();
    status.clients.push_back(!title.empty()  ? title
                             : !name.empty() ? name
                                             : std::string("MCP"));
  }
  return status;
}

void AgentService::DisconnectExternal() {
  std::vector<int> drop;
  for (const auto& [id, connection] : connections_) {
    if (connection->kind == Connection::Kind::kExternal ||
        connection->kind == Connection::Kind::kRefused) {
      drop.push_back(id);
    }
  }
  for (int id : drop) {
    Drop(id);
  }
  NotifyStatus();
}

base::CallbackListSubscription AgentService::AddStatusObserver(
    base::RepeatingClosure callback) {
  return status_observers_.Add(std::move(callback));
}

void AgentService::NotifyStatus() {
  status_observers_.Notify();
}

}  // namespace starry::agent
