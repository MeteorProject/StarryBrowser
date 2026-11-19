// Copyright 2026 The Starry Browser Authors

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <array>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "starry/browser/agent/agent_bridge_platform.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#endif

namespace starry::agent::bridge {
namespace {
bool WriteAll(int fd, std::string_view data) {
  while (!data.empty()) {
    const ssize_t written = HANDLE_EINTR(write(fd, data.data(), data.size()));
    if (written <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<size_t>(written));
  }
  return true;
}

}  // namespace

// 浏览器在不在运行：Chrome 的单实例锁（数据目录里的 SingletonLock，指向
// 「主机名-进程号」）。
bool BrowserRunning(const base::FilePath& user_data_dir) {
  base::FilePath target;
  if (!base::ReadSymbolicLink(user_data_dir.Append("SingletonLock"), &target)) {
    return false;
  }
  const std::string value = target.value();
  const size_t dash = value.rfind('-');
  int pid = 0;
  if (dash == std::string::npos ||
      !base::StringToInt(std::string_view(value).substr(dash + 1), &pid) ||
      pid <= 0) {
    return false;
  }
  return kill(pid, 0) == 0 || errno == EPERM;
}

// 在后台启动浏览器（不抢前台）。换了数据目录的是另一个实例，要另开一个进程。
void LaunchBrowser(const base::FilePath& user_data_dir, bool custom) {
  std::vector<std::string> argv;
#if BUILDFLAG(IS_MAC)
  argv = {"/usr/bin/open"};
  if (custom) {
    argv.push_back("-n");
  }
  argv.insert(argv.end(), {"-g", "-a", base::apple::OuterBundlePath().value()});
  if (custom) {
    argv.insert(argv.end(),
                {"--args", "--user-data-dir=" + user_data_dir.value()});
  }
#else
  base::FilePath exe;
  if (!base::ReadSymbolicLink(base::FilePath("/proc/self/exe"), &exe)) {
    return;
  }
  argv = {exe.value()};
  if (custom) {
    argv.push_back("--user-data-dir=" + user_data_dir.value());
  }
#endif
  base::LaunchOptions options;
  options.fds_to_remap.emplace_back(STDERR_FILENO, STDERR_FILENO);
  // 标准输出只能有 MCP 消息：不交给它。
  base::Process process = base::LaunchProcess(argv, options);
#if BUILDFLAG(IS_MAC)
  if (process.IsValid()) {
    int code = 0;
    process.WaitForExit(&code);
  }
#endif
}

int ReadInput(base::span<char> buffer) {
  return HANDLE_EINTR(read(STDIN_FILENO, buffer.data(), buffer.size()));
}

void WriteOutput(std::string_view data, bool diagnostic) {
  WriteAll(diagnostic ? STDERR_FILENO : STDOUT_FILENO, data);
}

bool WriteSocket(base::PlatformFile fd, std::string_view data) {
  return WriteAll(fd, data);
}

// 两个方向一起转：任一边结束（客户端关了标准输入、浏览器断开或退出）就退出。
[[noreturn]] void Pump(int socket) {
  char buffer[65536];
  std::array<pollfd, 2> fds = {
      {{STDIN_FILENO, POLLIN, 0}, {socket, POLLIN, 0}}};
  while (true) {
    if (HANDLE_EINTR(poll(fds.data(), fds.size(), -1)) < 0) {
      _exit(1);
    }
    for (size_t i = 0; i < fds.size(); ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
        continue;
      }
      const ssize_t count =
          HANDLE_EINTR(read(fds[i].fd, buffer, sizeof(buffer)));
      if (count <= 0 ||
          !WriteAll(i == 0 ? socket : STDOUT_FILENO,
                    std::string_view(buffer, static_cast<size_t>(count)))) {
        _exit(0);
      }
    }
  }
}

}  // namespace starry::agent::bridge
