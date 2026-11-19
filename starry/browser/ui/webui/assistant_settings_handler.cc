// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/assistant_settings_handler.h"

#include <algorithm>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_paths_internal.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_ui.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "starry/browser/agent/agent_service.h"
#include "starry/browser/assistant/assistant_settings.h"
#include "starry/browser/assistant/chat_backend.h"
#include "starry/browser/assistant/local_agent.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/assistant/assistant_controller.h"
#include "starry/browser/ui/assistant/assistant_island.h"
#include "starry/browser/ui/frame/starry_browser_view.h"

namespace starry {

namespace {

constexpr net::NetworkTrafficAnnotationTag kModelsAnnotation =
    net::DefineNetworkTrafficAnnotation("starry_assistant_models", R"(
        semantics {
          sender: "Starry Browser AI assistant settings"
          description:
            "Fetches the list of models from the chat-completions service the "
            "user configured, to choose one in settings."
          trigger: "The user clicks 'Choose from list' in the AI settings."
          data: "The API key the user entered for that service."
          destination: OTHER
          destination_other: "The service the user configured in settings."
        }
        policy {
          cookies_allowed: NO
          setting: "Settings > AI agent > Assistant."
          policy_exception_justification: "Not implemented."
        })");

// 外部 MCP 客户端使用的 stdio 服务器参数：浏览器可执行文件加 --mcp；使用自定义
// --user-data-dir 启动时一并透传。
std::vector<std::string> BridgeArguments() {
  std::vector<std::string> arguments = {"--mcp"};
  base::FilePath dir, default_dir;
  if (base::PathService::Get(chrome::DIR_USER_DATA, &dir) &&
      chrome::GetDefaultUserDataDirectory(&default_dir) && dir != default_dir) {
    arguments.push_back("--user-data-dir=" + dir.AsUTF8Unsafe());
  }
  return arguments;
}

std::string ShellQuote(const std::string& text) {
#if BUILDFLAG(IS_WIN)
  std::string quoted = text;
  // PowerShell treats typographic single quotes as delimiters too.
  for (std::string_view mark : {"'", "‘", "’", "‚", "‛"}) {
    base::ReplaceSubstringsAfterOffset(&quoted, 0, mark,
                                       std::string(mark) + std::string(mark));
  }
  return "'" + quoted + "'";
#else
  if (text.find_first_of(" '\"\\$`") == std::string::npos) {
    return text;
  }
  std::string quoted = "'";
  for (char c : text) {
    quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
  }
  return quoted + "'";
#endif
}

}  // namespace

AssistantSettingsHandler::AssistantSettingsHandler(Profile* profile)
    : profile_(profile) {}

AssistantSettingsHandler::~AssistantSettingsHandler() = default;

void AssistantSettingsHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getAssistantSettings", &AssistantSettingsHandler::HandleGet);
  add("setAssistantSetting", &AssistantSettingsHandler::HandleSet);
  add("setAssistantKey", &AssistantSettingsHandler::HandleSetKey);
  add("fetchAssistantModels", &AssistantSettingsHandler::HandleFetchModels);
  add("checkLocalAgent", &AssistantSettingsHandler::HandleCheckLocalAgent);
  add("disconnectAgents", &AssistantSettingsHandler::HandleDisconnectAgents);
}

void AssistantSettingsHandler::OnJavascriptAllowed() {
  prefs_.Init(prefs::PrefsFor(profile_));
  for (const char* name :
       {prefs::kAssistantProvider, prefs::kAssistantModels,
        prefs::kAssistantKeys, prefs::kAssistantCustomUrl,
        prefs::kAssistantMode, prefs::kAgentAllowScripts,
        prefs::kAgentAllowUploads, prefs::kAgentAllowPrivateTabs,
        prefs::kAssistantBackend, prefs::kAssistantLocalPaths,
        prefs::kAssistantLocalModels, prefs::kAgentAllowExternal,
        prefs::kAssistantReasoning, prefs::kAssistantPanelStyle,
        prefs::kAssistantEnabled}) {
    prefs_.Add(name, base::BindRepeating(&AssistantSettingsHandler::Changed,
                                         base::Unretained(this)));
  }
  agent_status_ =
      agent::AgentService::Get()->AddStatusObserver(base::BindRepeating(
          &AssistantSettingsHandler::Changed, base::Unretained(this)));
}

