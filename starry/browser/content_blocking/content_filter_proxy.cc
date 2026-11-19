// Copyright 2026 The Starry Browser Authors

#include "starry/browser/content_blocking/content_filter_proxy.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/byte_size.h"
#include "base/containers/span.h"
#include "base/containers/unique_ptr_adapters.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/content_security_policy/content_security_policy.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/parsed_headers.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "starry/browser/content_blocking/content_filter_tab_helper.h"
#include "starry/browser/content_blocking/filter_config.h"
#include "starry/browser/content_blocking/response_body_filter.h"
#include "starry/components/content_filter/content_filter_types.h"
#include "starry/components/content_filter/filter_engine.h"
#include "starry/components/content_filter/redirect_resources.h"
#include "starry/components/content_filter/request.h"
#include "starry/components/content_filter/response_filter.h"
#include "starry/components/content_filter/tracking_parameters.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace starry::content_blocking {

namespace {

using FactoryType = content::ContentBrowserClient::URLLoaderFactoryType;
using content_filter::NetworkMatch;
using content_filter::RequestType;
using network::mojom::RequestDestination;

// 刚启动、引擎还没建好时请求最多等这么久（建引擎通常几十毫秒）。
constexpr base::TimeDelta kMaxWaitForEngine = base::Seconds(3);
constexpr base::TimeDelta kEngineRetry = base::Milliseconds(15);

// 代理在浏览器的 IO 线程上：网页进程和网络服务的消息本来就在这里到达，不用再换一次
// 线程（放在线程池的序列上时 400 个请求的页面慢 20～40 毫秒）。每个请求只做几微秒的
// 事，不会挡住别的 IPC。
scoped_refptr<base::SequencedTaskRunner> ProxyTaskRunner() {
  return content::GetIOThreadTaskRunner({});
}

// 代理所在的这一组请求是从哪里来的（建代理时在界面线程上定）。
struct ProxyContext {
  FactoryType type = FactoryType::kDocumentSubResource;
  // 发请求的文档（子资源）。
  content::GlobalRenderFrameHostId frame_id;
  // 导航的 frame。
  content::FrameTreeNodeId frame_tree_node_id;
  url::Origin initiator;
  std::optional<url::Origin> top_frame_origin;
};

GURL OriginUrl(const url::Origin& origin) {
  if (origin.opaque()) {
    return origin.GetTupleOrPrecursorTupleIfOpaque().GetURL();
  }
  return origin.GetURL();
}

// 请求的类型（Adblock Plus 的类型选项）。
RequestType TypeOf(const network::ResourceRequest& request, bool navigation) {
  switch (request.destination) {
    case RequestDestination::kDocument:
      return navigation && request.is_outermost_main_frame
                 ? content_filter::kTypeDocument
                 : content_filter::kTypeSubdocument;
    case RequestDestination::kIframe:
    case RequestDestination::kFrame:
    case RequestDestination::kFencedframe:
      return content_filter::kTypeSubdocument;
    case RequestDestination::kScript:
    case RequestDestination::kWorker:
    case RequestDestination::kSharedWorker:
    case RequestDestination::kServiceWorker:
    case RequestDestination::kAudioWorklet:
    case RequestDestination::kPaintWorklet:
      return content_filter::kTypeScript;
    case RequestDestination::kStyle:
    case RequestDestination::kXslt:
      return content_filter::kTypeStylesheet;
    case RequestDestination::kImage:
      return content_filter::kTypeImage;
    case RequestDestination::kFont:
      return content_filter::kTypeFont;
    case RequestDestination::kAudio:
    case RequestDestination::kVideo:
    case RequestDestination::kTrack:
      return content_filter::kTypeMedia;
    case RequestDestination::kObject:
    case RequestDestination::kEmbed:
      return content_filter::kTypeObject;
    case RequestDestination::kEmpty:
      // sendBeacon、<a ping>、fetch 的 keepalive。
      if (request.keepalive) {
        return content_filter::kTypePing;
      }
      return content_filter::kTypeXmlHttpRequest;
    default:
      return content_filter::kTypeOther;
  }
}

// 图片、iframe、插件被拦下时收起来（同 Chrome 扩展 DNR 的「collapse」）。
bool ShouldCollapse(RequestType type) {
  return type == content_filter::kTypeImage ||
         type == content_filter::kTypeSubdocument ||
         type == content_filter::kTypeObject ||
         type == content_filter::kTypeMedia;
}

class ProxyFactory;

// 一个请求（同 Chrome 的 WebRequestProxyingURLLoaderFactory::InProgressRequest）：
// 对网页进程是 URLLoader，对网络服务是 URLLoaderClient，把两边的消息转过去。
class InProgressRequest : public network::mojom::URLLoader,
                          public network::mojom::URLLoaderClient {
 public:
  InProgressRequest(
      ProxyFactory* factory,
      mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation);
  InProgressRequest(const InProgressRequest&) = delete;
  InProgressRequest& operator=(const InProgressRequest&) = delete;
  ~InProgressRequest() override;

  void Start();

  // network::mojom::URLLoader:
  void FollowRedirect(
      network::HttpRequestHeadersUpdateParams headers_update_params,
      const std::optional<GURL>& new_url) override;
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override;

  // network::mojom::URLLoaderClient:
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr early_hints) override;
  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override;
  void OnReceiveRedirect(const net::RedirectInfo& redirect_info,
                         network::mojom::URLResponseHeadPtr head) override;
  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback callback) override;
  void OnTransferSizeUpdated(int32_t transfer_size_diff) override;
  void OnComplete(const network::URLLoaderCompletionStatus& status) override;

 private:
  bool IsNavigation() const;
  bool IsMainFrameNavigation() const;
  // 页面的地址（不拦截的网站、整页例外按它）。
  GURL TopLevelUrl() const;
  // 发出请求的文档（第三方、$domain= 按它）。
  GURL SourceUrl() const;

  void Decide();
  // 主框架的导航：去跟踪参数和 $removeparam，加 CSP。
  void DecideMainFrame(const FilterConfig& config);
  // 被拦的网址：拦下、换资源，否则返回 false。
  bool MaybeBlock(const FilterConfig& config, const GURL& url);
  // 拦下了（或者换了资源）返回 true。
  bool HandleBlocked(const NetworkMatch& match);
  // 留在中间（拦下、换资源、跳转、导航）：接上网页进程那两头。
  void Attach();
  // 不用管的子资源请求：直接交给网络服务，自己退出。
  void PassThrough();
  void StartNetwork();
  void Block(RequestType type);
  void ServeResource(const content_filter::RedirectResource& resource);
  void SendInternalRedirect(const GURL& new_url);
  void ReportBlocked();
  void AddCsp(network::mojom::URLResponseHead* head);
  // 改写响应（`$replace`、页面的 HTML 过滤 `网站##^…`）：去掉 HTML 过滤要去的响应头；要改
  // 响应体时接上 ResponseBodyFilter，把它的管道交给网页进程（body 换掉），返回 true。
  bool MaybeFilterBody(network::mojom::URLResponseHead* head,
                       mojo::ScopedDataPipeConsumerHandle* body);
  void OnBodyFiltered(size_t bytes);
  void Finished();
  void OnClientDisconnected();
  void OnNetworkDisconnected();

  const raw_ptr<ProxyFactory> factory_;
  network::ResourceRequest request_;
  const int32_t request_id_;
  const uint32_t options_;
  const net::MutableNetworkTrafficAnnotationTag traffic_annotation_;
  const base::TimeTicks created_ = base::TimeTicks::Now();

  RequestType type_ = content_filter::kTypeOther;
  // 页面要加的 CSP（只有页面的导航有）。
  std::vector<std::string> csp_;
  // 自己回的跳转（去参数）在等网页跟过来。
  bool internal_redirect_pending_ = false;
  net::RedirectInfo redirect_info_;
  // 服务器跳转的目标去掉跟踪参数后的网址（同源，FollowRedirect 时交给网络服务）。
  std::optional<GURL> redirect_override_;
  bool completed_ = false;
  // 在改响应体：网络那头的 OnComplete 等改完、写完再转。
  std::unique_ptr<ResponseBodyFilter> body_filter_;
  bool body_filtered_ = false;
  size_t filtered_bytes_ = 0;
  std::optional<network::URLLoaderCompletionStatus> pending_status_;

  // 判断之前先不接：不用处理响应的请求直接把这两头交给网络服务（响应不再经过这里）。
  mojo::PendingReceiver<network::mojom::URLLoader> pending_loader_;
  mojo::PendingRemote<network::mojom::URLLoaderClient> pending_client_;
  mojo::Receiver<network::mojom::URLLoader> loader_receiver_{this};
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};
  mojo::Remote<network::mojom::URLLoader> target_loader_;

  base::WeakPtrFactory<InProgressRequest> weak_factory_{this};
};

