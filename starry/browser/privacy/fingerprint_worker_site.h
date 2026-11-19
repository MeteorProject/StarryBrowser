// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_PRIVACY_FINGERPRINT_WORKER_SITE_H_
#define STARRY_BROWSER_PRIVACY_FINGERPRINT_WORKER_SITE_H_

#include <optional>

#include "content/common/content_export.h"
#include "net/base/schemeful_site.h"
#include "third_party/blink/public/common/tokens/tokens.h"

namespace content {
class RenderProcessHost;
}

namespace starry {

// Compiled into //content/browser. Resolve the worker's actual storage
// partition, including shared/service workers without a live owner frame.
CONTENT_EXPORT std::optional<net::SchemefulSite> FingerprintWorkerSite(
    content::RenderProcessHost& process,
    const blink::ExecutionContextToken& token);

}  // namespace starry

#endif  // STARRY_BROWSER_PRIVACY_FINGERPRINT_WORKER_SITE_H_
