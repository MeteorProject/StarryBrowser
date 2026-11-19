// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/http_downloader.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/isolation_info.h"
#include "net/base/load_flags.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/referrer_policy.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace starry {

namespace {

// 一个连接这么久没收到数据就当它断了，按出错重试。
constexpr base::TimeDelta kIdleTimeout = base::Seconds(30);
// 限速时要等的时间短于这个就不等了。
constexpr base::TimeDelta kMinimumWait = base::Milliseconds(5);
// 限速时一次请求的大小：每个连接大约半秒的量。
constexpr int64_t kMinimumChunk = 64 * 1024;
constexpr int64_t kMaximumChunk = 4 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("starry_segmented_download", R"(
        semantics {
          sender: "Starry downloads"
          description:
            "A file the user is downloading, fetched again by the browser "
            "itself in several ranges at once (multi-connection download), "
            "or resumed from where it stopped."
          trigger:
            "The user starts a download from a server that supports range "
            "requests, or resumes such a download."
          data: "None, other than the request for the file."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting:
            "Multi-connection downloads can be turned off in Starry's "
            "download settings; the file is then fetched over one connection."
          policy_exception_justification: "User initiated."
        })");

DownloadError ErrorOf(DownloadError::Kind kind) {
  DownloadError error;
  error.kind = kind;
  return error;
}

DownloadError StatusError(int status) {
  DownloadError error = ErrorOf(DownloadError::Kind::kHttpStatus);
  error.status = status;
  return error;
}

DownloadError FileError(DownloadError::Kind kind, base::File::Error code) {
  DownloadError error = ErrorOf(kind);
  error.detail = base::File::ErrorToString(code);
  return error;
}

std::string Header(const net::HttpResponseHeaders& headers,
                   std::string_view name) {
  return headers.GetNormalizedHeader(name).value_or(std::string());
}

}  // namespace

// 一个连接：一个 SimpleURLLoader，下载一段（或者探测）。
struct HttpDownloader::Connection
    : public network::SimpleURLLoaderStreamConsumer {
  Connection(HttpDownloader* owner, int id) : owner(owner), id(id) {}
  ~Connection() override = default;

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view data,
                      base::OnceClosure resume_reading) override {
    owner->OnData(this, data, std::move(resume_reading));
  }
  void OnComplete(bool success) override { owner->OnComplete(this, success); }
  void OnRetry(base::OnceClosure start_retry) override {}

  // 停掉请求（不会再有回调），状态留着。
  void Stop() {
    loader.reset();
    resume.Reset();
    idle_timer.Stop();
  }

  const raw_ptr<HttpDownloader> owner;
  const int id;
  std::unique_ptr<network::SimpleURLLoader> loader;
  // 正在下载的段；探测请求还没回应时是空的。
  std::optional<size_t> segment;
  bool probing = false;
  int attempt = 0;
  // 每发一次请求加一：取 Cookie 回来时连接已经发了别的请求就不算。
  int serial = 0;
  bool responded = false;
  bool waiting_for_retry = false;
  // 这次请求到哪里为止（不含）。限速时一次只请求一小块。
  int64_t request_end = DownloadSegment::kOpenEnd;
  // 限速：在等预定的令牌，等到了再请求下一块。
  bool pacing = false;
  // 限速（不支持分段的单线程下载）：在等令牌，等到了再读下一块。
  base::OnceClosure resume;
  base::OneShotTimer idle_timer;
};

HttpDownloader::Request::Request() = default;
HttpDownloader::Request::Request(const Request&) = default;
HttpDownloader::Request& HttpDownloader::Request::operator=(const Request&) =
    default;
HttpDownloader::Request::~Request() = default;

HttpDownloader::Progress::Progress() = default;
HttpDownloader::Progress::Progress(const Progress&) = default;
HttpDownloader::Progress& HttpDownloader::Progress::operator=(const Progress&) =
    default;
HttpDownloader::Progress::~Progress() = default;

bool HttpDownloader::Progress::complete() const {
  return total_bytes >= 0 &&
         std::ranges::all_of(segments, &DownloadSegment::complete);
}