// 一个被代理的 URLLoaderFactory：自己管自己，网页进程那头和所有请求都断开以后删掉。
class ProxyFactory : public network::mojom::URLLoaderFactory {
 public:
  static void Create(
      ProxyContext context,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target) {
    new ProxyFactory(std::move(context), std::move(receiver),
                     std::move(target));
  }

  ProxyFactory(const ProxyFactory&) = delete;
  ProxyFactory& operator=(const ProxyFactory&) = delete;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    if (!target_.is_bound()) {
      return;
    }
    auto in_progress = std::make_unique<InProgressRequest>(
        this, std::move(loader_receiver), request_id, options, request,
        std::move(client), traffic_annotation);
    InProgressRequest* raw = in_progress.get();
    requests_.insert(std::move(in_progress));
    raw->Start();
  }

  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }

  const ProxyContext& context() const { return context_; }
  network::mojom::URLLoaderFactory* target() { return target_.get(); }

  void RemoveRequest(InProgressRequest* request) {
    auto it = requests_.find(request);
    if (it != requests_.end()) {
      requests_.erase(it);
    }
    MaybeDestroy();
  }

  // 页面的整页例外（按发出请求的文档和顶层页面；同一组请求常常是同一个页面，记住
  // 上一次的）。
  uint32_t DocumentFlags(const content_filter::FilterEngine& engine,
                         const GURL& top,
                         const GURL& source) {
    if (engine.id() == flags_engine_ && top == flags_top_ &&
        source == flags_source_) {
      return flags_;
    }
    flags_engine_ = engine.id();
    flags_top_ = top;
    flags_source_ = source;
    flags_ = engine.DocumentFlags(top, GURL());
    if (source.is_valid() && url::Origin::Create(source) != url::Origin::Create(top)) {
      flags_ |= engine.DocumentFlags(source, top);
    }
    return flags_;
  }

 private:
  ProxyFactory(ProxyContext context,
               mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
               mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
      : context_(std::move(context)) {
    target_.Bind(std::move(target));
    target_.set_disconnect_handler(base::BindOnce(
        &ProxyFactory::OnTargetDisconnected, base::Unretained(this)));
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(
        &ProxyFactory::OnProxyDisconnected, base::Unretained(this)));
  }

  ~ProxyFactory() override = default;

  void OnTargetDisconnected() {
    target_.reset();
    receivers_.Clear();
    MaybeDestroy();
  }

  void OnProxyDisconnected() {
    if (receivers_.empty()) {
      target_.reset();
    }
    MaybeDestroy();
  }

  void MaybeDestroy() {
    // 网页进程那头都断了还要等进行中的请求做完。
    if (target_.is_bound() || !requests_.empty()) {
      return;
    }
    delete this;
  }

  const ProxyContext context_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
  std::set<std::unique_ptr<InProgressRequest>, base::UniquePtrComparator>
      requests_;

  // 算这个缓存时的引擎（FilterEngine::id，换了引擎就不算数）。
  uint64_t flags_engine_ = 0;
  GURL flags_top_;
  GURL flags_source_;
  uint32_t flags_ = 0;
};

