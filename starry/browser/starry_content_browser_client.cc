// Copyright 2026 The Starry Browser Authors

#include "starry/browser/starry_content_browser_client.h"

#include <string>
#include <utility>

#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "chrome/browser/chrome_browser_main.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/browser_url_handler.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "net/http/http_request_headers.h"
#include "services/device/public/mojom/time_zone_monitor.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "services/service_manager/public/cpp/binder_registry.h"
#include "starry/browser/content_blocking/content_blocking_hooks.h"
#include "starry/browser/content_blocking/content_filter_proxy.h"
#include "starry/browser/content_blocking/content_filter_tab_helper.h"
#include "starry/browser/new_tab/new_tab.h"
#include "starry/browser/privacy/fingerprint_protection.h"
#include "starry/browser/sessions/session_titles.h"
#include "starry/browser/starry_browser_main_extra_parts.h"
#include "starry/browser/ui/media/media_pip.h"
#include "starry/browser/ui/tabs/pinned_peek.h"
#include "starry/browser/web_time_zone.h"
#include "starry/common/fingerprint_protection.h"
#include "starry/common/internal_urls.h"
#include "starry/common/web_time_zone.h"
#include "starry/components/privacy/mojom/fingerprint_protection.mojom.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace starry {

namespace {

// Chrome 默认开着、Starry 不要的功能。
constexpr const char* kDisabledFeatures[] = {
    // Gemini in Chrome：菜单栏图标、后台常驻、网页智能体。
    // 服务入口也由 starry/common/google_services.h 固定关闭。
    "Glic",
    "GlicActor",
    // Chromium 157 的桌面依赖不支持关闭 enable_lens_desktop，禁用它的搜图入口。
    "LensStandalone",
    "LensOverlay",
    "Compose",
    // 网页端的 Google 本地翻译 API、语言包下载、实时字幕翻译。
    "TranslationAPI",
    "TranslationAPIForWorkers",
    "AutoDownloadTranslateLanguagePacks",
    "LiveCaptionOnDeviceTranslation",
    "TranslateRankerQuery",
    "TranslateRankerEnforcement",
    // Chrome 的模型执行、后台模型下载和模型质量上报。Starry 助手走自己的后端。
    "OptimizationGuideModelExecution",
    "OnDeviceModelBackgroundDownload",
    "ModelQualityLogging",
    "AiSettingsPageForceAvailable",
};

// Chrome 在这个平台上默认关着、Starry 要用的功能。
constexpr const char* kEnabledFeatures[] = {
    // 后台标签的缩略图（ThumbnailTabHelper）：标签的预览卡片要用，macOS 上 Chrome 默认关着。
    "TabHoverCardImages",
};

// 加到 --disable-features、--enable-features 里（和命令行上已有的合并；AppendSwitch
// 会覆盖同名开关）。
void AppendFeatures(base::CommandLine& command_line,
                    const char* switch_name,
                    base::span<const char* const> features) {
  std::string value = command_line.GetSwitchValueASCII(switch_name);
  for (const char* feature : features) {
    if (!value.empty()) {
      value += ",";
    }
    value += feature;
  }
  command_line.AppendSwitchASCII(switch_name, value);
}

void DisableUnwantedFeatures(base::CommandLine& command_line) {
  AppendFeatures(command_line, switches::kDisableFeatures, kDisabledFeatures);
  AppendFeatures(command_line, switches::kEnableFeatures, kEnabledFeatures);
}

// 扩展页重定向到 Starry 设置页的「扩展」分类。Chrome 内部打开的扩展管理、
// 扩展菜单和应用店相关链接也会落到这里。
// chrome://extensions/?id=<ID> 展开那个扩展的详情，/shortcuts 展开有快捷键的。
bool RewriteExtensionsURL(GURL* url, content::BrowserContext* browser_context) {
  if (!url->SchemeIs(content::kChromeUIScheme) ||
      url->host() != chrome::kChromeUIExtensionsHost) {
    return false;
  }
  std::string query(url->query());
  if (url->path() == "/shortcuts") {
    query = query.empty() ? "shortcuts" : query + "&shortcuts";
  }
  *url = GURL(base::StrCat({content::kChromeUIScheme,
                            url::kStandardSchemeSeparator, "starry-settings",
                            "/extensions", query.empty() ? "" : "?", query}));
  return true;
}

// 导入浏览器数据用 Starry 设置页的（能从 Chromium 系的浏览器导入，见
// starry/browser/importer/import_service.h）。Chrome 自己打开的（macOS 菜单栏的「导入书签和
// 设置…」、chrome://settings/importData）也到这里。
bool RewriteImportDataURL(GURL* url, content::BrowserContext* browser_context) {
  if (!url->SchemeIs(content::kChromeUIScheme) ||
      url->host() != chrome::kChromeUISettingsHost ||
      url->path() != base::StrCat({"/", chrome::kImportDataSubPage})) {
    return false;
  }
  *url = GURL(
      base::StrCat({content::kChromeUIScheme, url::kStandardSchemeSeparator,
                    "starry-settings", "/importData"}));
  return true;
}

// Translate before Chrome's handlers, then let its normal WebUI rewrites run.
bool RewritePublicURL(GURL* url, content::BrowserContext* browser_context) {
  *url = ToChromeURL(*url);
  return false;
}

// Catch WebUI URLs not handled above so same-document navigation also updates
// their public address (settings routes use history.replaceState).
bool HandleInternalURL(GURL* url, content::BrowserContext* browser_context) {
  if (!IsInternalURL(*url)) {
    return false;
  }
  *url = ToChromeURL(*url);
  return true;
}

bool ReverseInternalURL(GURL* url, content::BrowserContext* browser_context) {
  if (!IsInternalURL(*url)) {
    return false;
  }
  *url = ToStarryURL(*url);
  return true;
}

}  // namespace

