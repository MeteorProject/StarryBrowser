// Copyright 2026 The Starry Browser Authors

#include "starry/browser/privacy/process_time_zone.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/no_destructor.h"
#include "base/supports_user_data.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "ipc/ipc_channel_proxy.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/device/public/mojom/time_zone_monitor.mojom.h"
#include "starry/browser/privacy/location_simulation.h"
#include "starry/browser/web_time_zone.h"
#include "starry/common/web_time_zone.h"
#include "starry/components/privacy/mojom/web_time_zone.mojom.h"
#include "url/gurl.h"

namespace starry {

namespace {

constexpr int kProcessZoneKey = 0;

class ProcessZone : public base::SupportsUserData::Data {
 public:
  // 进程锁定的网站。没有时（还没分到网站，或可以放任何网站）按全局的结果。
  std::optional<GURL> site;
  // 网页进程现在用的时区（空字符串：跟随系统）。还没启动时为空。
  std::optional<std::string> applied;
};

ProcessZone* FindZoneData(content::RenderProcessHost* process) {
  return static_cast<ProcessZone*>(process->GetUserData(&kProcessZoneKey));
}

ProcessZone& ZoneData(content::RenderProcessHost* process) {
  ProcessZone* data = FindZoneData(process);
  if (!data) {
    auto owned = std::make_unique<ProcessZone>();
    data = owned.get();
    process->SetUserData(&kProcessZoneKey, std::move(owned));
  }
  return *data;
}

std::string ZoneFrom(Profile* profile, const LocationDecision& decision) {
  if (decision.simulated && decision.location &&
      CreateWebTimeZone(decision.location->time_zone)) {
    return decision.location->time_zone;
  }
  return EffectiveWebTimeZone(profile);
}

std::string DesiredZone(content::RenderProcessHost* process,
                        const ProcessZone& data) {
  Profile* profile = Profile::FromBrowserContext(process->GetBrowserContext());
  if (!profile) {
    return {};
  }
  return ZoneFrom(profile, data.site ? ResolveLocation(profile, *data.site)
                                     : ResolveDefaultLocation(profile));
}

void Push(content::RenderProcessHost* process, const std::string& id) {
  IPC::ChannelProxy* channel = process->GetChannel();
  if (!channel || id.empty()) {
    return;
  }
  // 每次新建一个端点：同一个通道上的关联接口按发送顺序到达，排在之后的
  // CommitNavigation 前面；调用发出后丢掉端点也不影响送达。
  mojo::AssociatedRemote<mojom::WebTimeZone> remote;
  channel->GetRemoteAssociatedInterface(&remote);
  remote->SetTimeZone(id);
}

void Apply(content::RenderProcessHost* process) {
  ProcessZone& data = ZoneData(process);
  if (!data.applied) {
    // 还没启动：启动参数会用到时候的值。
    return;
  }
  std::string zone = DesiredZone(process, data);
  if (zone == *data.applied) {
    return;
  }
  Push(process, zone.empty() ? SystemTimeZoneId() : zone);
  data.applied = std::move(zone);
}

// 系统时区：Chromium 的设备服务通知这里，再转给跟随系统的网页进程。
class SystemTimeZoneWatcher : public device::mojom::TimeZoneMonitorClient {
 public:
  static void EnsureStarted() {
    static base::NoDestructor<SystemTimeZoneWatcher> watcher;
  }

  SystemTimeZoneWatcher() {
    // 同 Blink 的 TimeZoneController：监视器只用来加客户端。
    mojo::Remote<device::mojom::TimeZoneMonitor> monitor;
    content::GetDeviceService().BindTimeZoneMonitor(
        monitor.BindNewPipeAndPassReceiver());
    monitor->AddClient(receiver_.BindNewPipeAndPassRemote());
  }

  void OnTimeZoneChange(const std::string& zone_id) override {
    for (auto it = content::RenderProcessHost::AllHostsIterator();
         !it.IsAtEnd(); it.Advance()) {
      content::RenderProcessHost* process = it.GetCurrentValue();
      ProcessZone* data = FindZoneData(process);
      if (data && data->applied && data->applied->empty()) {
        Push(process, zone_id);
      }
    }
  }

 private:
  mojo::Receiver<device::mojom::TimeZoneMonitorClient> receiver_{this};
};

// 交给 Blink 的 TimeZoneController：不发通知，免得异步到达的系统时区盖掉网站
// 的时区（它们走的不是同一条管道）。
class SilentTimeZoneMonitor : public device::mojom::TimeZoneMonitor {
 public:
  void AddClient(mojo::PendingRemote<device::mojom::TimeZoneMonitorClient>
                     client) override {}
};

}  // namespace

void NoteSiteInstanceProcess(content::SiteInstance* site_instance) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!site_instance->HasProcess() ||
      !site_instance->RequiresDedicatedProcess()) {
    return;
  }
  const GURL& site =
      site_instance->GetSecurityPrincipal().GetDeprecatedSiteURL();
  if (!site.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  content::RenderProcessHost* process = site_instance->GetProcess();
  ZoneData(process).site = site;
  Apply(process);
}

void AppendProcessWebTimeZone(content::RenderProcessHost* process,
                              base::CommandLine& command_line) {
  ProcessZone& data = ZoneData(process);
  std::string zone = DesiredZone(process, data);
  if (!zone.empty()) {
    command_line.AppendSwitchASCII(kWebTimeZoneSwitch, zone);
  }
  data.applied = std::move(zone);
}

bool CanUseSpareProcessForSite(content::BrowserContext* context,
                               const GURL& site_url) {
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile || !site_url.SchemeIsHTTPOrHTTPS()) {
    return true;
  }
  return ZoneFrom(profile, ResolveLocation(profile, site_url)) ==
         ZoneFrom(profile, ResolveDefaultLocation(profile));
}

void BindProcessTimeZoneMonitor(
    mojo::PendingReceiver<device::mojom::TimeZoneMonitor> receiver) {
  SystemTimeZoneWatcher::EnsureStarted();
  mojo::MakeSelfOwnedReceiver(std::make_unique<SilentTimeZoneMonitor>(),
                              std::move(receiver));
}

void UpdateProcessWebTimeZones() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto it = content::RenderProcessHost::AllHostsIterator(); !it.IsAtEnd();
       it.Advance()) {
    if (FindZoneData(it.GetCurrentValue())) {
      Apply(it.GetCurrentValue());
    }
  }
}

}  // namespace starry