HttpDownloader::Outcome::Outcome() = default;
HttpDownloader::Outcome::Outcome(const Outcome&) = default;
HttpDownloader::Outcome& HttpDownloader::Outcome::operator=(const Outcome&) =
    default;
HttpDownloader::Outcome::~Outcome() = default;

HttpDownloader::HttpDownloader(
    std::unique_ptr<network::PendingSharedURLLoaderFactory> factory,
    mojo::PendingRemote<network::mojom::CookieManager> cookie_manager,
    const Request& request,
    const Progress& progress,
    ReportCallback report)
    : pending_factory_(std::move(factory)),
      pending_cookie_manager_(std::move(cookie_manager)),
      request_(request),
      report_(std::move(report)),
      progress_(progress),
      connection_limit_(std::max(1, request.max_connections)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

HttpDownloader::~HttpDownloader() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ShutDown();
}

// ---- 对外 ----

void HttpDownloader::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!factory_) {
    factory_ =
        network::SharedURLLoaderFactory::Create(std::move(pending_factory_));
  }
  if (pending_cookie_manager_) {
    cookie_manager_.Bind(std::move(pending_cookie_manager_));
  }
  Begin();
}

HttpDownloader::Progress HttpDownloader::Pause() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ShutDown();
  return progress_;
}

void HttpDownloader::Cancel() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ShutDown();
  base::DeleteFile(request_.partial_file);
}

HttpDownloader::Snapshot HttpDownloader::GetSnapshot() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Snapshot snapshot;
  snapshot.progress = progress_;
  std::optional<int> waiting;
  for (const std::unique_ptr<Connection>& connection : connections_) {
    if (connection->waiting_for_retry) {
      waiting = std::max(waiting.value_or(1), connection->attempt);
    } else if ((connection->loader && connection->responded &&
                connection->segment) ||
               connection->pacing) {
      ++snapshot.connections;
    }
  }
  if (snapshot.connections == 0) {
    snapshot.retry_attempt = waiting;
  }
  snapshot.connecting = !stopped_ && progress_.total_bytes < 0 &&
                        progress_.received() == 0 && snapshot.connections == 0;
  return snapshot;
}

HttpDownloader::Progress HttpDownloader::Checkpoint() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (file_.IsValid()) {
    file_.Flush();
  }
  return progress_;
}

void HttpDownloader::SetMaxConnections(int count) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  connection_limit_ = std::max(1, count);
  Fill();
}

// ---- 开始 ----

void HttpDownloader::Begin() {
  stopped_ = false;
  const bool resuming = progress_.range_support == RangeSupport::kSupported &&
                        progress_.total_bytes > 0 &&
                        !progress_.segments.empty() &&
                        base::PathExists(request_.partial_file);
  base::CreateDirectory(request_.partial_file.DirName());
  file_ = base::File(request_.partial_file, base::File::FLAG_OPEN_ALWAYS |
                                                base::File::FLAG_READ |
                                                base::File::FLAG_WRITE);
  if (!file_.IsValid()) {
    Fail(FileError(DownloadError::Kind::kCreateFailed, file_.error_details()),
         /*rejected=*/false);
    return;
  }
  if (!resuming) {
    StartOver();
    return;
  }
  TrimSegmentsToFileLength();
  if (progress_.complete()) {
    Finish();
  } else {
    Fill();
  }
}

// 文件比记录的短（被截断过，或者上次没来得及写进磁盘）：段里超出文件长度的部分算没下载。
void HttpDownloader::TrimSegmentsToFileLength() {
  const int64_t length = file_.GetLength();
  if (length < 0) {
    return;
  }
  for (DownloadSegment& segment : progress_.segments) {
    if (segment.position() > length) {
      segment.received = std::max<int64_t>(0, length - segment.start);
    }
  }
}

// 清空文件，用一个探测请求从头开始。
void HttpDownloader::StartOver() {
  DiscardConnections();
  file_.SetLength(0);
  progress_.segments.clear();
  progress_.total_bytes = -1;
  progress_.range_support = RangeSupport::kUnknown;
  progress_.etag.clear();
  progress_.last_modified.clear();
  live_url_ = GURL();
  Connection* connection = AddConnection();
  connection->probing = true;
  // 限速时探测也只要一小块。
  if (const std::optional<int64_t> chunk = ChunkSize()) {
    DownloadThrottle::Get().Consume(*chunk);
    connection->request_end = *chunk;
  } else {
    connection->request_end = DownloadSegment::kOpenEnd;
  }
  Send(connection, 0, connection->request_end, /*validated=*/false);
}

