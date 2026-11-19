// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/local_agent.h"

#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/process/launch.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "starry/browser/assistant/agent_process_win.h"
#include "starry/browser/assistant/line_process.h"
#include "starry/browser/assistant/local_agent_platform.h"

namespace starry::assistant {
namespace {
bool IsExecutable(const base::FilePath& path) {
  return path.IsAbsolute() && path.MatchesExtension(L".exe") &&
         base::PathExists(path) && !base::DirectoryExists(path);
}
}  // namespace

base::FilePath LocateLocalAgent(LocalAgentKind kind,
                                const std::string& configured) {
  const auto home = base::GetHomeDir();
  if (!configured.empty()) {
    auto path = base::FilePath::FromUTF8Unsafe(configured);
    if (configured.starts_with("~/") || configured.starts_with("~\\")) {
      auto relative = base::FilePath::FromUTF8Unsafe(configured.substr(2));
      if (relative.IsAbsolute() || relative.value().starts_with(L"\\")) {
        return {};
      }
      path = home.Append(relative);
    }
    return IsExecutable(path) ? path : base::FilePath();
  }
  const std::string exe = std::string(ExecutableName(kind)) + ".exe";
  auto candidate =
      home.AppendASCII(".local").AppendASCII("bin").AppendASCII(exe);
  if (IsExecutable(candidate)) {
    return candidate;
  }
  const auto paths = base::Environment::Create()->GetVar("PATH").value_or("");
  for (std::string_view dir : base::SplitStringPiece(
           paths, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
      dir = dir.substr(1, dir.size() - 2);
    }
    candidate = base::FilePath::FromUTF8Unsafe(dir).AppendASCII(exe);
    if (IsExecutable(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::optional<ProbeOutput> RunShort(const base::FilePath& executable,
                                    const std::vector<std::string>& arguments,
                                    base::TimeDelta timeout) {
  auto job = CreateAgentJob();
  if (!job.is_valid()) {
    return std::nullopt;
  }
  base::LaunchOptions options;
  options.job_handle = job.get();
  options.start_hidden = true;
  options.clear_environment = true;
  options.environment = ChildEnvironment(executable);
  if (!base::GetTempDir(&options.current_directory)) {
    return std::nullopt;
  }
  ProbeOutput output;
  if (!base::GetAppOutputWithExitCodeAndTimeout(
          AgentCommand(executable, arguments), false, &output.text,
          &output.status, timeout, options)) {
    return std::nullopt;
  }
  return output;
}
}  // namespace starry::assistant
