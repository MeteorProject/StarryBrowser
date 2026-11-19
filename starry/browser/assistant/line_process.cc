// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/line_process.h"

#include <utility>
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "starry/browser/agent/line_channel.h"

namespace starry::assistant {
namespace {
constexpr size_t kErrorTail = 4096;
}
LineProcess::Launched::Launched() = default;
LineProcess::Launched::~Launched() = default;

LineProcess::Options::Options() = default;
LineProcess::Options::Options(const Options&) = default;
LineProcess::Options& LineProcess::Options::operator=(const Options&) = default;
LineProcess::Options::~Options() = default;

LineProcess::LineProcess(base::RepeatingCallback<void(std::string)> on_line,
                         base::OnceCallback<void(int, std::string)> on_exit)
    : on_line_(std::move(on_line)), on_exit_(std::move(on_exit)) {}

void LineProcess::Start(Options options,
                        base::OnceCallback<void(std::string)> started) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&LineProcess::Launch, std::move(options)),
      base::BindOnce(&LineProcess::DidLaunch, weak_factory_.GetWeakPtr(),
                     std::move(started)));
}

void LineProcess::DidLaunch(base::OnceCallback<void(std::string)> started,
                            std::unique_ptr<Launched> launched) {
  if (!launched->error.empty()) {
    std::move(started).Run(launched->error);
    return;
  }
#if BUILDFLAG(IS_WIN)
  job_ = std::move(launched->job);
#endif
  process_ = std::move(launched->process);
  io_ = std::make_unique<agent::LineChannel>(
      std::move(launched->output), std::move(launched->input), on_line_,
      base::BindOnce(&LineProcess::OutputClosed, weak_factory_.GetWeakPtr()));
  errors_ = std::make_unique<agent::LineChannel>(
      std::move(launched->errors), base::ScopedPlatformFile(),
      base::BindRepeating(
          [](base::WeakPtr<LineProcess> self, std::string line) {
            if (!self) {
              return;
            }
            self->error_tail_ += line;
            self->error_tail_ += '\n';
            if (self->error_tail_.size() > kErrorTail) {
              self->error_tail_.erase(0, self->error_tail_.size() - kErrorTail);
            }
          },
          weak_factory_.GetWeakPtr()),
      base::OnceClosure());
  // 启动之前写的。
  for (std::string& line : pending_) {
    io_->Send(std::move(line));
  }
  pending_.clear();
  if (close_input_) {
    io_->CloseWrite();
  }
  // 等它退出（后台线程上阻塞地等）。
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(
          [](base::Process process) {
            int code = -1;
            process.WaitForExit(&code);
            return code;
          },
          process_.Duplicate()),
      base::BindOnce(&LineProcess::Exited, weak_factory_.GetWeakPtr()));
  std::move(started).Run(std::string());
}

void LineProcess::Write(std::string line) {
  if (exited_) {
    return;
  }
  if (io_) {
    io_->Send(std::move(line));
  } else {
    pending_.push_back(std::move(line));
  }
}

void LineProcess::CloseInput() {
  close_input_ = true;
  if (io_) {
    io_->CloseWrite();
  }
}

void LineProcess::OutputClosed() {
  output_closed_ = true;
  MaybeFinish();
}

void LineProcess::Exited(int code) {
  exited_ = true;
  exit_code_ = code;
  kill_timer_.Stop();
  if (!output_closed_) {
    exit_grace_.Start(
        FROM_HERE, base::Seconds(1),
        base::BindOnce(&LineProcess::OutputClosed, weak_factory_.GetWeakPtr()));
  }
  MaybeFinish();
}

void LineProcess::MaybeFinish() {
  if (!output_closed_ || !exit_code_ || !on_exit_) {
    return;
  }
  exit_grace_.Stop();
  std::string errors(base::TrimWhitespaceASCII(error_tail_, base::TRIM_ALL));
  std::move(on_exit_).Run(*exit_code_, std::move(errors));
}

}  // namespace starry::assistant
