// Copyright 2026 The Starry Browser Authors

#include "starry/browser/content_blocking/response_body_filter.h"

#include <optional>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_view_util.h"
#include "base/task/thread_pool.h"

namespace starry::content_blocking {

namespace {

// 再大的不改（几乎没有这么大的页面、脚本；也免得一直不结束的响应占着内存）。
constexpr size_t kMaxFilteredBytes = 16 * 1024 * 1024;

}  // namespace

ResponseBodyFilter::ResponseBodyFilter(
    mojo::ScopedDataPipeConsumerHandle source,
    mojo::ScopedDataPipeProducerHandle destination,
    Transform transform,
    DoneCallback done)
    : source_(std::move(source)),
      destination_(std::move(destination)),
      source_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
      destination_watcher_(FROM_HERE,
                           mojo::SimpleWatcher::ArmingPolicy::MANUAL),
      transform_(std::move(transform)),
      done_(std::move(done)) {
  source_watcher_.Watch(
      source_.get(),
      MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
      MOJO_WATCH_CONDITION_SATISFIED,
      base::BindRepeating(
          [](ResponseBodyFilter* self, MojoResult,
             const mojo::HandleSignalsState&) { self->ReadMore(); },
          base::Unretained(this)));
  destination_watcher_.Watch(
      destination_.get(),
      MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
      MOJO_WATCH_CONDITION_SATISFIED,
      base::BindRepeating(
          [](ResponseBodyFilter* self, MojoResult,
             const mojo::HandleSignalsState&) { self->WriteMore(); },
          base::Unretained(this)));
  ReadMore();
}

ResponseBodyFilter::~ResponseBodyFilter() = default;

void ResponseBodyFilter::ReadMore() {
  while (source_) {
    base::span<const uint8_t> data;
    const MojoResult result =
        source_->BeginReadData(MOJO_BEGIN_READ_DATA_FLAG_NONE, data);
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      source_watcher_.ArmOrNotify();
      return;
    }
    if (result != MOJO_RESULT_OK) {
      // 读完了（网络那头关了）。
      OnSourceComplete();
      return;
    }
    const std::string_view chunk = base::as_string_view(data);
    if (passthrough_) {
      Enqueue(std::string(chunk));
    } else {
      buffer_.append(chunk);
    }
    source_->EndReadData(data.size());
    if (!passthrough_ && buffer_.size() > kMaxFilteredBytes) {
      passthrough_ = true;
      Enqueue(std::move(buffer_));
      buffer_ = std::string();
    }
    if (passthrough_) {
      WriteMore();
      if (finished_) {
        return;
      }
    }
  }
}

void ResponseBodyFilter::OnSourceComplete() {
  source_watcher_.Cancel();
  source_.reset();
  if (passthrough_) {
    source_done_ = true;
    WriteMore();
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](Transform transform, std::string body) {
            std::optional<std::string> filtered =
                std::move(transform).Run(body);
            return filtered ? std::move(*filtered) : std::move(body);
          },
          std::move(transform_), std::move(buffer_)),
      base::BindOnce(&ResponseBodyFilter::OnFiltered,
                     weak_factory_.GetWeakPtr()));
  buffer_ = std::string();
}

void ResponseBodyFilter::OnFiltered(std::string body) {
  Enqueue(std::move(body));
  source_done_ = true;
  WriteMore();
}

void ResponseBodyFilter::Enqueue(std::string data) {
  if (!data.empty()) {
    pending_.push_back(std::move(data));
  }
}

void ResponseBodyFilter::WriteMore() {
  if (finished_) {
    return;
  }
  while (!pending_.empty()) {
    const std::string& chunk = pending_.front();
    size_t written = 0;
    const MojoResult result = destination_->WriteData(
        base::as_byte_span(chunk).subspan(pending_offset_),
        MOJO_WRITE_DATA_FLAG_NONE, written);
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      destination_watcher_.ArmOrNotify();
      return;
    }
    if (result != MOJO_RESULT_OK) {
      // 网页那头不要了。
      Finish();
      return;
    }
    bytes_written_ += written;
    pending_offset_ += written;
    if (pending_offset_ == chunk.size()) {
      pending_.pop_front();
      pending_offset_ = 0;
    }
  }
  if (source_done_) {
    Finish();
  }
}

void ResponseBodyFilter::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  source_watcher_.Cancel();
  destination_watcher_.Cancel();
  source_.reset();
  destination_.reset();
  pending_.clear();
  std::move(done_).Run(bytes_written_);
}

}  // namespace starry::content_blocking
