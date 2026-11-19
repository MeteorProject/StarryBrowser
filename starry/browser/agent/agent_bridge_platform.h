// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_AGENT_BRIDGE_PLATFORM_H_
#define STARRY_BROWSER_AGENT_AGENT_BRIDGE_PLATFORM_H_

#include <string_view>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/platform_file.h"

namespace starry::agent::bridge {
bool BrowserRunning(const base::FilePath& dir);
void LaunchBrowser(const base::FilePath& dir, bool custom);
int ReadInput(base::span<char> buffer);
void WriteOutput(std::string_view data, bool diagnostic);
bool WriteSocket(base::PlatformFile socket, std::string_view data);
[[noreturn]] void Pump(base::PlatformFile socket);
}  // namespace starry::agent::bridge

#endif  // STARRY_BROWSER_AGENT_AGENT_BRIDGE_PLATFORM_H_
