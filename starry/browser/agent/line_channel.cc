// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/line_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <utility>

#include "base/files/file_descriptor_watcher_posix.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/posix/eintr_wrapper.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "starry/browser/agent/line_channel_reader.h"

namespace starry::agent {

namespace {

// 写不进去的数据最多攒这么多（对方一直不读）：超过就断开。
constexpr size_t kMaxPending = 64u << 20;

void SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

ssize_t WriteSome(int fd, const char* data, size_t size) {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  // 套接字：对方断开时不要 SIGPIPE。管道上 send 不行，失败时退回 write。
  ssize_t written = HANDLE_EINTR(send(fd, data, size, MSG_NOSIGNAL));
  if (written >= 0 || errno != ENOTSOCK) {
    return written;
  }
#endif
  return HANDLE_EINTR(write(fd, data, size));
}

}  // namespace

class LineChannel::Core {
 public:
  Core(scoped_refptr<base::SequencedTaskRunner> owner,
       base::ScopedFD read_fd,
       base::ScopedFD write_fd,
       base::RepeatingCallback<void(std::string)> on_line,
       base::OnceClosure on_closed)
      : reader_(std::move(owner), std::move(on_line), std::move(on_closed)),
        read_fd_(std::move(read_fd)),
        write_fd_(std::move(write_fd)) {
    SetNonBlocking(read_fd_.get());
#if BUILDFLAG(IS_APPLE)
    // macOS 没有 MSG_NOSIGNAL：在套接字上关掉 SIGPIPE（管道上设不了，没关系）。
    int on = 1;
    setsockopt(read_fd_.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    if (write_fd_.is_valid()) {
      SetNonBlocking(write_fd_.get());
#if BUILDFLAG(IS_APPLE)
      setsockopt(write_fd_.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    }
    read_watch_ = base::FileDescriptorWatcher::WatchReadable(
        read_fd_.get(),
        base::BindRepeating(&Core::OnReadable, base::Unretained(this)));
  }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  ~Core() = default;

  void Send(std::string line) {
    if (!write_fd_.is_valid()) {
      return;
    }
    pending_ += line;
    pending_ += '\n';
    if (pending_.size() > kMaxPending) {
      Closed();
      return;
    }
    Flush();
  }

  void CloseWrite() {
    close_after_flush_ = true;
    Flush();
  }

 private:
  void OnReadable() {
    char chunk[65536];
    while (true) {
      const ssize_t count =
          HANDLE_EINTR(read(read_fd_.get(), chunk, sizeof(chunk)));
      if (count > 0) {
        if (!reader_.Append(
                std::string_view(chunk, static_cast<size_t>(count)))) {
          Closed();
          return;
        }
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      // 0 是对方关了；别的错误也当断开。最后没有换行的半行也交出去。
      Closed(/*deliver_tail=*/true);
      return;
    }
  }

  void Flush() {
    while (!pending_.empty() && write_fd_.is_valid()) {
      const ssize_t written =
          WriteSome(write_fd_.get(), pending_.data(), pending_.size());
      if (written > 0) {
        pending_.erase(0, static_cast<size_t>(written));
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!write_watch_) {
          write_watch_ = base::FileDescriptorWatcher::WatchWritable(
              write_fd_.get(),
              base::BindRepeating(&Core::OnWritable, base::Unretained(this)));
        }
        return;
      }
      // 对方关了读的一端：读那边会收到结尾。
      pending_.clear();
      write_watch_.reset();
      write_fd_.reset();
      return;
    }
    write_watch_.reset();
    if (pending_.empty() && close_after_flush_) {
      write_fd_.reset();
    }
  }

  void OnWritable() { Flush(); }

  void Closed(bool deliver_tail = false) {
    read_watch_.reset();
    write_watch_.reset();
    reader_.Finish(deliver_tail);
  }

  LineChannelReader reader_;
  base::ScopedFD read_fd_;
  base::ScopedFD write_fd_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> read_watch_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> write_watch_;
  std::string pending_;
  bool close_after_flush_ = false;
};

LineChannel::LineChannel(base::ScopedFD read_fd,
                         base::ScopedFD write_fd,
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
