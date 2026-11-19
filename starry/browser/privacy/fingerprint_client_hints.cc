// Copyright 2026 The Starry Browser Authors

#include "starry/browser/privacy/fingerprint_client_hints.h"

#include <string>

#include "base/strings/string_number_conversions.h"
#include "content/public/browser/browser_thread.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/cpp/client_hints.h"
#include "services/network/public/mojom/web_client_hints_types.mojom-shared.h"
#include "starry/common/fingerprint_values.h"
#include "third_party/blink/public/common/device_memory/approximated_device_memory.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "url/origin.h"

namespace starry {

namespace {

FingerprintClientHintsCheck g_check = nullptr;

bool Protects(content::BrowserContext* context, const url::Origin& top_origin) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return g_check && context && g_check(context, top_origin);
}

}  // namespace

void SetFingerprintClientHintsCheck(FingerprintClientHintsCheck check) {
  g_check = check;
}

void ProtectClientHintsUserAgent(content::BrowserContext* context,
                                 const url::Origin& top_origin,
                                 blink::UserAgentMetadata& metadata) {
  if (Protects(context, top_origin)) {
    ReduceUserAgentMetadata(metadata);
  }
}

void ProtectClientHintsHeaders(content::BrowserContext* context,
                               const url::Origin& top_origin,
                               net::HttpRequestHeaders& headers) {
  if (!Protects(context, top_origin)) {
    return;
  }
  using network::mojom::WebClientHintsType;
  const auto replace = [&](WebClientHintsType type, const std::string& value) {
    const std::string& name = network::GetClientHintToNameMap().at(type);
    if (headers.HasHeader(name)) {
      headers.SetHeader(name, value);
    }
  };
  replace(WebClientHintsType::kRtt_DEPRECATED,
          base::NumberToString(kProtectedRttMs));
  replace(WebClientHintsType::kDownlink_DEPRECATED,
          base::NumberToString(kProtectedDownlinkMbps));
  replace(WebClientHintsType::kEct_DEPRECATED, kProtectedEffectiveType);

  blink::ApproximatedDeviceMemory::Initialize();
  const std::string memory = base::NumberToString(ProtectedDeviceMemory(
      blink::ApproximatedDeviceMemory::GetApproximatedDeviceMemory()));
  replace(WebClientHintsType::kDeviceMemory_DEPRECATED, memory);
  replace(WebClientHintsType::kDeviceMemory, memory);

  // Chromium sends the primary display's scale times the site's zoom.
  double scale = 1;
  if (display::Screen* screen = display::Screen::Get()) {
    scale = screen->GetPrimaryDisplay().device_scale_factor();
  }
  const std::string dpr =
      base::NumberToString(ProtectedDevicePixelRatio(scale));
  replace(WebClientHintsType::kDpr_DEPRECATED, dpr);
  replace(WebClientHintsType::kDpr, dpr);
}

}  // namespace starry
