// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/line_channel_reader.h"

#include "base/functional/bind.h"
#include "starry/browser/agent/line_channel.h"

namespace starry::agent {
LineChannelReader::LineChannelReader(
    scoped_refptr<base::SequencedTaskRunner> owner,
    base::RepeatingCallback<void(std::string)> on_line,
    base::OnceClosure on_closed)
    : owner_(std::move(owner)),
      on_line_(std::move(on_line)),
      on_closed_(std::move(on_closed)) {}

LineChannelReader::~LineChannelReader() = default;

bool LineChannelReader::Append(std::string_view bytes) {
  while (!bytes.empty()) {
    const size_t end = bytes.find('\n');
    const std::string_view part = bytes.substr(0, end);
    if (part.size() > LineChannel::kMaxLineLength - line_.size()) {
      return false;
    }
    line_.append(part);
    if (end == std::string_view::npos) {
      break;
    }
    if (!line_.empty() && line_.back() == '\r') {
      line_.pop_back();
    }
    Deliver();
    bytes.remove_prefix(end + 1);
  }
  return true;
}

void LineChannelReader::Finish(bool deliver_tail) {
  if (deliver_tail) {
    Deliver();
  }
  if (on_closed_) {
    owner_->PostTask(FROM_HERE, std::move(on_closed_));
  }
}

void LineChannelReader::Deliver() {
  if (!line_.empty()) {
    owner_->PostTask(FROM_HERE, base::BindOnce(on_line_, std::move(line_)));
    line_.clear();
  }
}
}  // namespace starry::agent