StarryContentBrowserClient::StarryContentBrowserClient() {
  RegisterFingerprintClientHints();
  // ChromeMainDelegate 在建 FeatureList 之前建浏览器进程的 ContentBrowserClient
  // （ContentMainRunnerImpl::Initialize → PostEarlyInitialization），
  // 所以这里改命令行还来得及。
  if (base::FeatureList::GetInstance()) {
    LOG(ERROR) << "[starry] FeatureList already exists; "
                  "default --disable-features not applied";
    return;
  }
  DisableUnwantedFeatures(*base::CommandLine::ForCurrentProcess());
}

StarryContentBrowserClient::~StarryContentBrowserClient() = default;

void StarryContentBrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int child_process_id) {
  ChromeContentBrowserClient::AppendExtraCommandLineSwitches(command_line,
                                                             child_process_id);
  if (command_line->GetSwitchValueASCII(switches::kProcessType) !=
      switches::kRendererProcess) {
    return;
  }
  command_line->AppendSwitch(kFingerprintServiceSwitch);
  if (auto* process = content::RenderProcessHost::FromID(child_process_id)) {
    const std::string& zone = EffectiveWebTimeZone(
        Profile::FromBrowserContext(process->GetBrowserContext()));
    if (!zone.empty()) {
      command_line->AppendSwitchASCII(kWebTimeZoneSwitch, zone);
    }
  }
}

void StarryContentBrowserClient::ExposeInterfacesToRenderer(
    service_manager::BinderRegistry* registry,
    blink::AssociatedInterfaceRegistry* associated_registry,
    content::RenderProcessHost* render_process_host) {
  ChromeContentBrowserClient::ExposeInterfacesToRenderer(
      registry, associated_registry, render_process_host);
  registry->AddInterface(base::BindRepeating(&BindFingerprintProtection,
                                             render_process_host->GetID()),
                         content::GetUIThreadTaskRunner({}));
  const std::string& zone = EffectiveWebTimeZone(
      Profile::FromBrowserContext(render_process_host->GetBrowserContext()));
  if (!zone.empty()) {
    registry->AddInterface(base::BindRepeating(&BindWebTimeZoneMonitor, zone),
                           content::GetUIThreadTaskRunner({}));
  }
}

void StarryContentBrowserClient::WillCreateURLLoaderFactory(
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
    bool is_for_network_service) {
  // 先插 Starry 的（离网页进程最近）：拦下的请求不再经过扩展的 webRequest。
  content_blocking::MaybeProxyURLLoaderFactory(
      browser_context, frame, render_process_id, type, request_initiator,
      isolation_info, factory_builder);
  ChromeContentBrowserClient::WillCreateURLLoaderFactory(
      browser_context, frame, render_process_id, type, request_initiator,
      isolation_info, std::move(navigation_id), ukm_source_id, factory_builder,
      header_client, bypass_redirect_checks, disable_secure_dns,
      factory_override, std::move(navigation_response_task_runner),
      is_for_network_service);
}