std::optional<int64_t> HttpDownloader::ChunkSize() const {
  const int64_t limit = DownloadThrottle::Get().limit();
  if (limit <= 0) {
    return std::nullopt;
  }
  return std::clamp<int64_t>(limit / 2 / std::max(1, connection_limit_),
                             kMinimumChunk, kMaximumChunk);
}

HttpDownloader::Connection* HttpDownloader::AddConnection() {
  connections_.push_back(
      std::make_unique<Connection>(this, next_connection_id_++));
  return connections_.back().get();
}

HttpDownloader::Connection* HttpDownloader::FindConnection(int id) {
  for (const std::unique_ptr<Connection>& connection : connections_) {
    if (connection->id == id) {
      return connection.get();
    }
  }
  return nullptr;
}

// 请求 [from, to)；to 是 kOpenEnd 时请求到文件末尾。先取这个网址的 Cookie（异步），再发。
void HttpDownloader::Send(Connection* connection,
                          int64_t from,
                          int64_t to,
                          bool validated) {
  // 自动重试和手动继续都从原始网址取新的重定向地址。
  const GURL url = connection->attempt == 0 && live_url_.is_valid()
                       ? live_url_
                       : request_.url;
  connection->Stop();
  connection->responded = false;
  connection->waiting_for_retry = false;
  const int serial = ++connection->serial;
  connection->idle_timer.Start(
      FROM_HERE, kIdleTimeout,
      base::BindOnce(&HttpDownloader::OnIdleTimeout, weak_factory_.GetWeakPtr(),
                     connection->id));
  auto send = base::BindOnce(&HttpDownloader::SendWithCookies,
                             weak_factory_.GetWeakPtr(), connection->id, serial,
                             url, from, to, validated);
  if (!request_.send_cookies || !cookie_manager_) {
    std::move(send).Run({}, {});
    return;
  }
  // 同顶层导航：同站发起（或者浏览器自己发起）时带 SameSite=Strict 的，跨站时只带 Lax 的。
  const bool same_site =
      !request_.initiator ||
      (!request_.initiator->opaque() &&
       net::SchemefulSite(*request_.initiator) == net::SchemefulSite(url));
  net::CookieOptions options;
  options.set_include_httponly();
  options.set_same_site_cookie_context(
      net::CookieOptions::SameSiteCookieContext(
          same_site ? net::CookieOptions::SameSiteCookieContext::ContextType::
                          SAME_SITE_STRICT
                    : net::CookieOptions::SameSiteCookieContext::ContextType::
                          SAME_SITE_LAX));
  // 网络服务重启了（连接断了）就不带 Cookie 发。
  cookie_manager_->GetCookieList(
      url, options, net::CookiePartitionKeyCollection(),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(send), net::CookieAccessResultList(),
          net::CookieAccessResultList()));
}

