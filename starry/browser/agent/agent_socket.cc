// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/agent_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <utility>

#include "base/files/file_descriptor_watcher_posix.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"

namespace starry::agent {

namespace {

constexpr size_t kPathCapacity = sizeof(sockaddr_un::sun_path);

std::string Fnv1a(const std::string& text) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (unsigned char byte : text) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return base::StringPrintf("%llx", static_cast<unsigned long long>(hash));
}

bool Address(const base::FilePath& path, sockaddr_un* address) {
  const std::string& value = path.value();
  if (value.size() + 1 > kPathCapacity) {
    return false;
  }
  *address = sockaddr_un();
  address->sun_family = AF_UNIX;
  value.copy(address->sun_path, value.size());
#if BUILDFLAG(IS_APPLE)
  address->sun_len = sizeof(*address);
#endif
  return true;
}

// 对方是不是同一个用户。
bool PeerIsSameUser(int fd) {
#if BUILDFLAG(IS_APPLE)
  uid_t uid = 0;
  gid_t gid = 0;
  return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#else
  ucred credentials = {};
  socklen_t size = sizeof(credentials);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
         credentials.uid == getuid();
#endif
}

std::string SystemError(const char* call) {
  return base::StrCat({call, " failed: ", strerror(errno)});
}

}  // namespace

base::FilePath SocketPath(const base::FilePath& user_data_dir) {
  const base::FilePath preferred = user_data_dir.Append(kSocketName);
  if (preferred.value().size() < kPathCapacity) {
    return preferred;
  }
  return base::FilePath(base::StrCat(
      {"/tmp/starry-agent-", base::NumberToString(getuid()), "-",
       Fnv1a(user_data_dir.StripTrailingSeparators().value()), ".sock"}));
}

base::ScopedFD ConnectSocket(const base::FilePath& path) {
  sockaddr_un address;
  if (!Address(path, &address)) {
    return base::ScopedFD();
  }
  base::ScopedFD fd(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!fd.is_valid()) {
    return base::ScopedFD();
  }
  if (HANDLE_EINTR(connect(fd.get(), reinterpret_cast<sockaddr*>(&address),
                           sizeof(address))) != 0 ||
      !PeerIsSameUser(fd.get())) {
    return base::ScopedFD();
  }
#if BUILDFLAG(IS_APPLE)
  int on = 1;
  setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
  return fd;
}

class SocketServer::Core {
 public:
  Core(scoped_refptr<base::SequencedTaskRunner> owner,
       base::RepeatingCallback<void(base::ScopedFD)> on_connection)
      : owner_(std::move(owner)), on_connection_(std::move(on_connection)) {}
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  ~Core() {
    watch_.reset();
    if (fd_.is_valid()) {
      fd_.reset();
      unlink(path_.value().c_str());
    }
  }

  std::string Start(const base::FilePath& path) {
    base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
    sockaddr_un address;
    if (!Address(path, &address)) {
      return "The agent socket path is too long.";
    }
    // 留下的旧文件：有人在听就是另一个实例，否则删掉重建。
    if (base::PathExists(path)) {
      if (ConnectSocket(path).is_valid()) {
        return "Another Starry Browser instance is already serving agents for "
               "this data directory.";
      }
      unlink(path.value().c_str());
    }
    base::CreateDirectory(path.DirName());
    base::ScopedFD fd(socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.is_valid()) {
      return SystemError("socket");
    }
    // 数据目录本来只有自己进得去（0700）；套接字文件再收紧到 0600。不改 umask：它是整个
    // 进程的，别的线程这时可能正在建文件。
    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
      return SystemError("bind");
    }
    chmod(path.value().c_str(), 0600);
    if (listen(fd.get(), 8) != 0) {
      const std::string error = SystemError("listen");
      unlink(path.value().c_str());
      return error;
    }
    const int flags = fcntl(fd.get(), F_GETFL);
    fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK);
    path_ = path;
    fd_ = std::move(fd);
    watch_ = base::FileDescriptorWatcher::WatchReadable(
        fd_.get(),
        base::BindRepeating(&Core::OnAcceptable, base::Unretained(this)));
    return std::string();
  }

 private:
  void OnAcceptable() {
    while (true) {
      base::ScopedFD client(HANDLE_EINTR(accept(fd_.get(), nullptr, nullptr)));
      if (!client.is_valid()) {
        return;
      }
      if (!PeerIsSameUser(client.get())) {
        continue;
      }
      owner_->PostTask(FROM_HERE,
                       base::BindOnce(on_connection_, std::move(client)));
    }
  }

  const scoped_refptr<base::SequencedTaskRunner> owner_;
  base::RepeatingCallback<void(base::ScopedFD)> on_connection_;
  base::FilePath path_;
  base::ScopedFD fd_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> watch_;
};

SocketServer::SocketServer(
    base::RepeatingCallback<void(base::ScopedFD)> on_connection)
    : core_(base::ThreadPool::CreateSequencedTaskRunner(
                {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
            base::SequencedTaskRunner::GetCurrentDefault(),
            std::move(on_connection)) {}

SocketServer::~SocketServer() = default;

void SocketServer::Start(const base::FilePath& dir,
                         base::OnceCallback<void(std::string)> done) {
  core_.AsyncCall(&Core::Start).WithArgs(SocketPath(dir)).Then(std::move(done));
}

}  // namespace starry::agent
