// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/assistant_backend.h"

#include "base/no_destructor.h"

namespace starry::assistant {

const std::vector<std::string>& ClaudeCodeReasoningLevels() {
  static const base::NoDestructor<std::vector<std::string>> levels(
      std::vector<std::string>{"low", "medium", "high", "xhigh", "max"});
  return *levels;
}

const std::vector<std::string>& CodexFallbackReasoningLevels() {
  static const base::NoDestructor<std::vector<std::string>> levels(
      std::vector<std::string>{"low", "medium", "high", "xhigh"});
  return *levels;
}

UserMessage::UserMessage() = default;
UserMessage::UserMessage(UserMessage&&) = default;
UserMessage& UserMessage::operator=(UserMessage&&) = default;
UserMessage::~UserMessage() = default;

}  // namespace starry::assistant
