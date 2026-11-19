// Copyright 2026 The Starry Browser Authors

#include "starry/browser/privacy/fingerprint_worker_site.h"

#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_info.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/browser/worker_host/dedicated_worker_host.h"
#include "content/browser/worker_host/dedicated_worker_service_impl.h"
#include "content/browser/worker_host/shared_worker_host.h"
#include "content/browser/worker_host/shared_worker_service_impl.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/shared_worker_instance.h"
#include "content/public/browser/storage_partition.h"

namespace starry {

std::optional<net::SchemefulSite> FingerprintWorkerSite(
    content::RenderProcessHost& process,
    const blink::ExecutionContextToken& token) {
  content::StoragePartition* partition = process.GetStoragePartition();
  if (token.Is<blink::DedicatedWorkerToken>()) {
    auto* service = static_cast<content::DedicatedWorkerServiceImpl*>(
        partition->GetDedicatedWorkerService());
    auto* worker = service->GetDedicatedWorkerHostFromToken(
        token.GetAs<blink::DedicatedWorkerToken>());
    if (worker && worker->GetProcessHost() == &process) {
      return worker->GetWorkerStorageKey()
          .CopyWithForceEnabledThirdPartyStoragePartitioning()
          .top_level_site();
    }
  } else if (token.Is<blink::SharedWorkerToken>()) {
    auto* service = static_cast<content::SharedWorkerServiceImpl*>(
        partition->GetSharedWorkerService());
    auto* worker = service->GetSharedWorkerHostFromToken(
        token.GetAs<blink::SharedWorkerToken>());
    if (worker && worker->GetProcessHost() == &process) {
      return worker->instance()
          .worker_storage_key()
          .CopyWithForceEnabledThirdPartyStoragePartitioning()
          .top_level_site();
    }
  } else if (token.Is<blink::ServiceWorkerToken>()) {
    auto* service = static_cast<content::ServiceWorkerContextWrapper*>(
        partition->GetServiceWorkerContext());
    // The running-worker list is populated after the first script has run.
    // Include starting versions so that this first script is protected too.
    for (const auto& info : service->GetAllLiveVersionInfo()) {
      if (info.process_id != process.GetID()) {
        continue;
      }
      auto* worker = service->GetLiveVersion(info.version_id);
      if (worker && worker->start_worker_token() ==
                        token.GetAs<blink::ServiceWorkerToken>()) {
        return info.storage_key
            .CopyWithForceEnabledThirdPartyStoragePartitioning()
            .top_level_site();
      }
    }
  }
  return std::nullopt;
}

}  // namespace starry
