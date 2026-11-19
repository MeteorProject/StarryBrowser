// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/line_process.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include "base/compiler_specific.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/process/launch.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "starry/browser/agent/line_channel.h"

extern char** environ;

namespace starry::assistant {

namespace {

bool MakePipe(base::ScopedFD* read_end, base::ScopedFD* write_end) {
  int fds[2];
  if (pipe(fds) != 0) {
    return false;
  }
  read_end->reset(fds[0]);
  write_end->reset(fds[1]);
  return true;
}

}  // namespace

LineProcess::~LineProcess() {
  on_exit_.Reset();
  if (running()) {
    const pid_t pid = process_.Pid();
    process_.Terminate(0, /*wait=*/false);
    // 不肯退的 2 秒后 SIGKILL（这时 LineProcess 已经没了）。
    base::ThreadPool::PostDelayedTask(
        FROM_HERE, {base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(
            [](pid_t pid) {
              if (kill(pid, 0) == 0) {
                kill(pid, SIGKILL);
              }
            },
            pid),
        base::Seconds(2));
  }
}

std::unique_ptr<LineProcess::Launched> LineProcess::Launch(Options options) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  auto launched = std::make_unique<Launched>();
  base::ScopedFD input_read, output_write, errors_write;
  if (!MakePipe(&input_read, &launched->input) ||
      !MakePipe(&launched->output, &output_write) ||
      !MakePipe(&launched->errors, &errors_write)) {
    launched->error = "Could not create pipes.";
    return launched;
  }
  if (!base::PathExists(options.executable)) {
    launched->error = "No such file: " + options.executable.value();
    return launched;
  }
  std::vector<std::string> argv = {options.executable.value()};
  argv.insert(argv.end(), options.arguments.begin(), options.arguments.end());
  base::LaunchOptions launch;
  launch.environment = options.environment;
  launch.clear_environment = true;
  launch.current_directory = options.directory;
  launch.fds_to_remap.emplace_back(input_read.get(), STDIN_FILENO);
  launch.fds_to_remap.emplace_back(output_write.get(), STDOUT_FILENO);
  launch.fds_to_remap.emplace_back(errors_write.get(), STDERR_FILENO);
  launched->process = base::LaunchProcess(argv, launch);
  if (!launched->process.IsValid()) {
    launched->error = "Could not start " + options.executable.value();
  }
  return launched;
}

void LineProcess::Interrupt() {
  if (running()) {
    kill(process_.Pid(), SIGINT);
  }
}

void LineProcess::Terminate() {
  if (!running()) {
    return;
  }
  process_.Terminate(0, /*wait=*/false);
  kill_timer_.Start(FROM_HERE, base::Seconds(2),
                    base::BindOnce(
                        [](base::WeakPtr<LineProcess> self) {
                          if (self && self->running()) {
                            kill(self->process_.Pid(), SIGKILL);
                          }
                        },
                        weak_factory_.GetWeakPtr()));
}

base::EnvironmentMap ChildEnvironment(const base::FilePath& executable) {
  base::EnvironmentMap environment;
  // environ 是 C 的数组，以空指针结尾。
  for (char** entry = environ; entry && *entry; UNSAFE_BUFFERS(++entry)) {
    std::string_view pair(*entry);
    const size_t equals = pair.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      continue;
    }
    environment[std::string(pair.substr(0, equals))] =
        std::string(pair.substr(equals + 1));
  }
  std::vector<std::string> paths = base::SplitString(
      environment.contains("PATH") ? environment["PATH"]
                                   : "/usr/bin:/bin:/usr/sbin:/sbin",
      ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  const std::string home = environment.contains("HOME")
                               ? environment["HOME"]
                               : base::GetHomeDir().value();
  std::vector<std::string> extra = {home + "/.local/bin", "/opt/homebrew/bin",
                                    "/usr/local/bin"};
  if (!executable.empty()) {
    extra.insert(extra.begin(), executable.DirName().value());
  }
  for (auto it = extra.rbegin(); it != extra.rend(); ++it) {
    if (std::find(paths.begin(), paths.end(), *it) == paths.end()) {
      paths.insert(paths.begin(), *it);
    }
  }
  environment["PATH"] = base::JoinString(paths, ":");
  // 浏览器自己的调试变量不要传下去。
  environment.erase("OS_ACTIVITY_DT_MODE");
  return environment;
}

}  // namespace starry::assistant