InProgressRequest::InProgressRequest(
    ProxyFactory* factory,
    mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
    : factory_(factory),
      request_(request),
      request_id_(request_id),
      options_(options),
      traffic_annotation_(traffic_annotation),
      pending_loader_(std::move(loader_receiver)),
      pending_client_(std::move(client)) {
  type_ = TypeOf(request_, IsNavigation());
}

void InProgressRequest::Attach() {
  if (loader_receiver_.is_bound()) {
    return;
  }
  loader_receiver_.Bind(std::move(pending_loader_));
  client_.Bind(std::move(pending_client_));
  loader_receiver_.set_disconnect_handler(base::BindOnce(
      &InProgressRequest::OnClientDisconnected, base::Unretained(this)));
  client_.set_disconnect_handler(base::BindOnce(
      &InProgressRequest::OnClientDisconnected, base::Unretained(this)));
}

void InProgressRequest::PassThrough() {
  if (IsNavigation() || loader_receiver_.is_bound()) {
    // 导航要留在中间（加 CSP、看跳转）。
    Attach();
    StartNetwork();
    return;
  }
  if (network::mojom::URLLoaderFactory* target = factory_->target()) {
    target->CreateLoaderAndStart(std::move(pending_loader_), request_id_,
                                 options_, request_, std::move(pending_client_),
                                 traffic_annotation_);
  }
  factory_->RemoveRequest(this);
}

InProgressRequest::~InProgressRequest() = default;

bool InProgressRequest::IsNavigation() const {
  return factory_->context().type == FactoryType::kNavigation;
}

bool InProgressRequest::IsMainFrameNavigation() const {
  return IsNavigation() && request_.is_outermost_main_frame &&
         request_.destination == RequestDestination::kDocument;
}

GURL InProgressRequest::TopLevelUrl() const {
  if (IsMainFrameNavigation()) {
    return request_.url;
  }
  const ProxyContext& context = factory_->context();
  if (context.top_frame_origin) {
    return OriginUrl(*context.top_frame_origin);
  }
  if (request_.trusted_params &&
      request_.trusted_params->isolation_info.top_frame_origin()) {
    return OriginUrl(
        *request_.trusted_params->isolation_info.top_frame_origin());
  }
  return SourceUrl();
}

GURL InProgressRequest::SourceUrl() const {
  if (request_.request_initiator) {
    return OriginUrl(*request_.request_initiator);
  }
  const ProxyContext& context = factory_->context();
  if (!IsNavigation() && !context.initiator.opaque()) {
    return OriginUrl(context.initiator);
  }
  if (request_.trusted_params &&
      request_.trusted_params->isolation_info.top_frame_origin()) {
    return OriginUrl(
        *request_.trusted_params->isolation_info.top_frame_origin());
  }
  return GURL();
}

void InProgressRequest::Start() {
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  if (!config->ready() &&
      base::TimeTicks::Now() - created_ < kMaxWaitForEngine) {
    // 刚启动：等引擎建好（最多几秒）。
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&InProgressRequest::Start, weak_factory_.GetWeakPtr()),
        kEngineRetry);
    return;
  }
  Decide();
}

