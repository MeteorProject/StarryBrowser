// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_LOCAL_AGENT_PLATFORM_H_
#define STARRY_BROWSER_ASSISTANT_LOCAL_AGENT_PLATFORM_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"

namespace starry::assistant {
struct ProbeOutput {
  int status = -1;
  std::string text;
};
std::optional<ProbeOutput> RunShort(const base::FilePath& executable,
                                    const std::vector<std::string>& arguments,
                                    base::TimeDelta timeout);
}  // namespace starry::assistant
#endif  // STARRY_BROWSER_ASSISTANT_LOCAL_AGENT_PLATFORM_H_
