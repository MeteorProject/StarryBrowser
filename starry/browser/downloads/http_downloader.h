// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_HTTP_DOWNLOADER_H_
#define STARRY_BROWSER_DOWNLOADS_HTTP_DOWNLOADER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/cookies/canonical_cookie.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "starry/browser/downloads/download_model.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {
struct RedirectInfo;
}

namespace network {
class PendingSharedURLLoaderFactory;
class SharedURLLoaderFactory;
namespace mojom {
class URLResponseHead;
}
}  // namespace network

namespace starry {

// 下载器报的错。界面线程上才换成文字（DownloadErrorText），存盘时存的也是这个。
struct DownloadError {
  enum class Kind {
    kNone,
    // 打不开、建不了临时文件（detail 是系统给的原因）。
    kCreateFailed,
    kWriteFailed,
    kDiskFull,
    // 开始前发现磁盘放不下（needed、available）。
    kDiskSpace,
    kInvalidResponse,
    kConnectionClosed,
    // 服务器回了出错的状态码（status）。
    kHttpStatus,
    // 文件在服务器上变了，重来了两次还是对不上。
    kFileChanged,
    // 下完了，改不回原名。
    kSaveFailed,
    kNoDestination,
    // 浏览器上次没正常退出，下载断了。
    kInterrupted,
  };

  Kind kind = Kind::kNone;
  int status = 0;
  std::string detail;
  int64_t needed = 0;
  int64_t available = 0;

  explicit operator bool() const { return kind != Kind::kNone; }
};

// Starry 自己的 HTTP 下载器：分段（Range）、多个连接同时下载、断点续传。
//
// - 第一个请求带 `Range: bytes=0-` 探测：回 206 就确认服务器支持分段，记下总大小和
//   ETag / Last-Modified，然后按 NextSegment 把剩得最多的段对半分给新的连接，直到连接数
//   用满；回 200 就只用这一个连接从头下完。
// - 请求走用户资料的网络（Chrome 的网络栈：代理、证书、HSTS 都和网页一样），每个连接一个
//   SimpleURLLoader。不让网络栈自己带 Cookie（credentials 是 omit）：这样的请求在另一组
//   连接池里（隐私模式），不占网页和 Chrome 自己的下载对同一主机的连接。Cookie 从用户资料的
//   Cookie 存储里按网址取出来自己带上（SameSite=Strict 的只在同站发起时带），跨站重定向时
//   去掉，后加的连接直接请求重定向后的网址。请求不受每个主机 6 个连接的限制
//   （LOAD_IGNORE_LIMITS）：HTTP/1.1 的服务器上是几条 TCP 连接；HTTP/2 的服务器上 Chrome
//   把它们并进同一条连接的几个流（没有单个请求不用 HTTP/2 的办法），和网页的连接分开。要登
//   录（HTTP 认证）的服务器回 401，交回 Chrome。
// - 续传带 `If-Range`：文件在服务器上变了会回 200（或者 Content-Range 对不上），这时
//   丢掉已经下载的部分从头开始，一次下载里最多重来两次。
// - 连接出错按 RetryDelay 重试；别的连接还在下载时，反复失败、被拒绝或被限流（429、
//   503）的连接让出它的段，最多连接数减一。
// - 限速：所有下载共用一个令牌桶（DownloadThrottle）。支持分段时每个连接一次只请求一小块
//   （约半秒的量，64 KB–4 MB），请求前先预定令牌，欠的要等多久就等多久：服务器只发请求的
//   那一块，数据管道、套接字里缓存的也就超不过它。下载中途打开限速时，正在进行的大请求停掉
//   改成小块。不支持分段的单线程下载按收到的量取令牌，欠下的还清了再读下一块（网络栈不再从
//   套接字里读，服务器慢下来），开头会多冲一个数据管道的量。
// - 数据按偏移写进临时文件（`<文件名>.starrydownload`），完成后由 StarryDownload 改名。
//
// 整个对象只在自己的序列上（可以阻塞，文件读写直接做），界面线程用 base::SequenceBound
// 调它。结果经 `report`（已经转到界面线程）报回去。
class HttpDownloader {
 public:
  struct Request {
    Request();
    Request(const Request&);
    Request& operator=(const Request&);
    ~Request();

    // 原始网址：续传时从这里重新请求（重定向后的网址可能已经过期）。
    GURL url;
    GURL referrer;
    std::optional<url::Origin> initiator;
    base::FilePath partial_file;
    int max_connections = 1;
    bool retries = true;
    // 带不带 Cookie（用户在设置里屏蔽了这个网站的 Cookie 时不带）。
    bool send_cookies = true;
    int64_t minimum_segment_size = kMinimumSegmentSize;
  };

  // 要存盘、用来续传的进度。
  struct Progress {
    Progress();
    Progress(const Progress&);
    Progress& operator=(const Progress&);
    ~Progress();

    int64_t total_bytes = -1;
    RangeSupport range_support = RangeSupport::kUnknown;
    std::string etag;
    std::string last_modified;
    std::vector<DownloadSegment> segments;
    GURL final_url;
    std::string mime_type;