void HttpDownloader::SendWithCookies(
    int id,
    int serial,
    const GURL& url,
    int64_t from,
    int64_t to,
    bool validated,
    const net::CookieAccessResultList& cookies,
    const net::CookieAccessResultList& excluded) {
  Connection* connection = FindConnection(id);
  if (stopped_ || !connection || connection->serial != serial) {
    return;
  }
  // 别的段也可能刚收到 Retry-After（包括在异步取 Cookie 的时候）。现有的流
  // 可以继续读，新请求统一等到期限后再取当时的网址和 Cookie。
  const base::TimeDelta wait = retry_after_ - base::TimeTicks::Now();
  if (wait.is_positive()) {
    connection->waiting_for_retry = true;
    connection->idle_timer.Start(
        FROM_HERE, wait,
        base::BindOnce(&HttpDownloader::Send, base::Unretained(this),
                       base::Unretained(connection), from, to, validated));
    return;
  }
  auto resource = std::make_unique<network::ResourceRequest>();
  resource->url = url;
  resource->method = net::HttpRequestHeaders::kGetMethod;
  resource->request_initiator = request_.initiator;
  resource->referrer = request_.referrer;
  resource->referrer_policy =
      net::ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN;
  const url::Origin origin = url::Origin::Create(url);
  resource->trusted_params = network::ResourceRequest::TrustedParams();
  resource->trusted_params->isolation_info = net::IsolationInfo::Create(
      net::IsolationInfo::RequestType::kOther, origin, origin,
      net::SiteForCookies::FromOrigin(origin));
  // 网络栈不带、不存 Cookie（请求落在隐私模式那一组连接池里），Cookie 自己带。
  resource->mode = network::mojom::RequestMode::kNoCors;
  resource->credentials_mode = network::mojom::CredentialsMode::kOmit;
  if (!cookies.empty()) {
    resource->headers.SetHeader(net::HttpRequestHeaders::kCookie,
                                net::CanonicalCookie::BuildCookieLine(cookies));
  }
  resource->do_not_prompt_for_login = true;
  // 不受每个主机 6 个连接的限制（HTTP/1.1 的服务器上多出来的连接、同一主机的几个下载都不用
  // 排队等套接字）。这个标志要配最高的优先级，网络服务也不再改它的优先级；这些连接和网页的
  // 不在同一组里，抢不到网页的。
  resource->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_IGNORE_LIMITS;
  resource->priority = net::MAXIMUM_PRIORITY;
  // 分段只对原始字节有意义：不要压缩。
  resource->headers.SetHeader(net::HttpRequestHeaders::kAcceptEncoding,
                              "identity");
  resource->headers.SetHeader(
      net::HttpRequestHeaders::kRange,
      to == DownloadSegment::kOpenEnd
          ? base::StrCat({"bytes=", base::NumberToString(from), "-"})
          : base::StrCat({"bytes=", base::NumberToString(from), "-",
                          base::NumberToString(to - 1)}));
  if (validated) {
    const std::string& validator =
        !progress_.etag.empty() ? progress_.etag : progress_.last_modified;
    if (!validator.empty()) {
      resource->headers.SetHeader(net::HttpRequestHeaders::kIfRange, validator);
    }
  }

  connection->loader =
      network::SimpleURLLoader::Create(std::move(resource), kTrafficAnnotation);
  connection->loader->SetOnResponseStartedCallback(
      base::BindOnce(&HttpDownloader::OnResponseStarted,
                     weak_factory_.GetWeakPtr(), connection->id, url));
  connection->loader->SetOnRedirectCallback(base::BindRepeating(
      &HttpDownloader::OnRedirect, weak_factory_.GetWeakPtr()));
  connection->loader->DownloadAsStream(factory_.get(), connection);
}

// 重定向到别的源：原来的 Cookie 不带过去（SimpleURLLoader 换不了请求头，只能去掉）。后加的
// 连接直接请求重定向后的网址，带那里的 Cookie。
void HttpDownloader::OnRedirect(const GURL& url_before_redirect,
                                const net::RedirectInfo& redirect_info,
                                const network::mojom::URLResponseHead& head,
                                std::vector<std::string>* removed_headers) {
  if (!url::IsSameOriginWith(url_before_redirect, redirect_info.new_url)) {
    removed_headers->push_back(net::HttpRequestHeaders::kCookie);
  }
}

// 请求这一段剩下的部分。限速时只请求一块，并且先从令牌桶里预定这一块：欠下的要等多久
// 就等多久再请求。等的时候段可能被别的连接分走一半，到时按当时的样子请求。
void HttpDownloader::Run(Connection* connection, size_t index, bool reserved) {
  connection->segment = index;
  connection->probing = false;
  connection->pacing = false;
  if (progress_.range_support != RangeSupport::kSupported) {
    // 不支持分段时不能接着下，只能从头来。
    StartOver();
    return;
  }
  const DownloadSegment& segment = progress_.segments[index];
  int64_t end = segment.end;
  if (const std::optional<int64_t> chunk = ChunkSize()) {
    end = std::min(end, segment.position() + *chunk);
    if (!reserved) {
      const base::TimeDelta wait =
          DownloadThrottle::Get().Consume(end - segment.position());
      if (wait >= kMinimumWait) {
        connection->Stop();
        connection->pacing = true;
        base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&HttpDownloader::RunReserved,
                           weak_factory_.GetWeakPtr(), connection->id),
            wait);
        return;
      }
    }
  }
  connection->request_end = end;
  Send(connection, segment.position(), end, /*validated=*/true);
}