void InProgressRequest::Decide() {
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  const GURL& url = request_.url;
  if (!url.SchemeIsHTTPOrHTTPS()) {
    PassThrough();
    return;
  }
  if (IsMainFrameNavigation()) {
    Attach();
    DecideMainFrame(*config);
    return;
  }
  const GURL top = TopLevelUrl();
  if (!config->ShouldFilter(top) || !top.SchemeIsHTTPOrHTTPS()) {
    PassThrough();
    return;
  }
  const content_filter::FilterEngine& engine = config->engine();
  const GURL source = SourceUrl();
  const uint32_t flags = factory_->DocumentFlags(engine, top, source);
  content_filter::Request request(url, source, type_);
  request.set_method(request_.method);
  const NetworkMatch match = engine.MatchRequest(request, flags);
  if (HandleBlocked(match)) {
    return;
  }
  if (match.rewritten.is_valid() && match.rewritten != url) {
    Attach();
    SendInternalRedirect(match.rewritten);
    return;
  }
  if (type_ == content_filter::kTypeSubdocument && IsNavigation()) {
    csp_ = engine.CspDirectives(request, flags);
  }
  // 有 $replace 的子资源留在中间，响应到了再改（MaybeFilterBody 按跳转以后的网址再查）。
  if (!IsNavigation() && !engine.ReplaceFilters(request, flags).empty()) {
    Attach();
    StartNetwork();
    return;
  }
  PassThrough();
}

void InProgressRequest::DecideMainFrame(const FilterConfig& config) {
  const GURL& url = request_.url;
  GURL target = url;
  if (config.strip_tracking_parameters()) {
    if (GURL cleaned = content_filter::StripTrackingParameters(target);
        cleaned.is_valid()) {
      target = std::move(cleaned);
    }
  }
  if (config.ShouldFilter(url)) {
    const content_filter::FilterEngine& engine = config.engine();
    const uint32_t flags = engine.DocumentFlags(url, GURL());
    content_filter::Request request(target, target,
                                    content_filter::kTypeDocument);
    request.set_method(request_.method);
    const NetworkMatch match = engine.MatchRequest(request, flags);
    if (match.rewritten.is_valid()) {
      target = match.rewritten;
    }
    content_filter::Request page(url, url, content_filter::kTypeDocument);
    csp_ = engine.CspDirectives(page, flags);
  }
  if (target != url) {
    csp_.clear();
    SendInternalRedirect(target);
    return;
  }
  StartNetwork();
}

