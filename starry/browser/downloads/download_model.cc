// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/download_model.h"

#include <algorithm>
#include <cmath>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/http/http_response_headers.h"
#include "url/gurl.h"

namespace starry {

namespace {

std::string_view Trim(std::string_view text) {
  return base::TrimWhitespaceASCII(text, base::TRIM_ALL);
}

}  // namespace

const char* RangeSupportName(RangeSupport support) {
  switch (support) {
    case RangeSupport::kUnknown:
      return "unknown";
    case RangeSupport::kSupported:
      return "supported";
    case RangeSupport::kUnsupported:
      return "unsupported";
  }
}

RangeSupport RangeSupportFromName(std::string_view name) {
  if (name == "supported") {
    return RangeSupport::kSupported;
  }
  if (name == "unsupported") {
    return RangeSupport::kUnsupported;
  }
  return RangeSupport::kUnknown;
}

std::optional<ContentRange> ContentRange::Parse(std::string_view header) {
  std::string_view text = Trim(header);
  if (!base::StartsWith(text, "bytes", base::CompareCase::INSENSITIVE_ASCII)) {
    return std::nullopt;
  }
  text = Trim(text.substr(5));
  const size_t slash = text.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view bounds = Trim(text.substr(0, slash));
  const std::string_view total_text = Trim(text.substr(slash + 1));
  const size_t dash = bounds.find('-');
  if (dash == std::string_view::npos) {
    return std::nullopt;
  }
  ContentRange range;
  if (!base::StringToInt64(Trim(bounds.substr(0, dash)), &range.start) ||
      !base::StringToInt64(Trim(bounds.substr(dash + 1)), &range.end) ||
      range.start < 0 || range.end < range.start) {
    return std::nullopt;
  }
  if (total_text != "*") {
    int64_t total = 0;
    if (!base::StringToInt64(total_text, &total) || total <= range.end) {
      return std::nullopt;
    }
    range.total = total;
  }
  return range;
}

std::optional<int64_t> ContentRange::UnsatisfiedTotal(std::string_view header) {
  std::string text;
  for (char c : header) {
    if (c != ' ') {
      text.push_back(base::ToLowerASCII(c));
    }
  }
  constexpr std::string_view kPrefix = "bytes*/";
  int64_t total = 0;
  if (!text.starts_with(kPrefix) ||
      !base::StringToInt64(std::string_view(text).substr(kPrefix.size()),
                           &total)) {
    return std::nullopt;
  }
  return total;
}

int64_t DownloadSegment::remaining() const {
  return end == kOpenEnd ? kOpenEnd : std::max<int64_t>(0, end - position());
}

std::optional<SegmentAssignment> NextSegment(
    const std::vector<DownloadSegment>& segments,
    const std::set<size_t>& busy,
    bool allow_split,
    int64_t minimum_size) {
  // 没人下的段里最靠前的。
  std::optional<size_t> idle;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (busy.contains(i) || segments[i].complete()) {
      continue;
    }
    if (!idle || segments[i].start < segments[*idle].start) {
      idle = i;
    }
  }
  if (idle) {
    return SegmentAssignment{SegmentAssignment::Kind::kExisting, *idle, 0};
  }
  if (!allow_split) {
    return std::nullopt;
  }
  // 剩得最多、还够切成两段的。
  std::optional<size_t> largest;
  for (size_t i = 0; i < segments.size(); ++i) {
    const DownloadSegment& segment = segments[i];
    if (segment.complete() || segment.end == DownloadSegment::kOpenEnd ||
        segment.remaining() < 2 * minimum_size) {
      continue;
    }
    if (!largest || segment.remaining() > segments[*largest].remaining()) {
      largest = i;
    }
  }
  if (!largest) {
    return std::nullopt;
  }
  const DownloadSegment& segment = segments[*largest];
  return SegmentAssignment{SegmentAssignment::Kind::kSplit, *largest,
                           segment.position() + segment.remaining() / 2};
}

size_t ApplySegment(const SegmentAssignment& assignment,
                    std::vector<DownloadSegment>& segments) {
  if (assignment.kind == SegmentAssignment::Kind::kExisting) {
    return assignment.index;
  }
  DownloadSegment tail;
  tail.start = assignment.at;
  tail.end = segments[assignment.index].end;
  segments[assignment.index].end = assignment.at;
  segments.push_back(tail);
  return segments.size() - 1;
}

int64_t ReceivedBytes(const std::vector<DownloadSegment>& segments) {
  int64_t total = 0;
  for (const DownloadSegment& segment : segments) {
    total += segment.received;
  }
  return total;
}

