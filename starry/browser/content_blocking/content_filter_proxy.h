// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_FILTER_PROXY_H_
#define STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_FILTER_PROXY_H_

#include "content/public/browser/content_browser_client.h"

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace net {
class IsolationInfo;
}

namespace network {
class URLLoaderFactoryBuilder;
}

namespace url {
class Origin;
}

namespace starry::content_blocking {

// 网页的请求先经过 Starry 的代理：
// StarryContentBrowserClient::WillCreateURLLoaderFactory 调用它，在网页进程和网络服务
// 之间插一层 URLLoaderFactory（类似 Chrome 扩展的 webRequest）。代理在自己
// 的一个线程序列上，不经过界面线程；每个请求问一次过滤引擎（几微秒）：
// - 拦下：直接回 ERR_BLOCKED_BY_CLIENT，图片、iframe 收起来不占位置；
// - 替身资源（$redirect）：不发请求，代理自己回一个空脚本、透明图片这类；
// - 去参数（$removeparam、跟踪参数）：回一个内部的 307 跳转，去掉参数后再请求；
// - 页面的响应加上 $csp 的策略；服务器的跳转再检查一遍目标。
// 顶层页面不拦（只去参数）。拦下的请求记到标签上（content_filter_tab_helper.h）。
// 拦截和去跟踪参数都关着时不插代理（没有多出来的一跳）。
void MaybeProxyURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    int render_process_id,
    content::ContentBrowserClient::URLLoaderFactoryType type,
    const url::Origin& request_initiator,
    const net::IsolationInfo& isolation_info,
    network::URLLoaderFactoryBuilder& factory_builder);

}  // namespace starry::content_blocking

#endif  // STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_FILTER_PROXY_H_
