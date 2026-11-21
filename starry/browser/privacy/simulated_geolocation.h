// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_PRIVACY_SIMULATED_GEOLOCATION_H_
#define STARRY_BROWSER_PRIVACY_SIMULATED_GEOLOCATION_H_

#include <optional>

#include "content/common/content_export.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/device/public/mojom/geolocation.mojom-forward.h"

class GURL;

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace starry {

// 编入 //content/browser，由浏览器客户端提供按用户资料查询的回调。
struct SimulatedPosition {
  double latitude = 0;
  double longitude = 0;
  double accuracy = 0;
  // 规则说要模拟、但没有有效位置（例如全局模拟还没选点）：报位置不可用，
  // 不回退到真实位置。
  bool available = false;
};

// 返回 nullopt：这个顶层网站用真实位置（走 Chromium 原来的流程）。
using SimulatedGeolocationLookup =
    std::optional<SimulatedPosition> (*)(content::BrowserContext* context,
                                         const GURL& top_url);
CONTENT_EXPORT void SetSimulatedGeolocationLookup(
    SimulatedGeolocationLookup lookup);

// 规则改了：正在读位置的网页拿到新位置（watchPosition 立刻回调）；不再模拟的
// 连接报位置不可用并断开，网页要重新请求，不会悄悄换成真实位置。
CONTENT_EXPORT void UpdateSimulatedGeolocation();

// 权限：顶层网站有模拟规则就算允许，也跳过系统的定位权限（GeolocationPermission-
// Context(System)::GetPermissionStatusInternal）。embedding_origin 是顶层的源。
CONTENT_EXPORT bool IsGeolocationSimulated(content::BrowserContext* context,
                                           const GURL& embedding_origin);

// GeolocationServiceImpl 在权限通过后调用。按 outermost main frame 的源判断，
// iframe 跟着顶层网站走（同 Chromium 的权限委托）。接管了返回 true，receiver 被取走。
bool MaybeBindSimulatedGeolocation(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<device::mojom::Geolocation>& receiver);

}  // namespace starry

#endif  // STARRY_BROWSER_PRIVACY_SIMULATED_GEOLOCATION_H_
