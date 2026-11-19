// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/line_process.h"

#include <windows.h>

#include <array>
#include <string_view>

#include "base/process/environment_internal.h"
#include "base/process/launch.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/unguessable_token.h"
#include "base/win/scoped_process_information.h"
#include "base/win/security_descriptor.h"
#include "base/win/startup_information.h"
#include "starry/browser/agent/line_channel.h"
#include "starry/browser/assistant/agent_process_win.h"

namespace starry::assistant {
namespace {

// Child tools expect synchronous standard handles. Only the browser end uses
// overlapped I/O so that stopping a conversation also cancels pending reads.
bool MakePipe(bool parent_reads,
              base::win::ScopedHandle& parent,
              base::win::ScopedHandle& child,
              SECURITY_ATTRIBUTES& security) {
  const std::wstring name =
      L"\\\\.\\pipe\\starry.stdio." +
      base::ASCIIToWide(base::UnguessableToken::Create().ToString());
  parent.Set(::CreateNamedPipeW(
      name.c_str(),
      (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
          FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0,
      &security));
  if (!parent.is_valid()) {
    return false;
  }
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  child.Set(::CreateFileW(
      name.c_str(),
      (parent_reads ? FILE_WRITE_DATA : FILE_READ_DATA) | SYNCHRONIZE, 0,
      &inherit, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
      nullptr));
  return child.is_valid();
}

}  // namespace

std::wstring AgentCommand(const base::FilePath& program,
                          const std::vector<std::string>& arguments) {
  std::wstring command =
      base::CommandLine::QuoteForCommandLineToArgvW(program.value());
  for (const auto& arg : arguments) {
    // Claude's "--tools" takes an explicit empty argument. GetCommandLineString
    // otherwise serializes it as whitespace and the next switch becomes its
    // value.
    command += L' ';
    command += arg.empty() ? L"\"\""
                           : base::CommandLine::QuoteForCommandLineToArgvW(
                                 base::UTF8ToWide(arg));
  }
  return command;
}

base::win::ScopedHandle CreateAgentJob() {
  base::win::ScopedHandle job(::CreateJobObjectW(nullptr, nullptr));
  if (!job.is_valid() || !base::SetJobObjectLimitFlags(
                             job.get(), JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)) {
    return {};
  }
  return job;
}

std::unique_ptr<LineProcess::Launched> LineProcess::Launch(Options options) {
  auto result = std::make_unique<Launched>();
  base::win::ScopedHandle input, output, errors;
  auto token = base::win::AccessToken::FromCurrentProcess();
  auto sd = base::win::SecurityDescriptor::CreateWithEmptyDacl();
  if (!token || !sd.SetDaclEntry(*token, base::win::SecurityAccessMode::kGrant,
                                 GENERIC_ALL, 0)) {
    result->error = "Could not secure agent pipes.";
    return result;
  }
  auto descriptor = sd.ToAbsolute();
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
  if (!MakePipe(false, result->input, input, security) ||
      !MakePipe(true, result->output, output, security) ||
      !MakePipe(true, result->errors, errors, security)) {
    result->error = "Could not create agent pipes.";
    return result;
  }
  result->job = CreateAgentJob();
  if (!result->job.is_valid()) {
    result->error = "Could not create agent job.";
    return result;
  }
  // Assign the job at creation. A CLI may spawn its bridge before a
  // post-launch AssignProcessToJobObject could have captured the child.
  std::array<HANDLE, 3> stdio{input.get(), output.get(), errors.get()};
  HANDLE job = result->job.get();
  base::win::StartupInformation startup;
  if (!startup.InitializeProcThreadAttributeList(2) ||
      !startup.UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         stdio.data(), sizeof(stdio)) ||
      !startup.UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_JOB_LIST, &job,
                                         sizeof(job))) {
    result->error = "Could not configure the agent process.";
    return result;
  }
  STARTUPINFOW* info = startup.startup_info();
  info->dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  info->wShowWindow = SW_HIDE;
  info->hStdInput = input.get();
  info->hStdOutput = output.get();
  info->hStdError = errors.get();
  std::wstring command = AgentCommand(options.executable, options.arguments);
  std::wstring env = base::internal::AlterEnvironment({}, options.environment);
  PROCESS_INFORMATION process{};
  if (::CreateProcessW(options.executable.value().c_str(), command.data(),
                       nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT |
                           CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                       env.data(),
                       options.directory.empty()
                           ? nullptr
                           : options.directory.value().c_str(),
                       info, &process)) {
    base::win::ScopedProcessInformation handles(process);
    result->process = base::Process(handles.TakeProcessHandle());
  }
  if (!result->process.IsValid()) {
    result->error = "Could not start " + options.executable.AsUTF8Unsafe();
  }
  return result;
}

LineProcess::~LineProcess() = default;

void LineProcess::Interrupt() {
  // The backend has already sent its protocol interrupt. EOF lets a stalled
  // CLI shut down before the backend's final termination deadline.
  CloseInput();
}

void LineProcess::Terminate() {
  job_.Close();
}

base::EnvironmentMap ChildEnvironment(const base::FilePath& executable) {
  base::EnvironmentMap env;
  const auto block = base::internal::GetEnvironment();
  const std::wstring_view entries(block.data(), block.size());
  for (size_t start = 0; start < entries.size() && entries[start];) {
    const size_t end = entries.find(L'\0', start);
    if (end == entries.npos) {
      break;
    }
    const auto entry = entries.substr(start, end - start);
    // Special drive-directory entries begin with '='.
    const size_t equal = entry.find(L'=', 1);
    if (equal != entry.npos) {
      auto key = std::wstring(entry.substr(0, equal));
      if (base::EqualsCaseInsensitiveASCII(key, L"PATH")) {
        key = L"PATH";
      } else if (base::EqualsCaseInsensitiveASCII(key, L"MCP_TOOL_TIMEOUT")) {
        key = L"MCP_TOOL_TIMEOUT";
      }
      env[key] = entry.substr(equal + 1);
    }
    start = end + 1;
  }
  if (!executable.empty()) {
    auto& path = env[L"PATH"];
    if (!path.empty()) {
      path.insert(0, L";");
    }
    path.insert(0, executable.DirName().value());
  }
  return env;
}
}  // namespace starry::assistant
