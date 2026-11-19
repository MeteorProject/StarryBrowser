// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_DOWNLOAD_MODEL_H_
#define STARRY_BROWSER_DOWNLOADS_DOWNLOAD_MODEL_H_

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"

class GURL;

namespace net {
class HttpResponseHeaders;
}

// Starry 自己的下载器的纯逻辑：分段、Range 头、重试、测速和限速。
// 网络和文件读写在 http_downloader.h。
namespace starry {

// 服务器是否支持分段下载（HTTP Range）。
enum class RangeSupport {
  // 还没问过服务器。
  kUnknown,
  // 分段请求得到了 206：可以多线程、断点续传。
  kSupported,
  // 服务器忽略 Range，只能从头单线程下载，暂停后要重新开始。
  kUnsupported,
};

// unknown、supported、unsupported（存盘、下载页用的名字）。
const char* RangeSupportName(RangeSupport support);
RangeSupport RangeSupportFromName(std::string_view name);

// `Content-Range: bytes 0-499/1234`。`end` 包含在内；总大小未知（`/*`）时 `total`
// 是空的。
struct ContentRange {
  int64_t start = 0;
  int64_t end = 0;
  std::optional<int64_t> total;

  static std::optional<ContentRange> Parse(std::string_view header);
  // 416 回应里的 `bytes */1234`。
  static std::optional<int64_t> UnsatisfiedTotal(std::string_view header);
};

// 文件里的一段 [start, end)，从 start 起已经连续收到 `received` 字节。总大小未知的单线程
// 下载用 `end == kOpenEnd`。
struct DownloadSegment {
  static constexpr int64_t kOpenEnd = std::numeric_limits<int64_t>::max();

  int64_t start = 0;
  int64_t end = 0;
  int64_t received = 0;

  int64_t position() const { return start + received; }
  int64_t remaining() const;
  bool complete() const { return end != kOpenEnd && position() >= end; }
  int64_t length() const { return end - start; }

  friend bool operator==(const DownloadSegment&,
                         const DownloadSegment&) = default;
};

// 多线程下载怎么分配（同 IDM、aria2 的动态分段）：一个连接空下来时，先接没人下的段，再把
// 剩得最多的段对半分一份给它。文件一开始只有一段，连接数是 2 的幂时依次对半分，正好分成
// 等长的几段；先下完的连接会分走慢连接的一半。
struct SegmentAssignment {
  enum class Kind {
    // 接着下 `index` 这一段。
    kExisting,
    // 把 `index` 这一段在 `at` 处切开，后半段（新加在最后）给空下来的连接。
    kSplit,
  };
  Kind kind = Kind::kExisting;
  size_t index = 0;
  int64_t at = 0;
};

// 每段至少这么大：太小的文件不分段，快下完的段也不再分。
inline constexpr int64_t kMinimumSegmentSize = 1 << 20;

// `busy` 是正在被连接下载的段的下标。不分段（服务器不支持）时只会给出没人下的段。
std::optional<SegmentAssignment> NextSegment(
    const std::vector<DownloadSegment>& segments,
    const std::set<size_t>& busy,
    bool allow_split,
    int64_t minimum_size = kMinimumSegmentSize);

// 应用分配：切开时前半段缩短、后半段加在最后。返回要下载的那一段的下标。
size_t ApplySegment(const SegmentAssignment& assignment,
                    std::vector<DownloadSegment>& segments);

// 已经收到的字节数。
int64_t ReceivedBytes(const std::vector<DownloadSegment>& segments);

// 分段记录说得通：段互不重叠、首尾相接铺满总大小、收到的不超过段长。读进来的记录可能被
// 改过。
bool SegmentsConsistent(const std::vector<DownloadSegment>& segments,
                        int64_t total_bytes);

// 连接出错后的重试：等 1、2、4、8、16 秒，共 5 次；收到数据后重新计数。
inline constexpr int kMaxRetryAttempts = 5;
base::TimeDelta RetryDelay(int attempt);
// 这个 HTTP 状态码值不值得重试（服务器忙、网关错误、限流）。
bool IsTransientStatus(int status);

// Chrome 收到回应后能不能交给 Starry 自己下载：回应声明了支持分段（`Accept-Ranges:
// bytes`）、知道大小、内容没压缩的 http(s) 下载。只看回应声明的能力：重新请求一次不会
// 坏事的大多是这类静态文件；一次性的链接通常不声明分段。
bool CanHandOff(const GURL& url,
                const net::HttpResponseHeaders* headers,
                int64_t total_bytes);

// 强 ETag（可以放进 If-Range）；弱的（W/"…"）和没有的是空的。
std::string StrongETag(std::string_view etag);

// 下载速度：指数平滑，时间常数 `smoothing`；两次读数间隔太短时不更新。
class TransferRateMeter {
 public:
  explicit TransferRateMeter(base::TimeDelta smoothing = base::Seconds(2));

  void Record(int64_t total_bytes, base::TimeTicks now);
  void Reset();
  double bytes_per_second() const { return bytes_per_second_; }

 private:
  base::TimeDelta smoothing_;
  double bytes_per_second_ = 0;
  std::optional<int64_t> last_bytes_;
  base::TimeTicks last_time_;
};

// 令牌桶限速：每秒补充 `rate` 个令牌，最多攒 `burst` 的量；欠下的令牌要多久才能还清就
// 等多久。
class TokenBucket {
 public:
  explicit TokenBucket(double rate = 0,
                       base::TimeDelta burst = base::Milliseconds(500));

  // 0 是不限速。
  void SetRate(double rate);
  double rate() const { return rate_; }
  // 用掉 `bytes` 个令牌，返回要等多久（零是不用等）。
  base::TimeDelta Consume(int64_t bytes, base::TimeTicks now);

 private:
  double rate_;
  base::TimeDelta burst_;
  double tokens_;
  base::TimeTicks last_;
};

// 所有下载共用的限速：每个下载器在自己的线程上取令牌。
class DownloadThrottle {
 public:
  static DownloadThrottle& Get();

  DownloadThrottle();
  DownloadThrottle(const DownloadThrottle&) = delete;
  DownloadThrottle& operator=(const DownloadThrottle&) = delete;
  ~DownloadThrottle();

  // 0 是不限速。
  void SetLimit(int64_t bytes_per_second);
  int64_t limit() const;
  base::TimeDelta Consume(int64_t bytes);

 private:
  mutable base::Lock lock_;
  TokenBucket bucket_ GUARDED_BY(lock_);
};

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_DOWNLOAD_MODEL_H_
