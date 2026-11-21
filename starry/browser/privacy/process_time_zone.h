// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_PRIVACY_PROCESS_TIME_ZONE_H_
#define STARRY_BROWSER_PRIVACY_PROCESS_TIME_ZONE_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/device/public/mojom/time_zone_monitor.mojom-forward.h"

class GURL;

namespace base {
class CommandLine;
}

namespace content {
class BrowserContext;
class RenderProcessHost;
class SiteInstance;
}  // namespace content

namespace starry {

// ICU 时区在渲染进程内共享，按进程锁定的站点选择。
void NoteSiteInstanceProcess(content::SiteInstance* site_instance);
void AppendProcessWebTimeZone(content::RenderProcessHost* process,
                              base::CommandLine& command_line);
bool CanUseSpareProcessForSite(content::BrowserContext* context,
                               const GURL& site_url);
void BindProcessTimeZoneMonitor(
    mojo::PendingReceiver<device::mojom::TimeZoneMonitor> receiver);

void UpdateProcessWebTimeZones();

}  // namespace starry

#endif  // STARRY_BROWSER_PRIVACY_PROCESS_TIME_ZONE_H_