void HttpDownloader::RunReserved(int id) {
  Connection* connection = FindConnection(id);
  if (stopped_ || !connection || !connection->pacing || !connection->segment) {
    return;
  }
  connection->pacing = false;
  if (progress_.segments[*connection->segment].complete()) {
    SegmentDone(connection);
  } else {
    Run(connection, *connection->segment, /*reserved=*/true);
  }
}

// ---- 分配 ----

// 补满连接：先给没人下的段，再对半分剩得最多的段。
void HttpDownloader::Fill() {
  if (stopped_) {
    return;
  }
  while (static_cast<int>(connections_.size()) < connection_limit_) {
    std::set<size_t> busy;
    for (const std::unique_ptr<Connection>& connection : connections_) {
      if (connection->segment) {
        busy.insert(*connection->segment);
      }
    }
    const std::optional<SegmentAssignment> next =
        NextSegment(progress_.segments, busy,
                    progress_.range_support == RangeSupport::kSupported,
                    request_.minimum_segment_size);
    if (!next) {
      break;
    }
    const size_t index = ApplySegment(*next, progress_.segments);
    Run(AddConnection(), index);
  }
}

// 一个连接空下来了：接新的段，没有可接的就关掉。
void HttpDownloader::Reassign(Connection* connection) {
  connection->segment.reset();
  connection->attempt = 0;
  if (static_cast<int>(connections_.size()) - 1 >= connection_limit_) {
    Retire(connection);
    return;
  }
  std::set<size_t> busy;
  for (const std::unique_ptr<Connection>& other : connections_) {
    if (other.get() != connection && other->segment) {
      busy.insert(*other->segment);
    }
  }
  const std::optional<SegmentAssignment> next =
      NextSegment(progress_.segments, busy,
                  progress_.range_support == RangeSupport::kSupported,
                  request_.minimum_segment_size);
  if (!next) {
    Retire(connection);
    return;
  }
  Run(connection, ApplySegment(*next, progress_.segments));
}

void HttpDownloader::Retire(Connection* connection) {
  std::erase_if(connections_,
                [connection](const std::unique_ptr<Connection>& candidate) {
                  return candidate.get() == connection;
                });
}

void HttpDownloader::DiscardConnections() {
  connections_.clear();
}

// ---- 回应 ----

void HttpDownloader::OnResponseStarted(
    int id,
    const GURL& requested_url,
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Connection* connection = FindConnection(id);
  if (stopped_ || !connection) {
    return;
  }
  if (!head.headers) {
    connection->Stop();
    ConnectionFailed(connection,
                     ErrorOf(DownloadError::Kind::kInvalidResponse));
    return;
  }
  connection->responded = true;
  const int status = head.headers->response_code();
  if (status == 429 || status == 503) {
    base::TimeDelta delay;
    if (net::HttpUtil::ParseRetryAfterHeader(
            Header(*head.headers, "Retry-After"), base::Time::Now(), &delay)) {
      retry_after_ = std::max(retry_after_, base::TimeTicks::Now() + delay);
    }
  }
  if (connection->probing) {
    AcceptProbe(connection, final_url, head);
    return;
  }
  if (!connection->segment) {
    Retire(connection);
    return;
  }
  const DownloadSegment segment = progress_.segments[*connection->segment];
  switch (status) {
    case 206: {
      const std::optional<ContentRange> range =
          ContentRange::Parse(Header(*head.headers, "Content-Range"));
      const bool use_etag = !progress_.etag.empty();
      const std::string& validator =
          use_etag ? progress_.etag : progress_.last_modified;
      const std::string actual =
          Header(*head.headers, use_etag ? "ETag" : "Last-Modified");
      if (!range || range->start != segment.position() ||
          (range->total && *range->total != progress_.total_bytes) ||
          (!validator.empty() && !actual.empty() && validator != actual)) {
        connection->Stop();
        FileChanged();
      } else if (requested_url == request_.url) {
        // 只有从原始入口取得的有效分段可以更新地址。旧连接晚到的回应不能覆盖它。
        live_url_ = final_url;
        progress_.final_url = final_url;
      }
      return;
    }
    case 200:
      // If-Range 对不上（文件变了），或者服务器不再支持分段。
      connection->Stop();
      FileChanged();
      return;
    case 416:
      if (segment.complete()) {
        connection->Stop();
        SegmentDone(connection);
        return;
      }
      [[fallthrough]];
    default: {
      // 签名过期时 CDN 可能回 403、404 或 GitHub 的 618。直接请求临时地址
      // 才值得回原始入口再试；经原始入口仍被拒绝就走原来的失败处理。
      const bool refresh = request_.retries && requested_url != request_.url &&
                           (status == 401 || status == 403 || status == 404 ||
                            status == 410 || status == 618);
      if (refresh && live_url_ == requested_url) {
        live_url_ = GURL();
      }
      connection->Stop();
      ConnectionFailed(connection, StatusError(status), refresh);
      return;
    }
  }
}