void AssistantSettingsHandler::OnJavascriptDisallowed() {
  prefs_.RemoveAll();
  agent_status_ = base::CallbackListSubscription();
  loader_.reset();
  weak_factory_.InvalidateWeakPtrs();
}

base::DictValue AssistantSettingsHandler::State() const {
  const assistant::Settings settings = assistant::ReadSettings(profile_);
  base::ListValue providers;
  for (const assistant::ChatProvider& provider : assistant::ProviderCatalog()) {
    providers.Append(base::DictValue()
                         .Set("id", provider.id)
                         .Set("title", assistant::ProviderDisplayName(provider))
                         .Set("requiresKey", provider.requires_key)
                         .Set("keyPage", provider.key_page)
                         .Set("local", provider.is_local)
                         .Set("baseUrl", provider.base_url));
  }
  providers.Append(base::DictValue()
                       .Set("id", assistant::kCustomProviderId)
                       .Set("title", L("settings.ai.assistant.custom.defaultName"))
                       .Set("requiresKey", false)
                       .Set("custom", true));
  base::ListValue suggested;
  for (const std::string& model : settings.provider.suggested_models) {
    suggested.Append(model);
  }
  base::DictValue state;
  state.Set("enabled", assistant::Enabled(profile_));
  state.Set("providers", std::move(providers));
  state.Set("provider", settings.provider_id);
  state.Set("customUrl", assistant::CustomBaseUrl(profile_));
  state.Set("model", settings.model);
  state.Set("suggestedModels", std::move(suggested));
  state.Set("hasKey", settings.has_key);
  state.Set("mode", settings.mode);
  state.Set("panelStyle", settings.panel_style);
  state.Set("allowScripts", settings.allow_scripts);
  state.Set("allowUploads", settings.allow_uploads);
  state.Set("allowPrivateTabs", settings.allow_private_tabs);
  state.Set("configured", settings.Configured());
  state.Set("backend", settings.backend);
  state.Set("localPath", settings.local_path);
  state.Set("localModel", settings.local_model);
  if (const std::string* backend = local_.FindString("backend");
      backend && *backend == settings.backend) {
    state.Set("local", local_.Clone());
  }
  // 外部智能体：开关、套接字的状态、已连接的、连接命令。
  const agent::AgentService::Status agents =
      agent::AgentService::Get()->GetStatus();
  base::ListValue clients;
  for (const std::string& client : agents.clients) {
    clients.Append(client);
  }
  state.Set("allowExternal",
            prefs::PrefsFor(profile_)->GetBoolean(prefs::kAgentAllowExternal));
  state.Set("agentError", agents.error);
  state.Set("agentClients", std::move(clients));
  base::FilePath exe;
  base::PathService::Get(base::FILE_EXE, &exe);
  const std::vector<std::string> arguments = BridgeArguments();
  std::string command = ShellQuote(exe.AsUTF8Unsafe());
  base::ListValue args;
  for (const std::string& argument : arguments) {
    command += " " + ShellQuote(argument);
    args.Append(argument);
  }
#if BUILDFLAG(IS_WIN)
  state.Set("connectCommand", "& " + command);
#else
  state.Set("connectCommand", command);
#endif
  state.Set("connectClaude",
            "claude mcp add --scope user starry -- " + command);
  state.Set("connectJson",
            base::WriteJsonWithOptions(
                base::DictValue().Set(
                    "mcpServers",
                    base::DictValue().Set(
                        "starry", base::DictValue()
                                      .Set("command", exe.AsUTF8Unsafe())
                                      .Set("args", std::move(args)))),
                base::JSONWriter::OPTIONS_PRETTY_PRINT)
                .value_or("{}"));
  return state;
}

void AssistantSettingsHandler::Changed() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("assistant-settings-changed", base::Value(State()));
  }
  // 各个窗口的助手：设置引导、模型名跟着变。
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetProfile()->GetOriginalProfile() !=
        profile_->GetOriginalProfile()) {
      continue;
    }
    StarryBrowserView* view = StarryBrowserView::From(browser);
    if (view && view->assistant_island()) {
      view->assistant_island()->controller()->SettingsChanged();
    }
  }
}

void AssistantSettingsHandler::HandleGet(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(State()));
}

