// Copyright 2026 The Starry Browser Authors

#include <windows.h>

#include <array>
#include <cstdlib>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/threading/platform_thread.h"
#include "base/win/scoped_handle.h"
#include "chrome/browser/win/chrome_process_finder.h"
#include "chrome/common/chrome_switches.h"
#include "starry/browser/agent/agent_bridge_platform.h"
#include "starry/browser/agent/agent_socket.h"

namespace starry::agent::bridge {
namespace {

bool Write(HANDLE output, std::string_view data, OVERLAPPED* io) {
  while (!data.empty()) {
    DWORD bytes = 0;
    if (!::WriteFile(output, data.data(), static_cast<DWORD>(data.size()),
                     &bytes, io)) {
      if (!io || ::GetLastError() != ERROR_IO_PENDING ||
          !::GetOverlappedResult(output, io, &bytes, TRUE)) {
        return false;
      }
    }
    if (!bytes) {
      return false;
    }
    data.remove_prefix(bytes);
  }
  return true;
}

// The MCP host normally supplies synchronous standard handles. One thread per
// direction lets either EOF terminate this dedicated bridge process.
class InputPump final : public base::PlatformThread::Delegate {
 public:
  explicit InputPump(HANDLE socket) : socket_(socket) {}
  void ThreadMain() override {
    base::win::ScopedHandle event(
        ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::array<char, 65536> buffer;
    while (event.is_valid()) {
      int bytes = ReadInput(buffer);
      OVERLAPPED io{};
      io.hEvent = event.get();
      if (bytes <= 0 ||
          !Write(socket_, std::string_view(buffer.data(), bytes), &io)) {
        break;
      }
    }
    std::_Exit(0);
  }

 private:
  HANDLE socket_;
};

}  // namespace

bool BrowserRunning(const base::FilePath& dir) {
  return FindRunningChromeWindow(dir) != nullptr;
}

void LaunchBrowser(const base::FilePath& dir, bool custom) {
  // An internal bridge belongs to a running conversation. Never launch a
  // browser inside the CLI's job if that conversation has already gone away.
  if (base::Environment::Create()->HasVar(kAssistantTokenVariable)) {
    return;
  }
  base::FilePath exe;
  if (base::PathService::Get(base::FILE_EXE, &exe)) {
    base::CommandLine command(exe);
    if (custom) {
      command.AppendSwitchPath(switches::kUserDataDir, dir);
    }
    base::LaunchProcess(command, {});
  }
}

int ReadInput(base::span<char> buffer) {
  DWORD bytes = 0;
  return ::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buffer.data(),
                    static_cast<DWORD>(buffer.size()), &bytes, nullptr)
             ? bytes
             : 0;
}

void WriteOutput(std::string_view data, bool diagnostic) {
  if (!Write(::GetStdHandle(diagnostic ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE),
             data, nullptr) &&
      !diagnostic) {
    std::_Exit(0);
  }
}

bool WriteSocket(HANDLE socket, std::string_view data) {
  base::win::ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  OVERLAPPED io{};
  io.hEvent = event.get();
  return event.is_valid() && Write(socket, data, &io);
}

[[noreturn]] void Pump(HANDLE socket) {
  InputPump input(socket);
  base::win::ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.is_valid() ||
      !base::PlatformThread::CreateNonJoinable(0, &input)) {
    std::_Exit(1);
  }
  std::array<char, 65536> buffer;
  for (;;) {
    OVERLAPPED io{};
    io.hEvent = event.get();
    DWORD bytes = 0;
    if (!::ReadFile(socket, buffer.data(), buffer.size(), &bytes, &io) &&
        (::GetLastError() != ERROR_IO_PENDING ||
         !::GetOverlappedResult(socket, &io, &bytes, TRUE))) {
      break;
    }
    if (!bytes) {
      break;
    }
    WriteOutput(std::string_view(buffer.data(), bytes), false);
  }
  std::_Exit(0);
}

}  // namespace starry::agent::bridge
