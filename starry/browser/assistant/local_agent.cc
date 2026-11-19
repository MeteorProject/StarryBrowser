// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/local_agent.h"

#include <algorithm>
#include <utility>

#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "base/values.h"
#include "starry/browser/assistant/codex_backend.h"
#include "starry/browser/assistant/line_process.h"
#include "starry/browser/assistant/local_agent_platform.h"

namespace starry::assistant {

BrowserMcpServer::BrowserMcpServer() = default;
BrowserMcpServer::BrowserMcpServer(const BrowserMcpServer&) = default;
BrowserMcpServer& BrowserMcpServer::operator=(const BrowserMcpServer&) =
    default;
BrowserMcpServer::~BrowserMcpServer() = default;

std::string BrowserToolName(const std::string& name) {
  const std::string prefix =
      std::string("mcp__") + kBrowserMcpServerName + "__";
  return base::StartsWith(name, prefix) ? name.substr(prefix.size()) : name;
}

bool LooksLikeAuthentication(const std::string& message) {
  const std::string text = base::ToLowerASCII(message);
  for (const char* hint : {"/login", "not logged in", "invalid api key",
                           "authentication", "oauth", "401"}) {
    if (text.find(hint) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::optional<LocalAgentKind> LocalAgentFromBackend(
    const std::string& backend) {
  if (backend == "claudeCode") {
    return LocalAgentKind::kClaudeCode;
  }
  if (backend == "codex") {
    return LocalAgentKind::kCodex;
  }
  return std::nullopt;
}

const char* BackendName(LocalAgentKind kind) {
  return kind == LocalAgentKind::kClaudeCode ? "claudeCode" : "codex";
}

const char* ExecutableName(LocalAgentKind kind) {
  return kind == LocalAgentKind::kClaudeCode ? "claude" : "codex";
}

std::u16string DisplayName(LocalAgentKind kind) {
  return kind == LocalAgentKind::kClaudeCode ? u"Claude Code" : u"Codex";
}

std::optional<bool> LocalAgentLoggedIn(LocalAgentKind kind,
                                       const base::FilePath& executable) {
  if (kind == LocalAgentKind::kClaudeCode) {
    std::optional<ProbeOutput> output =
        RunShort(executable, {"auth", "status", "--json"}, base::Seconds(10));
    if (!output) {
      return std::nullopt;
    }
    std::optional<base::Value> value =
        base::JSONReader::Read(output->text, base::JSON_PARSE_RFC);
    if (!value || !value->is_dict()) {
      return std::nullopt;
    }
    return value->GetDict().FindBool("loggedIn");
  }
  std::optional<ProbeOutput> output =
      RunShort(executable, {"login", "status"}, base::Seconds(10));
  if (!output) {
    return std::nullopt;
  }
  return output->status == 0;
}

std::set<std::string> CodexFeatures(const base::FilePath& executable) {
  std::set<std::string> features;
  std::optional<ProbeOutput> output =
      RunShort(executable, {"features", "list"}, base::Seconds(10));
  if (!output) {
    return features;
  }
  for (const std::string& line :
       base::SplitString(output->text, "\n", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    std::vector<std::string> columns = base::SplitString(
        line, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (!columns.empty()) {
      features.insert(columns[0]);
    }
  }
  return features;
}

std::vector<std::string> CodexMcpServers(const std::string& config_text) {
  std::vector<std::string> names;
  constexpr std::string_view kPrefix = "[mcp_servers.";
  for (const std::string& raw :
       base::SplitString(config_text, "\n", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    std::string_view line = raw;
    if (!base::StartsWith(line, kPrefix) || !base::EndsWith(line, "]") ||
        base::StartsWith(line, "[[")) {
      continue;
    }
    std::string_view key =
        line.substr(kPrefix.size(), line.size() - kPrefix.size() - 1);
    std::string name;
    if (base::StartsWith(key, "\"")) {
      // 带引号的名字：到下一个引号为止，后面不能再有子表。
      const size_t end = key.find('"', 1);
      if (end == std::string_view::npos || end + 1 != key.size()) {
        continue;
      }
      name = std::string(key.substr(1, end - 1));
    } else {
      if (key.find('.') != std::string_view::npos) {
        continue;
      }
      name = std::string(key);
    }
    if (!name.empty() && std::ranges::find(names, name) == names.end()) {
      names.push_back(name);
    }
  }
  return names;
}

base::FilePath CodexConfigFile() {
  std::optional<std::string> home =
      base::Environment::Create()->GetVar("CODEX_HOME");
  const base::FilePath dir = home && !home->empty()
                                 ? base::FilePath::FromUTF8Unsafe(*home)
                                 : base::GetHomeDir().AppendASCII(".codex");
  return dir.AppendASCII("config.toml");
}

LocalLaunch::LocalLaunch() = default;
LocalLaunch::LocalLaunch(LocalLaunch&&) = default;
LocalLaunch& LocalLaunch::operator=(LocalLaunch&&) = default;
LocalLaunch::~LocalLaunch() = default;

LocalLaunch PrepareLocalLaunch(LocalAgentKind kind,
                               std::string configured,
                               base::FilePath user_data) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  LocalLaunch launch;
  launch.executable = LocateLocalAgent(kind, configured);
  if (launch.executable.empty()) {
    return launch;
  }
  launch.environment = ChildEnvironment(launch.executable);
  launch.workspace =
      user_data.AppendASCII("Assistant").AppendASCII("Workspace");
  base::CreateDirectory(launch.workspace);
  if (kind == LocalAgentKind::kCodex) {
    const std::set<std::string> known = CodexFeatures(launch.executable);
    for (const std::string& feature : CodexBackend::UnwantedFeatures()) {
      if (known.contains(feature)) {
        launch.disabled_features.push_back(feature);
      }
    }
    std::string config;
    if (base::ReadFileToString(CodexConfigFile(), &config)) {
      launch.disabled_servers = CodexMcpServers(config);
    }
  }
  return launch;
}

}  // namespace starry::assistant