void AssistantSettingsHandler::HandleSet(const base::ListValue& args) {
  if (args.size() < 2 || !args[0].is_string()) {
    return;
  }
  const std::string& key = args[0].GetString();
  const base::Value& value = args[1];
  const assistant::Settings settings = assistant::ReadSettings(profile_);
  if (key == "enabled" && value.is_bool()) {
    // 各个窗口自己看这一项：岛收起来并结束对话，✦ 藏起来。
    assistant::SetEnabled(profile_, value.GetBool());
  } else if (key == "provider" && value.is_string()) {
    assistant::SetProvider(profile_, value.GetString());
  } else if (key == "customUrl" && value.is_string()) {
    assistant::SetCustomBaseUrl(profile_, value.GetString());
  } else if (key == "model" && value.is_string() &&
             !settings.provider_id.empty()) {
    assistant::SetModel(profile_, settings.provider_id, value.GetString());
  } else if (key == "mode" && value.is_string()) {
    assistant::SetMode(profile_, value.GetString());
  } else if (key == "panelStyle" && value.is_string()) {
    assistant::SetPanelStyle(profile_, value.GetString());
  } else if (key == "allowScripts" && value.is_bool()) {
    assistant::SetAllowScripts(profile_, value.GetBool());
    agent::AgentService::Get()->SettingsChanged();
  } else if (key == "allowUploads" && value.is_bool()) {
    assistant::SetAllowUploads(profile_, value.GetBool());
    agent::AgentService::Get()->SettingsChanged();
  } else if (key == "allowPrivateTabs" && value.is_bool()) {
    // 关掉时已经开着的无痕窗口也关掉（AgentService::SettingsChanged）。
    assistant::SetAllowPrivateTabs(profile_, value.GetBool());
    agent::AgentService::Get()->SettingsChanged();
  } else if (key == "backend" && value.is_string()) {
    assistant::SetBackend(profile_, value.GetString());
    CheckLocalAgent();
  } else if (key == "localPath" && value.is_string() && settings.is_local()) {
    assistant::SetLocalPath(profile_, settings.backend, value.GetString());
    CheckLocalAgent();
  } else if (key == "localModel" && value.is_string() && settings.is_local()) {
    assistant::SetLocalModel(profile_, settings.backend, value.GetString());
  } else if (key == "allowExternal" && value.is_bool()) {
    prefs::PrefsFor(profile_)->SetBoolean(prefs::kAgentAllowExternal,
                                          value.GetBool());
    agent::AgentService::Get()->SettingsChanged();
  }
}

void AssistantSettingsHandler::HandleCheckLocalAgent(
    const base::ListValue& args) {
  AllowJavascript();
  CheckLocalAgent();
}

void AssistantSettingsHandler::HandleDisconnectAgents(
    const base::ListValue& args) {
  agent::AgentService::Get()->DisconnectExternal();
}

void AssistantSettingsHandler::CheckLocalAgent() {
  const assistant::Settings settings = assistant::ReadSettings(profile_);
  std::optional<assistant::LocalAgentKind> kind =
      assistant::LocalAgentFromBackend(settings.backend);
  if (!kind) {
    local_ = base::DictValue();
    Changed();
    return;
  }
  local_ =
      base::DictValue().Set("backend", settings.backend).Set("checking", true);
  Changed();
  // 找可执行文件、看登没登录都要跑子进程：在后台做。
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](assistant::LocalAgentKind kind, std::string configured) {
            base::DictValue result;
            const base::FilePath path =
                assistant::LocateLocalAgent(kind, configured);
            if (path.empty()) {
              return result;
            }
            base::FilePath shown(FILE_PATH_LITERAL("~"));
            if (!base::GetHomeDir().AppendRelativePath(path, &shown)) {
              shown = path;
            }
            result.Set("path", shown.AsUTF8Unsafe());
            if (std::optional<bool> logged_in =
                    assistant::LocalAgentLoggedIn(kind, path)) {
              result.Set("loggedIn", *logged_in);
            }
            return result;
          },
          *kind, settings.local_path),
      base::BindOnce(
          [](base::WeakPtr<AssistantSettingsHandler> handler,
             std::string backend, base::DictValue result) {
            if (!handler) {
              return;
            }
            result.Set("backend", backend);
            result.Set("checking", false);
            handler->local_ = std::move(result);
            handler->Changed();
          },
          weak_factory_.GetWeakPtr(), settings.backend));
}