bool InProgressRequest::MaybeBlock(const FilterConfig& config,
                                   const GURL& url) {
  const content_filter::FilterEngine& engine = config.engine();
  const GURL top = TopLevelUrl();
  const GURL source = SourceUrl();
  const uint32_t flags = factory_->DocumentFlags(engine, top, source);
  content_filter::Request request(url, source, type_);
  request.set_method(request_.method);
  return HandleBlocked(engine.MatchRequest(request, flags));
}

bool InProgressRequest::HandleBlocked(const NetworkMatch& match) {
  if (!match.blocked()) {
    return false;
  }
  Attach();
  if (match.action == NetworkMatch::Action::kRedirect) {
    if (const content_filter::RedirectResource* resource =
            content_filter::FindRedirectResource(match.redirect)) {
      ReportBlocked();
      ServeResource(*resource);
      return true;
    }
  }
  if (match.blocked()) {
    ReportBlocked();
    Block(type_);
    return true;
  }
  return false;
}

void InProgressRequest::StartNetwork() {
  network::mojom::URLLoaderFactory* target = factory_->target();
  if (!target) {
    OnComplete(network::URLLoaderCompletionStatus(net::ERR_FAILED));
    return;
  }
  client_receiver_.reset();
  target_loader_.reset();
  target->CreateLoaderAndStart(
      target_loader_.BindNewPipeAndPassReceiver(), request_id_, options_,
      request_, client_receiver_.BindNewPipeAndPassRemote(),
      traffic_annotation_);
  client_receiver_.set_disconnect_handler(base::BindOnce(
      &InProgressRequest::OnNetworkDisconnected, base::Unretained(this)));
}

void InProgressRequest::Block(RequestType type) {
  network::URLLoaderCompletionStatus status(net::ERR_BLOCKED_BY_CLIENT);
  status.should_collapse_initiator = ShouldCollapse(type);
  client_->OnComplete(status);
  Finished();
}

void InProgressRequest::ServeResource(
    const content_filter::RedirectResource& resource) {
  auto head = network::mojom::URLResponseHead::New();
  const std::string mime(resource.mime_type);
  head->mime_type = mime;
  const bool text = mime.starts_with("text/") ||
                    mime == "application/javascript" ||
                    mime == "application/json" || mime == "application/xml";
  if (text) {
    head->charset = "utf-8";
  }
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(base::StringPrintf(
          "HTTP/1.1 200 OK\nContent-Type: %s%s\nCache-Control: no-store\n"
          "Access-Control-Allow-Origin: *\n\n",
          mime.c_str(), text ? "; charset=utf-8" : "")));
  head->content_length = static_cast<int64_t>(resource.body.size());
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(std::max<size_t>(resource.body.size(), 1), producer,
                           consumer) != MOJO_RESULT_OK) {
    Block(type_);
    return;
  }
  if (!resource.body.empty()) {
    producer->WriteAllData(base::as_byte_span(resource.body));
  }
  producer.reset();
  client_->OnReceiveResponse(std::move(head), std::move(consumer),
                             std::nullopt);
  network::URLLoaderCompletionStatus status(net::OK);
  status.decoded_body_length = base::ByteSize(resource.body.size());
  status.encoded_body_length = status.decoded_body_length;
  status.encoded_data_length = status.decoded_body_length;
  client_->OnComplete(status);
  Finished();
}

void InProgressRequest::SendInternalRedirect(const GURL& new_url) {
  redirect_info_ = net::RedirectInfo::ComputeRedirectInfo(
      request_.method, request_.url, request_.site_for_cookies,
      request_.update_first_party_url_on_redirect
          ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
          : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
      request_.referrer_policy, request_.referrer.spec(),
      request_.request_initiator, net::HTTP_TEMPORARY_REDIRECT, new_url,
      /*referrer_policy_header=*/std::nullopt,
      /*insecure_scheme_was_upgraded=*/false, /*copy_fragment=*/false,
      /*is_signed_exchange_fallback_redirect=*/false);
  auto head = network::mojom::URLResponseHead::New();
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(base::StringPrintf(
          "HTTP/1.1 %d Internal Redirect\nLocation: %s\n"
          "Non-Authoritative-Reason: Starry Content Blocking\n\n",
          net::HTTP_TEMPORARY_REDIRECT, new_url.spec().c_str())));
  head->encoded_data_length = 0;
  internal_redirect_pending_ = true;
  client_->OnReceiveRedirect(redirect_info_, std::move(head));
}