    int64_t received() const { return ReceivedBytes(segments); }
    bool complete() const;
  };

  struct Snapshot {
    Progress progress;
    // 正在传数据的连接。
    int connections = 0;
    // 所有连接都在等着重试时：第几次。
    std::optional<int> retry_attempt;
    bool connecting = false;
  };

  struct Outcome {
    enum class Kind {
      // 拿到了第一个回应：大小、是否支持分段。
      kResponded,
      kFinished,
      kFailed,
    };
    Outcome();
    Outcome(const Outcome&);
    Outcome& operator=(const Outcome&);
    ~Outcome();

    Kind kind = Kind::kResponded;
    Progress progress;
    DownloadError error;
    // 服务器拒绝了 Starry 的请求（4xx），而且一点数据都还没收到：值得交回 Chrome
    // 用网页自己的方式再试一次。
    bool rejected = false;
  };
  using ReportCallback = base::RepeatingCallback<void(const Outcome&)>;

  HttpDownloader(
      std::unique_ptr<network::PendingSharedURLLoaderFactory> factory,
      mojo::PendingRemote<network::mojom::CookieManager> cookie_manager,
      const Request& request,
      const Progress& progress,
      ReportCallback report);
  HttpDownloader(const HttpDownloader&) = delete;
  HttpDownloader& operator=(const HttpDownloader&) = delete;
  ~HttpDownloader();

  void Start();
  // 停下来、把数据刷到磁盘并关闭文件，返回可以续传的进度。
  Progress Pause();
  // 停下来并删掉临时文件。
  void Cancel();
  Snapshot GetSnapshot() const;
  // 先把已经写的数据刷到磁盘再返回进度：存盘的进度不会比文件里的数据多。
  Progress Checkpoint();
  // 设置里改了连接数（或限速变了）：多了马上补上，少了等连接下完手上的段再收。
  void SetMaxConnections(int count);

 private:
  struct Connection;

  void Begin();
  void TrimSegmentsToFileLength();
  void StartOver();
  Connection* AddConnection();
  Connection* FindConnection(int id);
  void Send(Connection* connection, int64_t from, int64_t to, bool validated);
  void SendWithCookies(int id,
                       int serial,
                       const GURL& url,
                       int64_t from,
                       int64_t to,
                       bool validated,
                       const net::CookieAccessResultList& cookies,
                       const net::CookieAccessResultList& excluded);
  void OnRedirect(const GURL& url_before_redirect,
                  const net::RedirectInfo& redirect_info,
                  const network::mojom::URLResponseHead& head,
                  std::vector<std::string>* removed_headers);
  // `reserved`：限速时这一块的令牌已经预定过了（等完令牌回来）。
  void Run(Connection* connection, size_t segment, bool reserved = false);
  void RunReserved(int id);
  // 限速时一次请求多大的一块；不限速时是空的。
  std::optional<int64_t> ChunkSize() const;
  void Fill();
  void Reassign(Connection* connection);
  void Retire(Connection* connection);
  void DiscardConnections();

  // SimpleURLLoader 的回调（Connection 转过来）。
  void OnResponseStarted(int id,
                         const GURL& requested_url,
                         const GURL& final_url,
                         const network::mojom::URLResponseHead& head);
  bool AcceptProbe(Connection* connection,
                   const GURL& final_url,
                   const network::mojom::URLResponseHead& head);
  void OnData(Connection* connection,
              std::string_view data,
              base::OnceClosure resume);
  void OnComplete(Connection* connection, bool success);
  void OnIdleTimeout(int id);
  void ResumeReading(int id);
  void RetryConnection(int id);

  void SegmentDone(Connection* connection);
  void ConnectionFailed(Connection* connection,
                        DownloadError error,
                        bool refresh_url = false);
  void GiveUp(Connection* connection);
  void FileChanged();
  std::optional<DownloadError> SpaceProblem() const;
  bool Write(std::string_view data, int64_t offset);

  void Finish();
  void Fail(DownloadError error, bool rejected);
  void ShutDown();
  void CloseFile();
  void Report(Outcome::Kind kind,
              DownloadError error = {},
              bool rejected = false);

  SEQUENCE_CHECKER(sequence_checker_);

  std::unique_ptr<network::PendingSharedURLLoaderFactory> pending_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  mojo::PendingRemote<network::mojom::CookieManager> pending_cookie_manager_;
  mojo::Remote<network::mojom::CookieManager> cookie_manager_;
  const Request request_;
  const ReportCallback report_;
  Progress progress_;
  std::vector<std::unique_ptr<Connection>> connections_;
  int next_connection_id_ = 1;
  int connection_limit_;
  base::File file_;
  bool stopped_ = true;
  // 这次下载里文件变了、从头开始的次数：服务器每次都回不一样的内容时不无限重来。
  int restarts_ = 0;
  // 这次拿到的最终网址（重定向之后），后加的连接直接请求它。
  GURL live_url_;
  // 429 / 503 指定的最早请求时间；所有段共用，正在接收的数据不受影响。
  base::TimeTicks retry_after_;

  base::WeakPtrFactory<HttpDownloader> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_HTTP_DOWNLOADER_H_