void StarryContentBrowserClient::CreateThrottlesForNavigation(
    content::NavigationThrottleRegistry& registry) {
  ChromeContentBrowserClient::CreateThrottlesForNavigation(registry);
  PinnedPeek::MaybeAddThrottle(registry);
  // 元素隐藏、拦截数挂在 WebContents 上：第一次导航时建好（赶得上这一次的
  // ReadyToCommitNavigation）。
  content_blocking::ContentFilterTabHelper::MaybeCreate(
      registry.GetNavigationHandle().GetWebContents());
  // 标题写进会话：恢复后休眠的标签显示得出标题。
  SaveTitlesInSession(registry.GetNavigationHandle().GetWebContents());
}

bool StarryContentBrowserClient::CanCreateWindow(
    content::RenderFrameHost* opener,
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
    bool* no_javascript_access) {
  if (content_blocking::ShouldBlockPopup(opener, opener_url,
                                         opener_top_level_frame_url,
                                         target_url)) {
    return false;
  }
  return ChromeContentBrowserClient::CanCreateWindow(
      opener, opener_url, opener_top_level_frame_url, source_origin,
      container_type, target_url, referrer, frame_name, disposition, features,
      user_gesture, opener_suppressed, no_javascript_access);
}

bool StarryContentBrowserClient::WillInterceptWebSocket(
    content::RenderFrameHost* frame) {
  return content_blocking::WantsWebSocket(frame) ||
         ChromeContentBrowserClient::WillInterceptWebSocket(frame);
}

void StarryContentBrowserClient::CreateWebSocket(
    content::RenderFrameHost* frame,
    WebSocketFactory factory,
    const GURL& url,
    const net::SiteForCookies& site_for_cookies,
    const std::optional<std::string>& user_agent,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
        handshake_client,
    content::ContentBrowserClient::WebSocketOptions options) {
  if (content_blocking::ShouldBlockWebSocket(frame, url)) {
    // 不连：网页的 WebSocket 收到连接失败。
    return;
  }
  if (ChromeContentBrowserClient::WillInterceptWebSocket(frame)) {
    ChromeContentBrowserClient::CreateWebSocket(
        frame, std::move(factory), url, site_for_cookies, user_agent,
        std::move(handshake_client), std::move(options));
    return;
  }
  // 同 content 自己连的写法（WebSocketConnectorImpl::Connect）。
  std::vector<network::mojom::HttpHeaderPtr> headers;
  if (user_agent) {
    headers.push_back(network::mojom::HttpHeader::New(
        net::HttpRequestHeaders::kUserAgent, *user_agent));
  }
  std::move(factory).Run(url, std::move(headers), std::move(handshake_client),
                         mojo::NullRemote(), std::move(options.header_client));
}

std::unique_ptr<content::BrowserMainParts>
StarryContentBrowserClient::CreateBrowserMainParts(bool is_integration_test) {
  std::unique_ptr<content::BrowserMainParts> parts =
      ChromeContentBrowserClient::CreateBrowserMainParts(is_integration_test);
  // ChromeContentBrowserClient 总是返回 ChromeBrowserMainParts（按平台的子类）。
  static_cast<ChromeBrowserMainParts*>(parts.get())
      ->AddParts(std::make_unique<StarryBrowserMainExtraParts>());
  return parts;
}

void StarryContentBrowserClient::BrowserURLHandlerCreated(
    content::BrowserURLHandler* handler) {
  // 加在 Chrome 的处理之前（按加入顺序匹配），要先于 Chrome 把 chrome://newtab
  // 改写成 chrome://new-tab-page。
  handler->AddHandlerPair(&RewritePublicURL,
                          content::BrowserURLHandler::null_handler());
  handler->AddHandlerPair(&new_tab::RewriteURL, &new_tab::ReverseURL);
  handler->AddHandlerPair(&RewriteExtensionsURL, &ReverseInternalURL);
  handler->AddHandlerPair(&RewriteImportDataURL, &ReverseInternalURL);
  ChromeContentBrowserClient::BrowserURLHandlerCreated(handler);
  handler->AddHandlerPair(&HandleInternalURL, &ReverseInternalURL);
}

std::unique_ptr<content::VideoOverlayWindow>
StarryContentBrowserClient::CreateWindowForVideoPictureInPicture(
    content::VideoPictureInPictureWindowController* controller) {
  return MediaPip::Create(controller);
}

}  // namespace starry