void InProgressRequest::ReportBlocked() {
  const ProxyContext& context = factory_->context();
  if (IsNavigation()) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&RecordBlockedNavigation, context.frame_tree_node_id));
  } else if (context.frame_id) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&RecordBlockedRequest, context.frame_id));
  }
}

void InProgressRequest::FollowRedirect(
    network::HttpRequestHeadersUpdateParams headers_update_params,
    const std::optional<GURL>& new_url) {
  if (internal_redirect_pending_) {
    // 去参数的跳转：换成新网址，重新开始（同 webRequest 的做法）。
    internal_redirect_pending_ = false;
    request_.url = new_url.value_or(redirect_info_.new_url);
    request_.method = redirect_info_.new_method;
    request_.site_for_cookies = redirect_info_.new_site_for_cookies;
    request_.referrer = GURL(redirect_info_.new_referrer);
    request_.referrer_policy = redirect_info_.new_referrer_policy;
    for (const std::string& name : headers_update_params.removed_headers) {
      request_.headers.RemoveHeader(name);
      request_.cors_exempt_headers.RemoveHeader(name);
    }
    request_.headers.MergeFrom(headers_update_params.modified_headers);
    request_.cors_exempt_headers.MergeFrom(
        headers_update_params.modified_cors_exempt_headers);
    if (IsMainFrameNavigation()) {
      // 去掉参数以后的页面也要加它的 CSP。
      scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
      if (config->ShouldFilter(request_.url)) {
        const content_filter::FilterEngine& engine = config->engine();
        content_filter::Request page(request_.url, request_.url,
                                     content_filter::kTypeDocument);
        csp_ = engine.CspDirectives(
            page, engine.DocumentFlags(request_.url, GURL()));
      }
    }
    StartNetwork();
    return;
  }
  if (!target_loader_) {
    return;
  }
  std::optional<GURL> target_url = new_url;
  if (!target_url && redirect_override_) {
    target_url = redirect_override_;
  }
  redirect_override_.reset();
  target_loader_->FollowRedirect(std::move(headers_update_params), target_url);
}

void InProgressRequest::SetPriority(net::RequestPriority priority,
                                    int32_t intra_priority_value) {
  if (target_loader_) {
    target_loader_->SetPriority(priority, intra_priority_value);
  }
}

void InProgressRequest::OnReceiveEarlyHints(
    network::mojom::EarlyHintsPtr early_hints) {
  client_->OnReceiveEarlyHints(std::move(early_hints));
}

void InProgressRequest::AddCsp(network::mojom::URLResponseHead* head) {
  if (csp_.empty() || !head->headers) {
    return;
  }
  for (const std::string& policy : csp_) {
    head->headers->AddHeader("Content-Security-Policy", policy);
    if (head->parsed_headers) {
      for (auto& parsed : network::ParseContentSecurityPolicies(
               policy, network::mojom::ContentSecurityPolicyType::kEnforce,
               network::mojom::ContentSecurityPolicySource::kHTTP,
               request_.url)) {
        head->parsed_headers->content_security_policy.push_back(
            std::move(parsed));
      }
    }
  }
}

