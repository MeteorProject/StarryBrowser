// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/agent_socket.h"

#include <windows.h>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/win/access_token.h"
#include "base/win/security_descriptor.h"
#include "content/public/browser/browser_thread.h"
#include "crypto/hash.h"
#include "mojo/public/cpp/platform/named_platform_channel.h"
#include "mojo/public/cpp/platform/platform_channel_server.h"

namespace starry::agent {

namespace {
using Channel = mojo::NamedPlatformChannel;

std::wstring UserSidString() {
  auto token = base::win::AccessToken::FromCurrentProcess();
  return token ? token->User().ToSddlString().value_or(L"") : L"";
}
bool OwnedByUser(HANDLE object, const base::win::Sid& user) {
  auto sd = base::win::SecurityDescriptor::FromHandle(
      object, base::win::SecurityObjectType::kFile, OWNER_SECURITY_INFORMATION);
  return sd && sd->owner() && *sd->owner() == user;
}
}  // namespace

base::FilePath SocketPath(const base::FilePath& dir) {
  const std::wstring sid = UserSidString();
  if (dir.empty() || sid.empty()) {
    return {};
  }
  // Use the directory identity where available. Junctions, drive-letter case,
  // short names and long paths must lead both processes to the same pipe.
  base::win::ScopedHandle folder(::CreateFileW(
      dir.value().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  FILE_ID_INFO identity{};
  if (folder.is_valid() &&
      ::GetFileInformationByHandleEx(folder.get(), FileIdInfo, &identity,
                                     sizeof(identity))) {
    return base::FilePath(
        L"starry.agent." + sid + L"." +
        base::ASCIIToWide(base::HexEncode(base::byte_span_from_ref(identity))));
  }
  base::FilePath canonical;
  if (!base::NormalizeFilePath(dir, &canonical)) {
    // The browser may not have created the directory yet; the bridge retries.
    canonical = dir.NormalizePathSeparators();
  }
  const auto hash =
      crypto::hash::Sha256(canonical.StripTrailingSeparators().AsUTF8Unsafe());
  return base::FilePath(L"starry.agent." + sid + L"." +
                        base::ASCIIToWide(base::HexEncode(hash)));
}

base::ScopedPlatformFile ConnectSocket(const base::FilePath& name) {
  if (name.empty()) {
    return {};
  }
  const auto token = base::win::AccessToken::FromCurrentProcess();
  if (!token) {
    return {};
  }
  const std::wstring pipe = Channel::GetPipeNameFromServerName(name.value());
  base::win::ScopedHandle handle(::CreateFileW(
      pipe.c_str(),
      FILE_READ_DATA | FILE_WRITE_DATA | READ_CONTROL | SYNCHRONIZE, 0, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
      nullptr));
  if (!handle.is_valid()) {
    return {};
  }
  if (!OwnedByUser(handle.get(), token->User())) {
    return {};
  }
  return handle;
}

class SocketServer::Core {
 public:
  Core(scoped_refptr<base::SequencedTaskRunner> owner,
       base::RepeatingCallback<void(base::ScopedPlatformFile)> connected)
      : owner_(std::move(owner)), connected_(std::move(connected)) {}

  void Start(const base::FilePath& dir,
             base::OnceCallback<void(std::string)> done) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()}, base::BindOnce(&SocketPath, dir),
        base::BindOnce(&Core::StartListening, weak_.GetWeakPtr(),
                       std::move(done)));
  }

 private:
  void StartListening(base::OnceCallback<void(std::string)> done,
                      base::FilePath name) {
    options_.server_name = name.value();
    const auto sid = UserSidString();
    if (!name.empty() && !sid.empty()) {
      options_.security_descriptor = L"O:" + sid + L"D:P(A;;GA;;;" + sid + L")";
      options_.max_clients = PIPE_UNLIMITED_INSTANCES;
      StartFirst(std::move(done), base::TimeTicks::Now() + base::Seconds(3));
      return;
    }
    ReportStarted(std::move(done), false);
  }

  void StartFirst(base::OnceCallback<void(std::string)> done,
                  base::TimeTicks deadline) {
    if (Listen()) {
      options_.enforce_uniqueness = false;
      ReportStarted(std::move(done), true);
      return;
    }
    // Previous clients may still be closing on their own I/O sequences.
    if (base::TimeTicks::Now() < deadline) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&Core::StartFirst, weak_.GetWeakPtr(), std::move(done),
                         deadline),
          base::Milliseconds(100));
      return;
    }
    ReportStarted(std::move(done), false);
  }

  void ReportStarted(base::OnceCallback<void(std::string)> done, bool ok) {
    owner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            std::move(done),
            std::string(ok ? ""
                           : "Could not listen on the agent named pipe.")));
  }

  bool Listen() {
    Channel channel(options_);
    auto endpoint = channel.TakeServerEndpoint();
    const auto token = base::win::AccessToken::FromCurrentProcess();
    if (!token || !endpoint.is_valid() ||
        !OwnedByUser(endpoint.platform_handle().GetHandle().get(),
                     token->User())) {
      return false;
    }
    return listener_.Listen(
        std::move(endpoint),
        base::BindOnce(&Core::Accepted, weak_.GetWeakPtr()));
  }

  void Accepted(mojo::PlatformChannelEndpoint endpoint) {
    listener_.Stop();
    // Keep the connected pipe alive until the next listening instance exists.
    // If all slots are busy, retry once Windows has released one.
    if (!Listen()) {
      ScheduleRetry();
    }
    if (endpoint.is_valid()) {
      owner_->PostTask(
          FROM_HERE,
          base::BindOnce(connected_,
                         endpoint.TakePlatformHandle().TakeHandle()));
    }
  }

  void ScheduleRetry() {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&Core::Retry, weak_.GetWeakPtr()),
        base::Milliseconds(200));
  }

  void Retry() {
    if (!Listen()) {
      ScheduleRetry();
    }
  }

  scoped_refptr<base::SequencedTaskRunner> owner_;
  base::RepeatingCallback<void(base::ScopedPlatformFile)> connected_;
  Channel::Options options_;
  mojo::PlatformChannelServer listener_;
  base::WeakPtrFactory<Core> weak_{this};
};

SocketServer::SocketServer(
    base::RepeatingCallback<void(base::ScopedPlatformFile)> connected)
    : core_(content::GetIOThreadTaskRunner({}),
            base::SequencedTaskRunner::GetCurrentDefault(),
            std::move(connected)) {}
SocketServer::~SocketServer() = default;

void SocketServer::Start(const base::FilePath& dir,
                         base::OnceCallback<void(std::string)> done) {
  core_.AsyncCall(&Core::Start).WithArgs(dir, std::move(done));
}

}  // namespace starry::agent