// 探测请求的回应：决定单线程还是分段，记下大小和校验用的 ETag / Last-Modified。
bool HttpDownloader::AcceptProbe(Connection* connection,
                                 const GURL& final_url,
                                 const network::mojom::URLResponseHead& head) {
  const net::HttpResponseHeaders& headers = *head.headers;
  progress_.final_url = final_url;
  progress_.mime_type = head.mime_type;
  const std::string encoding =
      base::ToLowerASCII(Header(headers, "Content-Encoding"));
  const bool encoded = !encoding.empty() && encoding != "identity";
  const int status = headers.response_code();
  const std::optional<ContentRange> range =
      ContentRange::Parse(Header(headers, "Content-Range"));
  const bool segmented =
      status == 206 && range && range->start == 0 && range->total && !encoded;
  const bool whole =
      status == 200 || (status == 206 && range && range->start == 0);
  if (segmented) {
    live_url_ = final_url;
    progress_.range_support = RangeSupport::kSupported;
    progress_.total_bytes = *range->total;
    // 弱 ETag 不能用在 If-Range 里。
    progress_.etag = StrongETag(Header(headers, "ETag"));
    progress_.last_modified = Header(headers, "Last-Modified");
    progress_.segments = {
        DownloadSegment{.start = 0, .end = *range->total, .received = 0}};
    connection->request_end = range->end + 1;
  } else if (whole) {
    // 服务器忽略了 Range（或者不知道总大小、内容压缩过）：只用这一个连接从头下完。
    progress_.range_support = RangeSupport::kUnsupported;
    int64_t length = -1;
    if (!encoded) {
      length = status == 206 ? range->total.value_or(-1) : head.content_length;
    }
    progress_.total_bytes = length >= 0 ? length : -1;
    progress_.segments = {
        DownloadSegment{.start = 0,
                        .end = length >= 0 ? length : DownloadSegment::kOpenEnd,
                        .received = 0}};
    connection->request_end = DownloadSegment::kOpenEnd;
  } else if (status == 416 && ContentRange::UnsatisfiedTotal(
                                  Header(headers, "Content-Range")) == 0) {
    // 空文件：bytes=0- 没有可给的内容。
    connection->Stop();
    progress_.range_support = RangeSupport::kSupported;
    progress_.total_bytes = 0;
    progress_.segments.clear();
    Report(Outcome::Kind::kResponded);
    Finish();
    return false;
  } else {
    connection->Stop();
    ConnectionFailed(connection, StatusError(status));
    return false;
  }
  if (std::optional<DownloadError> problem = SpaceProblem()) {
    Fail(*problem, /*rejected=*/false);
    return false;
  }
  connection->probing = false;
  connection->segment = 0;
  Report(Outcome::Kind::kResponded);
  if (progress_.total_bytes == 0) {
    connection->Stop();
    Finish();
    return false;
  }
  Fill();
  return true;
}

