// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/line_channel.h"

#include <windows.h>

#include <array>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/win/object_watcher.h"
#include "starry/browser/agent/line_channel_reader.h"

namespace starry::agent {

namespace {

// OVERLAPPED and its buffer must remain alive until the kernel completes
// cancellation, including when both handles refer to one duplex pipe.
struct Operation {
  explicit Operation(base::ScopedPlatformFile file)
      : pipe(std::move(file)),
        event(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Operation() { Close(); }

  void Prepare() {
    ::ResetEvent(event.get());
    io = {};
    io.hEvent = event.get();
    pending = true;
  }

  void Close() {
    watcher.StopWatching();
    if (pending) {
      ::CancelIoEx(pipe.get(), &io);
      DWORD unused;
      ::GetOverlappedResult(pipe.get(), &io, &unused, TRUE);
      pending = false;
    }
    pipe.Close();
  }

  base::ScopedPlatformFile pipe;
  base::win::ScopedHandle event;
  OVERLAPPED io{};
  base::win::ObjectWatcher watcher;
  bool pending = false;
};

}  // namespace

class LineChannel::Core : public base::win::ObjectWatcher::Delegate {
 public:
  Core(scoped_refptr<base::SequencedTaskRunner> owner,
       base::ScopedPlatformFile read,
       base::ScopedPlatformFile write,
       base::RepeatingCallback<void(std::string)> on_line,
       base::OnceClosure on_closed)
      : reader_(std::move(owner), std::move(on_line), std::move(on_closed)),
        read_(std::move(read)),
        write_(std::move(write)) {
    if (!read_.event.is_valid() || !write_.event.is_valid()) {
      Finish(false);
      return;
    }
    Read();
  }

  void Send(std::string line) {
    if (!write_.pipe.is_valid() || close_after_flush_) {
      return;
    }
    constexpr size_t kMaxPending = 64u << 20;
    if (line.size() >= kMaxPending - pending_.size() - in_flight_.size()) {
      Finish(false);
      return;
    }
    pending_ += line;
    pending_ += '\n';
    Flush();
  }

  void CloseWrite() {
    close_after_flush_ = true;
    Flush();
  }

 private:
  void Read() {
    read_.Prepare();
    if (!::ReadFile(read_.pipe.get(), chunk_.data(), chunk_.size(), nullptr,
                    &read_.io) &&
        ::GetLastError() != ERROR_IO_PENDING) {
      read_.pending = false;
      Finish(true);
      return;
    }
    if (!read_.watcher.StartWatchingOnce(read_.event.get(), this)) {
      Finish(false);
    }
  }

  void Flush() {
    if (write_.pending || !write_.pipe.is_valid()) {
      return;
    }
    if (in_flight_.empty()) {
      in_flight_.swap(pending_);
    }
    if (in_flight_.empty()) {
      if (close_after_flush_) {
        write_.Close();
      }
      return;
    }
    write_.Prepare();
    if (!::WriteFile(write_.pipe.get(), in_flight_.data(),
                     static_cast<DWORD>(in_flight_.size()), nullptr,
                     &write_.io) &&
        ::GetLastError() != ERROR_IO_PENDING) {
      write_.pending = false;
      write_.Close();
      return;
    }
    if (!write_.watcher.StartWatchingOnce(write_.event.get(), this)) {
      write_.Close();
    }
  }

  void OnObjectSignaled(HANDLE event) override {
    DWORD count = 0;
    if (event == read_.event.get()) {
      read_.pending = false;
      if (!::GetOverlappedResult(read_.pipe.get(), &read_.io, &count, FALSE) ||
          !count) {
        Finish(true);
        return;
      }
      if (!reader_.Append(std::string_view(chunk_.data(), count))) {
        Finish(false);
        return;
      }
      Read();
    } else {
      write_.pending = false;
      if (!::GetOverlappedResult(write_.pipe.get(), &write_.io, &count,
                                 FALSE) ||
          !count) {
        write_.Close();
        return;
      }
      in_flight_.erase(0, count);
      Flush();
    }
  }

  void Finish(bool deliver_tail) {
    read_.Close();
    write_.Close();
    reader_.Finish(deliver_tail);
  }

  LineChannelReader reader_;
  std::array<char, 65536> chunk_;
  std::string in_flight_;
  std::string pending_;
  bool close_after_flush_ = false;
  // These cancel outstanding I/O before the buffers above are destroyed.
  Operation read_;
  Operation write_;
};

LineChannel::LineChannel(base::ScopedPlatformFile read_fd,
                         base::ScopedPlatformFile write_fd,
                         base::RepeatingCallback<void(std::string)> on_line,
                         base::OnceClosure on_closed)
    : core_(base::ThreadPool::CreateSequencedTaskRunner(
                {base::MayBlock(),
                 base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}),
            base::SequencedTaskRunner::GetCurrentDefault(),
            std::move(read_fd),
            std::move(write_fd),
            std::move(on_line),
            std::move(on_closed)) {}

LineChannel::~LineChannel() = default;

void LineChannel::Send(std::string line) {
  core_.AsyncCall(&Core::Send).WithArgs(std::move(line));
}

void LineChannel::CloseWrite() {
  core_.AsyncCall(&Core::CloseWrite);
}

}  // namespace starry::agent
