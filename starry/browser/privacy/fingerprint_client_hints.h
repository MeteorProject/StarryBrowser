// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_PRIVACY_FINGERPRINT_CLIENT_HINTS_H_
#define STARRY_BROWSER_PRIVACY_FINGERPRINT_CLIENT_HINTS_H_

#include "content/common/content_export.h"

namespace blink {
struct UserAgentMetadata;
}

namespace content {
class BrowserContext;
}

namespace net {
class HttpRequestHeaders;
}

namespace url {
class Origin;
}

namespace starry {

// Compiled into //content/browser, which adds client hints to navigation
// requests (Blink adds them to subresources). The policy itself needs profiles
// and preferences above //content, so the Starry browser client installs it.
using FingerprintClientHintsCheck = bool (*)(content::BrowserContext* context,
                                             const url::Origin& top_origin);
CONTENT_EXPORT void SetFingerprintClientHintsCheck(
    FingerprintClientHintsCheck check);

// Called from content/browser/client_hints/client_hints.cc with the outermost
// main frame origin (for a main frame navigation: its target). The values are
// the ones Blink reports to the page's JavaScript.
void ProtectClientHintsUserAgent(content::BrowserContext* context,
                                 const url::Origin& top_origin,
                                 blink::UserAgentMetadata& metadata);
// Replaces RTT / Downlink / ECT / Device-Memory / DPR headers that were added.
void ProtectClientHintsHeaders(content::BrowserContext* context,
                               const url::Origin& top_origin,
                               net::HttpRequestHeaders& headers);

}  // namespace starry

#endif  // STARRY_BROWSER_PRIVACY_FINGERPRINT_CLIENT_HINTS_H_