void HttpDownloader::OnData(Connection* connection,
                            std::string_view data,
                            base::OnceClosure resume) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (stopped_ || !connection->segment) {
    return;
  }
  connection->idle_timer.Reset();
  const size_t index = *connection->segment;
  const DownloadSegment segment = progress_.segments[index];
  const int64_t room = std::max<int64_t>(
      0, std::min(segment.remaining(),
                  connection->request_end - segment.position()));
  const int64_t count = std::min<int64_t>(data.size(), room);
  if (count > 0) {
    if (!Write(data.substr(0, count), segment.position())) {
      return;
    }
    progress_.segments[index].received += count;
    connection->attempt = 0;
  }
  const int64_t position = progress_.segments[index].position();
  if (progress_.segments[index].complete()) {
    // 段下完了，或者被别的连接分走了一半：不再要这个请求后面的数据。
    connection->Stop();
    SegmentDone(connection);
    return;
  }
  if (progress_.range_support == RangeSupport::kSupported) {
    const std::optional<int64_t> chunk = ChunkSize();
    // 限速时请求的一块下完了；或者下载中途打开了限速：停掉这个大请求，改成一块一块地请求。
    if (position >= connection->request_end ||
        (chunk && connection->request_end - position > 2 * *chunk)) {
      connection->Stop();
      connection->attempt = 0;
      Run(connection, index);
      return;
    }
    // 令牌在请求前已经预定过了。
    std::move(resume).Run();
    return;
  }
  // 不支持分段的单线程下载：按收到的量取令牌，等令牌时先不读下一块。
  const base::TimeDelta wait = DownloadThrottle::Get().Consume(count);
  if (wait < kMinimumWait) {
    std::move(resume).Run();
    return;
  }
  connection->resume = std::move(resume);
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&HttpDownloader::ResumeReading, weak_factory_.GetWeakPtr(),
                     connection->id),
      wait);
  // 等令牌的时候不算没数据。
  connection->idle_timer.Start(
      FROM_HERE, wait + kIdleTimeout,
      base::BindOnce(&HttpDownloader::OnIdleTimeout, weak_factory_.GetWeakPtr(),
                     connection->id));
}

void HttpDownloader::ResumeReading(int id) {
  Connection* connection = FindConnection(id);
  if (stopped_ || !connection || !connection->resume) {
    return;
  }
  connection->idle_timer.Reset();
  std::move(connection->resume).Run();
}

void HttpDownloader::OnComplete(Connection* connection, bool success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (stopped_) {
    return;
  }
  const int net_error = connection->loader ? connection->loader->NetError() : 0;
  connection->Stop();
  if (connection->segment) {
    DownloadSegment& segment = progress_.segments[*connection->segment];
    if (segment.complete()) {
      SegmentDone(connection);
      return;
    }
    // 不知道总大小的单线程下载：连接正常结束就是下完了。
    if (success && segment.end == DownloadSegment::kOpenEnd) {
      segment.end = segment.position();
      progress_.total_bytes = segment.position();
      SegmentDone(connection);
      return;
    }
    // 限速时请求的一块下完了：请求下一块。
    if (success && segment.position() >= connection->request_end) {
      connection->attempt = 0;
      Run(connection, *connection->segment);
      return;
    }
  }
  DownloadError error = ErrorOf(DownloadError::Kind::kConnectionClosed);
  error.status = net_error;
  ConnectionFailed(connection, error);
}

void HttpDownloader::OnIdleTimeout(int id) {
  Connection* connection = FindConnection(id);
  if (stopped_ || !connection || !connection->loader) {
    return;
  }
  connection->Stop();
  ConnectionFailed(connection, ErrorOf(DownloadError::Kind::kConnectionClosed));
}

// ---- 进度与出错 ----

void HttpDownloader::SegmentDone(Connection* connection) {
  if (progress_.complete()) {
    Finish();
  } else {
    Reassign(connection);
  }
}

