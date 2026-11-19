// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_LINE_CHANNEL_READER_H_
#define STARRY_BROWSER_AGENT_LINE_CHANNEL_READER_H_

#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/task/sequenced_task_runner.h"

namespace starry::agent {

// Shared framing for socket and child stdout/stderr streams. Limits apply to
// individual lines, independent of how the OS partitions reads.
class LineChannelReader {
 public:
  LineChannelReader(scoped_refptr<base::SequencedTaskRunner> owner,
                    base::RepeatingCallback<void(std::string)> on_line,
                    base::OnceClosure on_closed);
  ~LineChannelReader();
  bool Append(std::string_view bytes);
  void Finish(bool deliver_tail);

 private:
  void Deliver();
  scoped_refptr<base::SequencedTaskRunner> owner_;
  base::RepeatingCallback<void(std::string)> on_line_;
  base::OnceClosure on_closed_;
  std::string line_;
};

}  // namespace starry::agent
#endif  // STARRY_BROWSER_AGENT_LINE_CHANNEL_READER_H_