void AssistantSettingsHandler::HandleSetKey(const base::ListValue& args) {
  AllowJavascript();
  if (args.size() < 2 || !args[1].is_string()) {
    return;
  }
  const std::string provider = assistant::ReadSettings(profile_).provider_id;
  if (provider.empty()) {
    ResolveJavascriptCallback(args[0], base::Value(State()));
    return;
  }
  assistant::SetApiKey(
      profile_, provider, args[1].GetString(),
      base::BindOnce(
          [](base::WeakPtr<AssistantSettingsHandler> handler,
             base::Value callback_id) {
            if (handler && handler->IsJavascriptAllowed()) {
              handler->ResolveJavascriptCallback(
                  callback_id, base::Value(handler->State()));
            }
          },
          weak_factory_.GetWeakPtr(), args[0].Clone()));
}

void AssistantSettingsHandler::HandleFetchModels(const base::ListValue& args) {
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  const assistant::Settings settings = assistant::ReadSettings(profile_);
  assistant::GetApiKey(
      profile_, settings.provider_id,
      base::BindOnce(&AssistantSettingsHandler::FetchModelsWithKey,
                     weak_factory_.GetWeakPtr(), callback_id));
}

void AssistantSettingsHandler::FetchModelsWithKey(std::string callback_id,
                                                  std::string key) {
  const assistant::Settings settings = assistant::ReadSettings(profile_);
  const GURL url =
      assistant::ProviderEndpoint(settings.provider.base_url, "models");
  if (url.is_empty()) {
    ResolveJavascriptCallback(
        base::Value(callback_id),
        base::Value(base::DictValue().Set(
            "error", "The service address is not a valid URL.")));
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  if (!key.empty()) {
    request->headers.SetHeader("Authorization",
                               base::StrCat({"Bearer ", key}));
  }
  for (const auto& [name, value] : settings.provider.headers) {
    request->headers.SetHeader(name, value);
  }
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kModelsAnnotation);
  loader_->SetTimeoutDuration(base::Seconds(20));
  loader_->SetAllowHttpErrorResults(true);
  loader_->DownloadToString(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(&AssistantSettingsHandler::ModelsFetched,
                     weak_factory_.GetWeakPtr(), callback_id),
      1 << 22);
}

void AssistantSettingsHandler::ModelsFetched(std::string callback_id,
                                             std::optional<std::string> body) {
  const int status = loader_ && loader_->ResponseInfo() &&
                             loader_->ResponseInfo()->headers
                         ? loader_->ResponseInfo()->headers->response_code()
                         : 0;
  loader_.reset();
  base::DictValue result;
  std::optional<base::Value> value =
      body ? base::JSONReader::Read(*body, base::JSON_PARSE_RFC) : std::nullopt;
  if (!body || status < 200 || status >= 300 || !value) {
    std::string reason = status ? base::StrCat({"HTTP ",
                                                base::NumberToString(status)})
                                : std::string("network error");
    if (value && value->is_dict()) {
      if (const base::DictValue* error = value->GetDict().FindDict("error")) {
        if (const std::string* message = error->FindString("message")) {
          reason = *message;
        }
      }
    }
    result.Set("error", reason);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  // {"data": [...]}；Ollama 等也有直接是数组或者 {"models": [...]} 的。
  const base::ListValue* items = nullptr;
  if (value->is_list()) {
    items = &value->GetList();
  } else if (value->is_dict()) {
    items = value->GetDict().FindList("data");
    if (!items) {
      items = value->GetDict().FindList("models");
    }
  }
  base::ListValue models;
  if (items) {
    for (const base::Value& item : *items) {
      if (!item.is_dict()) {
        continue;
      }
      const base::DictValue& dict = item.GetDict();
      const std::string* id = dict.FindString("id");
      if (!id) {
        id = dict.FindString("name");
      }
      if (!id || id->empty()) {
        continue;
      }
      base::DictValue model;
      model.Set("id", *id);
      if (const base::ListValue* parameters =
              dict.FindList("supported_parameters")) {
        model.Set("tools", std::ranges::find(*parameters, base::Value("tools")) != parameters->end());
      }
      if (const base::DictValue* architecture = dict.FindDict("architecture")) {
        if (const base::ListValue* modalities =
                architecture->FindList("input_modalities")) {
          model.Set("vision",
                    std::ranges::find(*modalities, base::Value("image")) != modalities->end());
        }
      }
      models.Append(std::move(model));
    }
  }
  result.Set("models", std::move(models));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

}  // namespace starry
