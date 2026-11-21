// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/web_time_zone_agent.h"

#include "base/functional/bind.h"
#include "starry/renderer/web_time_zone.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"

namespace starry {

WebTimeZoneAgent::WebTimeZoneAgent() = default;

WebTimeZoneAgent::~WebTimeZoneAgent() = default;

void WebTimeZoneAgent::RegisterMojoInterfaces(
    blink::AssociatedInterfaceRegistry* associated_interfaces) {
  associated_interfaces->AddInterface<mojom::WebTimeZone>(
      base::BindRepeating(&WebTimeZoneAgent::Bind, base::Unretained(this)));
}

void WebTimeZoneAgent::UnregisterMojoInterfaces(
    blink::AssociatedInterfaceRegistry* associated_interfaces) {
  associated_interfaces->RemoveInterface(mojom::WebTimeZone::Name_);
}

void WebTimeZoneAgent::SetTimeZone(const std::string& id) {
  // 浏览器发完就丢掉端点，这边的接收端随断开自动从集合里去掉。
  SetWebTimeZone(id);
}

void WebTimeZoneAgent::Bind(
    mojo::PendingAssociatedReceiver<mojom::WebTimeZone> receiver) {
  receivers_.Add(this, std::move(receiver));
}

}  // namespace starry
