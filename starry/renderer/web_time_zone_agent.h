// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_WEB_TIME_ZONE_AGENT_H_
#define STARRY_RENDERER_WEB_TIME_ZONE_AGENT_H_

#include <string>

#include "content/public/renderer/render_thread_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "starry/components/privacy/mojom/web_time_zone.mojom.h"

namespace starry {

// 收浏览器推来的网页时区（starry/browser/privacy/process_time_zone.h）。
// channel-associated：在主线程上按通道的顺序处理，先于之后提交的导航。
class WebTimeZoneAgent : public content::RenderThreadObserver,
                         public mojom::WebTimeZone {
 public:
  WebTimeZoneAgent();
  WebTimeZoneAgent(const WebTimeZoneAgent&) = delete;
  WebTimeZoneAgent& operator=(const WebTimeZoneAgent&) = delete;
  ~WebTimeZoneAgent() override;

  // content::RenderThreadObserver:
  void RegisterMojoInterfaces(
      blink::AssociatedInterfaceRegistry* associated_interfaces) override;
  void UnregisterMojoInterfaces(
      blink::AssociatedInterfaceRegistry* associated_interfaces) override;

  // mojom::WebTimeZone:
  void SetTimeZone(const std::string& id) override;

 private:
  void Bind(mojo::PendingAssociatedReceiver<mojom::WebTimeZone> receiver);

  // 浏览器每次推送新建一个端点。
  mojo::AssociatedReceiverSet<mojom::WebTimeZone> receivers_;
};

}  // namespace starry

#endif  // STARRY_RENDERER_WEB_TIME_ZONE_AGENT_H_
