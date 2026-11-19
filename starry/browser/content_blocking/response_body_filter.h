// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_CONTENT_BLOCKING_RESPONSE_BODY_FILTER_H_
#define STARRY_BROWSER_CONTENT_BLOCKING_RESPONSE_BODY_FILTER_H_

#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"

namespace starry::content_blocking {

// 改写响应体（页面的 HTML 过滤）：把响应体
// 整个读进来，在线程池上用 transform 改写，再写给网页进程。超过 16 MB 的不改，读到的和
// 后面的原样转过去。在代理的线程（IO 线程）上用。
class ResponseBodyFilter {
 public:
  // 在线程池上跑：返回改好的，nullopt 是原样。要用的规则、引擎由它自己拿着。
  using Transform =
      base::OnceCallback<std::optional<std::string>(std::string_view body)>;
  // bytes 是写给网页进程的字节数。
  using DoneCallback = base::OnceCallback<void(size_t bytes)>;

  ResponseBodyFilter(mojo::ScopedDataPipeConsumerHandle source,
                     mojo::ScopedDataPipeProducerHandle destination,
                     Transform transform,
                     DoneCallback done);
  ResponseBodyFilter(const ResponseBodyFilter&) = delete;
  ResponseBodyFilter& operator=(const ResponseBodyFilter&) = delete;
  ~ResponseBodyFilter();

 private:
  void ReadMore();
  void OnSourceComplete();
  void OnFiltered(std::string body);
  void Enqueue(std::string data);
  void WriteMore();
  // 写完了（或者网页那头不要了）。之后不能再碰成员（done_ 可能删掉自己）。
  void Finish();

  mojo::ScopedDataPipeConsumerHandle source_;
  mojo::ScopedDataPipeProducerHandle destination_;
  mojo::SimpleWatcher source_watcher_;
  mojo::SimpleWatcher destination_watcher_;
  Transform transform_;
  DoneCallback done_;

  std::string buffer_;
  // 太大了，不改，原样转过去。
  bool passthrough_ = false;
  bool source_done_ = false;
  std::deque<std::string> pending_;
  size_t pending_offset_ = 0;
  size_t bytes_written_ = 0;
  bool finished_ = false;

  base::WeakPtrFactory<ResponseBodyFilter> weak_factory_{this};
};

}  // namespace starry::content_blocking

#endif  // STARRY_BROWSER_CONTENT_BLOCKING_RESPONSE_BODY_FILTER_H_