bool InProgressRequest::MaybeFilterBody(
    network::mojom::URLResponseHead* head,
    mojo::ScopedDataPipeConsumerHandle* body) {
  const GURL& url = request_.url;
  if (!head->headers || !url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  const GURL top = TopLevelUrl();
  if (!config->ShouldFilter(top) || !top.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  const content_filter::FilterEngine& engine = config->engine();
  const GURL source = IsMainFrameNavigation() ? url : SourceUrl();
  uint32_t flags = 0;
  if (IsMainFrameNavigation()) {
    flags = engine.DocumentFlags(url, GURL());
  } else {
    flags = factory_->DocumentFlags(engine, top, source);
    if (IsNavigation()) {
      flags |= engine.DocumentFlags(url, top);
    }
  }
  content_filter::ResponseFilters filters;
  if (IsNavigation()) {
    content_filter::HtmlFilterResult html = engine.HtmlFilters(url, flags);
    for (const std::string& name : html.removed_headers) {
      head->headers->RemoveHeader(name);
    }
    filters.html = std::move(html.selectors);
  }
  content_filter::Request request(url, source, type_);
  request.set_method(request_.method);
  filters.replace = engine.ReplaceFilters(request, flags);
  if (filters.empty() || !*body) {
    return false;
  }
  // 同 uBlock Origin：只改成功的（部分内容的不改）、GET 和 POST 的、不是下载的
  // （Content-Disposition 是 inline 的可以）。
  const int status = head->headers->response_code();
  const std::optional<std::string> disposition =
      head->headers->GetNormalizedHeader("Content-Disposition");
  if (status < 200 || status >= 300 || status == net::HTTP_PARTIAL_CONTENT ||
      (request_.method != "GET" && request_.method != "POST") ||
      (disposition &&
       !base::StartsWith(*disposition, "inline",
                         base::CompareCase::INSENSITIVE_ASCII))) {
    return false;
  }
  const bool html = IsNavigation() && head->mime_type == "text/html";
  if (!html) {
    filters.html.clear();
  }
  if (!content_filter::IsFilterableResponseMime(head->mime_type)) {
    filters.replace.clear();
  }
  if (filters.empty()) {
    return false;
  }
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(256 * 1024, producer, consumer) != MOJO_RESULT_OK) {
    return false;
  }
  // 改完才知道多长。改过的脚本的代码缓存按内容的散列认（同 Service Worker 的响应），
  // 不会用上改之前那份的。
  head->content_length = -1;
  head->should_use_source_hash_for_js_code_cache = true;
  body_filter_ = std::make_unique<ResponseBodyFilter>(
      std::move(*body), std::move(producer),
      base::BindOnce(
          // 拿着引擎：规则指着它。
          [](scoped_refptr<const content_filter::FilterEngine> engine,
             content_filter::ResponseFilters filters, GURL url,
             std::string charset, bool html, std::string_view text) {
            return content_filter::FilterResponseBody(text, charset, html, url,
                                                      filters, nullptr);
          },
          config->engine_ref(), std::move(filters), url, head->charset, html),
      base::BindOnce(&InProgressRequest::OnBodyFiltered,
                     base::Unretained(this)));
  *body = std::move(consumer);
  return true;
}

void InProgressRequest::OnBodyFiltered(size_t bytes) {
  body_filtered_ = true;
  filtered_bytes_ = bytes;
  if (pending_status_) {
    network::URLLoaderCompletionStatus status = *pending_status_;
    pending_status_.reset();
    OnComplete(status);
  }
}

void InProgressRequest::OnReceiveResponse(
    network::mojom::URLResponseHeadPtr head,
    mojo::ScopedDataPipeConsumerHandle body,
    std::optional<mojo_base::BigBuffer> cached_metadata) {
  AddCsp(head.get());
  if (MaybeFilterBody(head.get(), &body)) {
    // 网络服务带来的代码缓存是改之前的。
    cached_metadata.reset();
  }
  client_->OnReceiveResponse(std::move(head), std::move(body),
                             std::move(cached_metadata));
}

void InProgressRequest::OnReceiveRedirect(
    const net::RedirectInfo& redirect_info,
    network::mojom::URLResponseHeadPtr head) {
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  net::RedirectInfo info = redirect_info;
  if (IsMainFrameNavigation()) {
    // 跳转的目标也去掉跟踪参数（邮件、短链接的跳转）。只改同源的部分。
    GURL target = info.new_url;
    if (config->strip_tracking_parameters()) {
      if (GURL cleaned = content_filter::StripTrackingParameters(target);
          cleaned.is_valid()) {
        target = std::move(cleaned);
      }
    }
    if (config->ShouldFilter(target)) {
      const content_filter::FilterEngine& engine = config->engine();
      content_filter::Request request(target, target,
                                      content_filter::kTypeDocument);
      const NetworkMatch match =
          engine.MatchRequest(request, engine.DocumentFlags(target, GURL()));
      if (match.rewritten.is_valid()) {
        target = match.rewritten;
      }
      content_filter::Request page(info.new_url, info.new_url,
                                   content_filter::kTypeDocument);
      csp_ = engine.CspDirectives(page,
                                  engine.DocumentFlags(info.new_url, GURL()));
    } else {
      csp_.clear();
    }
    if (target != info.new_url &&
        url::IsSameOriginWith(target, info.new_url)) {
      info.new_url = target;
      redirect_override_ = target;
    }
  } else {
    const GURL top = TopLevelUrl();
    if (config->ShouldFilter(top) && top.SchemeIsHTTPOrHTTPS() &&
        info.new_url.SchemeIsHTTPOrHTTPS()) {
      // 跳转到的网址也要过一遍（跟踪器的跳转链）。
      if (MaybeBlock(*config, info.new_url)) {
        return;
      }
    }
  }
  client_->OnReceiveRedirect(info, std::move(head));
  request_.url = info.new_url;
  request_.site_for_cookies = info.new_site_for_cookies;
  request_.referrer = GURL(info.new_referrer);
}

void InProgressRequest::OnUploadProgress(int64_t current_position,
                                         int64_t total_size,
                                         OnUploadProgressCallback callback) {
  client_->OnUploadProgress(current_position, total_size, std::move(callback));
}

void InProgressRequest::OnTransferSizeUpdated(int32_t transfer_size_diff) {
  client_->OnTransferSizeUpdated(transfer_size_diff);
}

void InProgressRequest::OnComplete(
    const network::URLLoaderCompletionStatus& status) {
  if (completed_) {
    return;
  }
  if (body_filter_) {
    if (!body_filtered_) {
      // 网络那头完了，改好的还没写完。只记第一个：之后网络那头断开时
      // OnNetworkDisconnected 还会补一个 ERR_ABORTED。
      if (!pending_status_) {
        pending_status_ = status;
      }
      return;
    }
    network::URLLoaderCompletionStatus adjusted = status;
    adjusted.decoded_body_length = base::ByteSize(filtered_bytes_);
    client_->OnComplete(adjusted);
    Finished();
    return;
  }
  client_->OnComplete(status);
  Finished();
}

void InProgressRequest::Finished() {
  completed_ = true;
  client_receiver_.reset();
  target_loader_.reset();
  // 消息已经发出去了，晚一步删掉自己（可能在 mojo 的回调里）。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<InProgressRequest> self) {
            if (self) {
              self->factory_->RemoveRequest(self.get());
            }
          },
          weak_factory_.GetWeakPtr()));
}

