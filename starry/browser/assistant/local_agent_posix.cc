// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/local_agent.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include "base/environment.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "starry/browser/assistant/line_process.h"
#include "starry/browser/assistant/local_agent_platform.h"

namespace starry::assistant {

// 跑一个短命令，拿标准输出。超时或者起不来是 nullopt。
std::optional<ProbeOutput> RunShort(const base::FilePath& executable,
                                    const std::vector<std::string>& arguments,
                                    base::TimeDelta timeout) {
  int fds[2];
  if (pipe(fds) != 0) {
    return std::nullopt;
  }
  base::ScopedFD read_end(fds[0]);
  base::ScopedFD write_end(fds[1]);
  std::vector<std::string> argv = {executable.value()};
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  base::LaunchOptions options;
  options.environment = ChildEnvironment(executable);
  options.clear_environment = true;
  base::FilePath temp;
  options.current_directory =
      base::GetTempDir(&temp) ? temp : base::FilePath("/tmp");
  options.fds_to_remap.emplace_back(write_end.get(), STDOUT_FILENO);
  base::Process process = base::LaunchProcess(argv, options);
  write_end.reset();
  if (!process.IsValid()) {
    return std::nullopt;
  }
  // 输出多时管道会写满：边等边读。进程卡住时这里也不跟着卡。
  ProbeOutput output;
  const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
  char buffer[4096];
  while (true) {
    const base::TimeDelta left = deadline - base::TimeTicks::Now();
    if (left <= base::TimeDelta()) {
      process.Terminate(0, false);
      return std::nullopt;
    }
    pollfd request = {read_end.get(), POLLIN, 0};
    const int ready = HANDLE_EINTR(
        poll(&request, 1, static_cast<int>(left.InMilliseconds())));
    if (ready <= 0) {
      continue;
    }
    const ssize_t count =
        HANDLE_EINTR(read(read_end.get(), buffer, sizeof(buffer)));
    if (count <= 0) {
      break;
    }
    output.text.append(buffer, static_cast<size_t>(count));
  }
  if (!process.WaitForExitWithTimeout(
          std::max(deadline - base::TimeTicks::Now(), base::Seconds(1)),
          &output.status)) {
    process.Terminate(0, false);
    return std::nullopt;
  }
  return output;
}

namespace {

base::FilePath Home() {
  return base::GetHomeDir();
}

base::FilePath Expand(const std::string& path) {
  if (path == "~") {
    return Home();
  }
  if (base::StartsWith(path, "~/")) {
    return Home().Append(path.substr(2));
  }
  return base::FilePath(path);
}

bool IsExecutable(const base::FilePath& path) {
  return !path.empty() && access(path.value().c_str(), X_OK) == 0 &&
         !base::DirectoryExists(path);
}

base::FilePath LoginShellLookup(const std::string& name) {
  std::optional<std::string> shell =
      base::Environment::Create()->GetVar("SHELL");
  const base::FilePath executable(shell && !shell->empty() ? *shell
                                                           : "/bin/zsh");
  std::optional<ProbeOutput> output = RunShort(
      executable, {"-l", "-i", "-c", "command -v " + name}, base::Seconds(5));
  if (!output) {
    return base::FilePath();
  }
  std::vector<std::string> lines = base::SplitString(
      output->text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (base::StartsWith(*it, "/") && IsExecutable(base::FilePath(*it))) {
      return base::FilePath(*it);
    }
  }
  return base::FilePath();
}

}  // namespace

base::FilePath LocateLocalAgent(LocalAgentKind kind,
                                const std::string& configured) {
  if (!configured.empty()) {
    const base::FilePath path = Expand(configured);
    return IsExecutable(path) ? path : base::FilePath();
  }
  const std::string name = ExecutableName(kind);
  std::vector<std::string> candidates = {
      "~/.local/bin/" + name,   "/opt/homebrew/bin/" + name,
      "/usr/local/bin/" + name, "~/.npm-global/bin/" + name,
      "~/.bun/bin/" + name,     "~/.volta/bin/" + name,
      "~/Library/pnpm/" + name,
  };
  if (kind == LocalAgentKind::kClaudeCode) {
    candidates.insert(candidates.begin() + 1, "~/.claude/local/" + name);
  }
  for (const std::string& candidate : candidates) {
    const base::FilePath path = Expand(candidate);
    if (IsExecutable(path)) {
      return path;
    }
  }
  // nvm 的每个版本各有一个 bin：新的在前。
  std::vector<base::FilePath> versions;
  base::FileEnumerator nvm(Home().Append(".nvm/versions/node"), false,
                           base::FileEnumerator::DIRECTORIES);
  for (base::FilePath dir = nvm.Next(); !dir.empty(); dir = nvm.Next()) {
    versions.push_back(dir);
  }
  std::sort(versions.rbegin(), versions.rend());
  for (const base::FilePath& version : versions) {
    const base::FilePath path = version.Append("bin").Append(name);
    if (IsExecutable(path)) {
      return path;
    }
  }
  return LoginShellLookup(name);
}

}  // namespace starry::assistant
