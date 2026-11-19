// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_BLOCKING_HOOKS_H_
#define STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_BLOCKING_HOOKS_H_

class GURL;

namespace content {
class RenderFrameHost;
}

// StarryContentBrowserClient 里拦截用到的几处（网络请求见 content_filter_proxy.h）。
namespace starry::content_blocking {

// 网页要打开的新窗口（window.open、target=_blank）被 `$popup` 规则拦下：返回 true，
// 窗口不开（ContentBrowserClient::CanCreateWindow）。
bool ShouldBlockPopup(content::RenderFrameHost* opener,
                      const GURL& opener_url,
                      const GURL& opener_top_level_url,
                      const GURL& target_url);

// 拦截开着时 WebSocket 也要过一遍（ContentBrowserClient::WillInterceptWebSocket）。
bool WantsWebSocket(content::RenderFrameHost* frame);
// 这个 WebSocket 被拦下（`$websocket`、`||ws.example.com^`）：返回 true。
bool ShouldBlockWebSocket(content::RenderFrameHost* frame, const GURL& url);

}  // namespace starry::content_blocking

#endif  // STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_BLOCKING_HOOKS_H_
