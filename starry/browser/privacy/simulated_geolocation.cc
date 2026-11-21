// Copyright 2026 The Starry Browser Authors

#include "starry/browser/privacy/simulated_geolocation.h"

#include <set>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/device/public/cpp/geolocation/geoposition.h"
#include "services/device/public/mojom/geolocation.mojom.h"
#include "services/device/public/mojom/geoposition.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace starry {

namespace {

SimulatedGeolocationLookup g_lookup = nullptr;

std::optional<SimulatedPosition> Lookup(content::BrowserContext* context,
                                        const GURL& top_url) {
  if (!g_lookup || !context || !top_url.SchemeIsHTTPOrHTTPS()) {
    return std::nullopt;
  }
  return g_lookup(context, top_url);
}

device::mojom::GeopositionResultPtr Unavailable() {
  return device::mojom::GeopositionResult::NewError(
      device::mojom::GeopositionError::New(
          device::mojom::GeopositionErrorCode::kPositionUnavailable,
          device::mojom::kGeoPositionUnavailableErrorMessage, ""));
}

// 同 Mac 的 Wi-Fi 定位：只有经纬度和精度，没有高度、方向和速度。
device::mojom::GeopositionResultPtr ResultFor(
    const SimulatedPosition& position) {
  if (!position.available) {
    return Unavailable();
  }
  auto geoposition = device::mojom::Geoposition::New();
  geoposition->latitude = position.latitude;
  geoposition->longitude = position.longitude;
  geoposition->accuracy = position.accuracy;
  geoposition->timestamp = base::Time::Now();
  geoposition->is_precise = true;
  if (!device::ValidateGeoposition(*geoposition)) {
    return Unavailable();
  }
  return device::mojom::GeopositionResult::NewPosition(std::move(geoposition));
}

bool SamePosition(const SimulatedPosition& a, const SimulatedPosition& b) {
  return a.available == b.available && a.latitude == b.latitude &&
         a.longitude == b.longitude && a.accuracy == b.accuracy;
}

class SimulatedGeolocation;

std::set<SimulatedGeolocation*>& Live() {
  static base::NoDestructor<std::set<SimulatedGeolocation*>> live;
  return *live;
}

// 一个 Geolocation 连接（Blink 的一次 getCurrentPosition / watchPosition 共用）。
// 语义同 GeolocationImpl 的位置覆盖：第一次 QueryNextPosition 立刻回答，之后等到
// 位置变了才回答。连接断开时删除自己。
class SimulatedGeolocation : public device::mojom::Geolocation {
 public:
  SimulatedGeolocation(
      content::GlobalRenderFrameHostId frame,
      GURL top_url,
      const SimulatedPosition& position,
      mojo::PendingReceiver<device::mojom::Geolocation> receiver)
      : frame_(frame),
        top_url_(std::move(top_url)),
        position_(position),
        receiver_(this, std::move(receiver)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&SimulatedGeolocation::Close, base::Unretained(this)));
    Live().insert(this);
  }

  SimulatedGeolocation(const SimulatedGeolocation&) = delete;
  SimulatedGeolocation& operator=(const SimulatedGeolocation&) = delete;

  ~SimulatedGeolocation() override { Live().erase(this); }

  // 重新读规则。可能删除自己。
  void Update() {
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_);
    const std::optional<SimulatedPosition> position =
        frame ? Lookup(frame->GetBrowserContext(), top_url_) : std::nullopt;
    if (!position) {
      // 不再模拟：报位置不可用并断开（Blink 对 watchPosition 也报错并停止），
      // 不把这条连接改接到真实位置。
      if (pending_) {
        std::move(pending_).Run(Unavailable());
      }
      Close();
      return;
    }
    if (SamePosition(*position, position_)) {
      return;
    }
    position_ = *position;
    reported_ = false;
    if (pending_) {
      Report();
    }
  }

  // device::mojom::Geolocation:
  void SetHighAccuracyHint(bool high_accuracy) override {}

  void QueryNextPosition(QueryNextPositionCallback callback) override {
    if (pending_) {
      // 同 GeolocationImpl：重叠的请求当作连接错误。
      Close();
      return;
    }
    pending_ = std::move(callback);
    if (!reported_) {
      Report();
    }
  }

  void QueryCachedPosition(QueryCachedPositionCallback callback) override {
    std::move(callback).Run(ResultFor(position_));
  }

 private:
  void Report() {
    reported_ = true;
    std::move(pending_).Run(ResultFor(position_));
  }

  void Close() { delete this; }

  const content::GlobalRenderFrameHostId frame_;
  const GURL top_url_;
  SimulatedPosition position_;
  bool reported_ = false;
  QueryNextPositionCallback pending_;
  // 最后声明、最先析构：等它断开以后才丢掉没回答的回调。
  mojo::Receiver<device::mojom::Geolocation> receiver_;
};

}  // namespace

void SetSimulatedGeolocationLookup(SimulatedGeolocationLookup lookup) {
  g_lookup = lookup;
}

void UpdateSimulatedGeolocation() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const std::vector<SimulatedGeolocation*> live(Live().begin(), Live().end());
  for (SimulatedGeolocation* geolocation : live) {
    // 前面的 Update 不会删掉别的连接，这里只是保险。
    if (Live().contains(geolocation)) {
      geolocation->Update();
    }
  }
}

bool IsGeolocationSimulated(content::BrowserContext* context,
                            const GURL& embedding_origin) {
  return Lookup(context, embedding_origin).has_value();
}

bool MaybeBindSimulatedGeolocation(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<device::mojom::Geolocation>& receiver) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  GURL top_url =
      frame->GetOutermostMainFrame()->GetLastCommittedOrigin().GetURL();
  const std::optional<SimulatedPosition> position =
      Lookup(frame->GetBrowserContext(), top_url);
  if (!position) {
    return false;
  }
  // 连接断开时删除自己（Close）。
  new SimulatedGeolocation(frame->GetGlobalId(), std::move(top_url), *position,
                           std::move(receiver));
  return true;
}

}  // namespace starry