bool SegmentsConsistent(const std::vector<DownloadSegment>& segments,
                        int64_t total_bytes) {
  if (total_bytes <= 0 || segments.empty()) {
    return false;
  }
  std::vector<DownloadSegment> sorted = segments;
  std::ranges::sort(sorted, {}, &DownloadSegment::start);
  int64_t cursor = 0;
  for (const DownloadSegment& segment : sorted) {
    if (segment.start != cursor || segment.end <= segment.start ||
        segment.received < 0 || segment.received > segment.length()) {
      return false;
    }
    cursor = segment.end;
  }
  return cursor == total_bytes;
}

base::TimeDelta RetryDelay(int attempt) {
  return base::Seconds(std::min(1 << (std::clamp(attempt, 1, 5) - 1), 16));
}

bool IsTransientStatus(int status) {
  return status == 408 || status == 429 || status == 500 || status == 502 ||
         status == 503 || status == 504;
}

bool CanHandOff(const GURL& url,
                const net::HttpResponseHeaders* headers,
                int64_t total_bytes) {
  if (!url.SchemeIsHTTPOrHTTPS() || !headers ||
      headers->response_code() != 200 || total_bytes <= 0) {
    return false;
  }
  // 压缩过的内容分段后拼不回原文件。
  const std::string encoding = base::ToLowerASCII(
      headers->GetNormalizedHeader("Content-Encoding").value_or(""));
  if (!encoding.empty() && encoding != "identity") {
    return false;
  }
  const std::string accept_ranges = base::ToLowerASCII(
      headers->GetNormalizedHeader("Accept-Ranges").value_or(""));
  for (std::string_view unit :
       base::SplitStringPiece(accept_ranges, ",", base::TRIM_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    if (unit == "bytes") {
      return true;
    }
  }
  return false;
}

std::string StrongETag(std::string_view etag) {
  etag = Trim(etag);
  if (etag.empty() || etag.starts_with("W/") || etag.starts_with("w/")) {
    return std::string();
  }
  return std::string(etag);
}

TransferRateMeter::TransferRateMeter(base::TimeDelta smoothing)
    : smoothing_(smoothing) {}

void TransferRateMeter::Record(int64_t total_bytes, base::TimeTicks now) {
  if (!last_bytes_) {
    last_bytes_ = total_bytes;
    last_time_ = now;
    return;
  }
  const base::TimeDelta elapsed = now - last_time_;
  if (elapsed < base::Milliseconds(50)) {
    return;
  }
  const double instant =
      std::max<int64_t>(0, total_bytes - *last_bytes_) / elapsed.InSecondsF();
  const double alpha = 1 - std::exp(-elapsed / smoothing_);
  bytes_per_second_ =
      bytes_per_second_ == 0
          ? instant
          : bytes_per_second_ + alpha * (instant - bytes_per_second_);
  if (bytes_per_second_ < 1) {
    bytes_per_second_ = 0;
  }
  last_bytes_ = total_bytes;
  last_time_ = now;
}

void TransferRateMeter::Reset() {
  bytes_per_second_ = 0;
  last_bytes_.reset();
  last_time_ = base::TimeTicks();
}

TokenBucket::TokenBucket(double rate, base::TimeDelta burst)
    : rate_(rate), burst_(burst), tokens_(rate * burst.InSecondsF()) {}

void TokenBucket::SetRate(double rate) {
  // 从不限速换成限速时桶是满的，不用先等一轮。
  const double capacity = rate * burst_.InSecondsF();
  tokens_ = rate_ <= 0 ? capacity : std::min(tokens_, capacity);
  rate_ = rate;
}

base::TimeDelta TokenBucket::Consume(int64_t bytes, base::TimeTicks now) {
  if (rate_ <= 0) {
    return base::TimeDelta();
  }
  if (!last_.is_null()) {
    tokens_ = std::min(rate_ * burst_.InSecondsF(),
                       tokens_ + (now - last_).InSecondsF() * rate_);
  }
  last_ = now;
  tokens_ -= static_cast<double>(bytes);
  return tokens_ >= 0 ? base::TimeDelta() : base::Seconds(-tokens_ / rate_);
}

// static
DownloadThrottle& DownloadThrottle::Get() {
  static base::NoDestructor<DownloadThrottle> throttle;
  return *throttle;
}

DownloadThrottle::DownloadThrottle() = default;
DownloadThrottle::~DownloadThrottle() = default;

void DownloadThrottle::SetLimit(int64_t bytes_per_second) {
  base::AutoLock lock(lock_);
  bucket_.SetRate(static_cast<double>(std::max<int64_t>(0, bytes_per_second)));
}

int64_t DownloadThrottle::limit() const {
  base::AutoLock lock(lock_);
  return static_cast<int64_t>(bucket_.rate());
}

base::TimeDelta DownloadThrottle::Consume(int64_t bytes) {
  base::AutoLock lock(lock_);
  return bucket_.Consume(bytes, base::TimeTicks::Now());
}

}  // namespace starry
