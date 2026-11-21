// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_STARRY_CONTENT_BROWSER_CLIENT_H_
#define STARRY_BROWSER_STARRY_CONTENT_BROWSER_CLIENT_H_

#include <memory>

#include "chrome/browser/chrome_content_browser_client.h"

namespace starry {

// Chrome 的 ContentBrowserClient 加上 Starry 的钩子。
// patches/chrome/app/chrome_main_delegate.cc.patch 让 Chrome 用它代替
// ChromeContentBrowserClient。
class StarryContentBrowserClient : public ChromeContentBrowserClient {
 public:
  StarryContentBrowserClient();
  StarryContentBrowserClient(const StarryContentBrowserClient&) = delete;
  StarryContentBrowserClient& operator=(const StarryContentBrowserClient&) =
      delete;
  ~StarryContentBrowserClient() override;

  // ChromeContentBrowserClient:
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;
  void BrowserURLHandlerCreated(content::BrowserURLHandler* handler) override;
  void AppendExtraCommandLineSwitches(base::CommandLine* command_line,
                                      int child_process_id) override;
  void SiteInstanceGotProcessAndSite(
      content::SiteInstance* site_instance) override;
  bool ShouldUseSpareRenderProcessHost(
      content::BrowserContext* browser_context,
      const GURL& site_url,
      std::optional<SpareProcessRefusedByEmbedderReason>& refused_reason)
      override;
  void ExposeInterfacesToRenderer(
      service_manager::BinderRegistry* registry,
      blink::AssociatedInterfaceRegistry* associated_registry,
      content::RenderProcessHost* render_process_host) override;

  // 广告与跟踪拦截：网页的请求经过 Starry 的代理，
  // 导航提交前送元素隐藏，弹出窗口和 WebSocket 也过一遍规则。
  void WillCreateURLLoaderFactory(
      content::BrowserContext* browser_context,
      content::RenderFrameHost* frame,
      int render_process_id,
      URLLoaderFactoryType type,
      const url::Origin& request_initiator,
      const net::IsolationInfo& isolation_info,
      std::optional<int64_t> navigation_id,
      ukm::SourceIdObj ukm_source_id,
      network::URLLoaderFactoryBuilder& factory_builder,
      mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
          header_client,
      bool* bypass_redirect_checks,
      bool* disable_secure_dns,
      network::mojom::URLLoaderFactoryOverridePtr* factory_override,
      scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner,
      bool is_for_network_service) override;
  void CreateThrottlesForNavigation(
      content::NavigationThrottleRegistry& registry) override;
  bool CanCreateWindow(content::RenderFrameHost* opener,
                       const GURL& opener_url,
                       const GURL& opener_top_level_frame_url,
                       const url::Origin& source_origin,
                       content::mojom::WindowContainerType container_type,
                       const GURL& target_url,
                       const content::Referrer& referrer,
                       const std::string& frame_name,
                       WindowOpenDisposition disposition,
                       const blink::mojom::WindowFeatures& features,
                       bool user_gesture,
                       bool opener_suppressed,
                       bool* no_javascript_access) override;
  bool WillInterceptWebSocket(content::RenderFrameHost* frame) override;
  void CreateWebSocket(
      content::RenderFrameHost* frame,
      WebSocketFactory factory,
      const GURL& url,
      const net::SiteForCookies& site_for_cookies,
      const std::optional<std::string>& user_agent,
      mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
          handshake_client,
      content::ContentBrowserClient::WebSocketOptions options) override;

  // 视频画中画换成 Starry
  // 的（ui/media/media_pip.h）：放在媒体岛上，或者桌面的小窗。
  std::unique_ptr<content::VideoOverlayWindow>
  CreateWindowForVideoPictureInPicture(
      content::VideoPictureInPictureWindowController* controller) override;
};

}  // namespace starry

#endif  // STARRY_BROWSER_STARRY_CONTENT_BROWSER_CLIENT_H_