void InProgressRequest::OnClientDisconnected() {
  // 网页不要了（取消、关标签）：网络那头一起断掉。
  factory_->RemoveRequest(this);
}

void InProgressRequest::OnNetworkDisconnected() {
  if (!completed_) {
    OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
  }
}

}  // namespace

void MaybeProxyURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    int render_process_id,
    content::ContentBrowserClient::URLLoaderFactoryType type,
    const url::Origin& request_initiator,
    const net::IsolationInfo& isolation_info,
    network::URLLoaderFactoryBuilder& factory_builder) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  switch (type) {
    case FactoryType::kNavigation:
    case FactoryType::kDocumentSubResource:
    case FactoryType::kWorkerMainResource:
    case FactoryType::kWorkerSubResource:
    case FactoryType::kServiceWorkerScript:
    case FactoryType::kServiceWorkerSubResource:
    case FactoryType::kPrefetch:
    case FactoryType::kEarlyHints:
      break;
    case FactoryType::kDownload:
    case FactoryType::kDevTools:
      return;
  }
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  const bool navigation = type == FactoryType::kNavigation;
  // 都关着时不插代理；只开着去跟踪参数时只管导航。设置更改后，已打开的网页需重新
  // 载入才生效。
  if (!config->enabled() &&
      !(navigation && config->strip_tracking_parameters())) {
    return;
  }
  // 浏览器自己的页面、扩展的页面发的请求不管。
  if (!navigation && !request_initiator.opaque() &&
      request_initiator.scheme() != url::kHttpScheme &&
      request_initiator.scheme() != url::kHttpsScheme) {
    return;
  }
  ProxyContext context;
  context.type = type;
  if (frame) {
    context.frame_id = frame->GetGlobalId();
    context.frame_tree_node_id = frame->GetFrameTreeNodeId();
  }
  context.initiator = request_initiator;
  context.top_frame_origin = isolation_info.top_frame_origin();
  auto [receiver, target] = factory_builder.Append();
  ProxyTaskRunner()->PostTask(
      FROM_HERE, base::BindOnce(&ProxyFactory::Create, std::move(context),
                                std::move(receiver), std::move(target)));
}

}  // namespace starry::content_blocking