void HttpDownloader::ConnectionFailed(Connection* connection,
                                      DownloadError error,
                                      bool refresh_url) {
  const bool alone = connections_.size() <= 1;
  const int status = error.status;
  const bool transient = error.kind != DownloadError::Kind::kHttpStatus ||
                         IsTransientStatus(status) || refresh_url;
  // 被拒绝（要登录、没有权限、找不到）：只有这一个连接时整个下载失败；后加的连接被拒就
  // 少用一个连接。
  if (!transient) {
    if (alone) {
      Fail(error, /*rejected=*/progress_.received() == 0 && status >= 400 &&
                      status < 500);
    } else {
      GiveUp(connection);
    }
    return;
  }
  // 服务器限流：别的连接还在下载时少开一个，不重试。
  if ((status == 429 || status == 503) && !alone) {
    GiveUp(connection);
    return;
  }
  ++connection->attempt;
  if (!request_.retries || connection->attempt > kMaxRetryAttempts) {
    if (alone) {
      Fail(error, /*rejected=*/false);
    } else {
      GiveUp(connection);
    }
    return;
  }
  connection->waiting_for_retry = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&HttpDownloader::RetryConnection,
                     weak_factory_.GetWeakPtr(), connection->id),
      std::max(RetryDelay(connection->attempt),
               retry_after_ - base::TimeTicks::Now()));
}

void HttpDownloader::RetryConnection(int id) {
  Connection* connection = FindConnection(id);
  if (stopped_ || !connection || !connection->waiting_for_retry) {
    return;
  }
  connection->waiting_for_retry = false;
  if (connection->segment &&
      progress_.range_support == RangeSupport::kSupported) {
    Run(connection, *connection->segment);
    return;
  }
  // 探测请求失败，或者不支持分段：从头再来（保留重试次数）。
  const int attempt = connection->attempt;
  StartOver();
  connections_.front()->attempt = attempt;
}

// 这个连接不再用：它的段留给别的连接接着下，最多连接数减一。
void HttpDownloader::GiveUp(Connection* connection) {
  Retire(connection);
  connection_limit_ = std::max(1, static_cast<int>(connections_.size()));
}

// 文件在服务器上变了（If-Range 不符、大小变了）：已经下载的部分作废，从头开始。
void HttpDownloader::FileChanged() {
  if (++restarts_ > 2) {
    Fail(ErrorOf(DownloadError::Kind::kFileChanged), /*rejected=*/false);
    return;
  }
  StartOver();
}

std::optional<DownloadError> HttpDownloader::SpaceProblem() const {
  if (progress_.total_bytes <= 0) {
    return std::nullopt;
  }
  const std::optional<int64_t> available =
      base::SysInfo::AmountOfFreeDiskSpace(request_.partial_file.DirName());
  const int64_t needed = progress_.total_bytes - progress_.received();
  if (!available || *available <= 0 || *available >= needed) {
    return std::nullopt;
  }
  DownloadError error = ErrorOf(DownloadError::Kind::kDiskSpace);
  error.needed = progress_.total_bytes;
  error.available = *available;
  return error;
}

bool HttpDownloader::Write(std::string_view data, int64_t offset) {
  const std::optional<size_t> written =
      file_.Write(offset, base::as_byte_span(data));
  if (written && *written == data.size()) {
    return true;
  }
  const base::File::Error code = base::File::GetLastFileError();
  Fail(code == base::File::FILE_ERROR_NO_SPACE
           ? ErrorOf(DownloadError::Kind::kDiskFull)
           : FileError(DownloadError::Kind::kWriteFailed, code),
       /*rejected=*/false);
  return false;
}

// ---- 结束 ----

void HttpDownloader::Finish() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  DiscardConnections();
  if (progress_.total_bytes >= 0) {
    file_.SetLength(progress_.total_bytes);
  }
  CloseFile();
  Report(Outcome::Kind::kFinished);
}

void HttpDownloader::Fail(DownloadError error, bool rejected) {
  if (stopped_ && !file_.IsValid()) {
    return;
  }
  stopped_ = true;
  DiscardConnections();
  CloseFile();
  Report(Outcome::Kind::kFailed, std::move(error), rejected);
}

void HttpDownloader::ShutDown() {
  stopped_ = true;
  DiscardConnections();
  CloseFile();
}

void HttpDownloader::CloseFile() {
  if (file_.IsValid()) {
    file_.Flush();
    file_.Close();
  }
}

void HttpDownloader::Report(Outcome::Kind kind,
                            DownloadError error,
                            bool rejected) {
  Outcome outcome;
  outcome.kind = kind;
  outcome.progress = progress_;
  outcome.error = std::move(error);
  outcome.rejected = rejected;
  report_.Run(outcome);
}

}  // namespace starry
