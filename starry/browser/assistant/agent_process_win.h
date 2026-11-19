// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_AGENT_PROCESS_WIN_H_
#define STARRY_BROWSER_ASSISTANT_AGENT_PROCESS_WIN_H_

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/win/scoped_handle.h"

namespace starry::assistant {
std::wstring AgentCommand(const base::FilePath& program,
                          const std::vector<std::string>& arguments);
base::win::ScopedHandle CreateAgentJob();
}  // namespace starry::assistant
#endif  // STARRY_BROWSER_ASSISTANT_AGENT_PROCESS_WIN_H_
