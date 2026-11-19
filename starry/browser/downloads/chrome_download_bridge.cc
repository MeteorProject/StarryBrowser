// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/chrome_download_bridge.h"

#include <memory>
#include <vector>

#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/download/download_status_updater.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/content/public/all_download_item_notifier.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_manager.h"
#include "extensions/buildflags/buildflags.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/downloads/starry_download.h"
#include "starry/browser/downloads/starry_download_item.h"
#include "starry/browser/starry_hooks.h"

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
#include "chrome/browser/extensions/api/downloads/downloads_api.h"
#endif

namespace starry {

namespace {

// 交给了 Starry 的 Chrome 下载（扩展不再看到它）。
const char kHiddenKey[] = "StarryHiddenFromExtensions";

class HiddenMarker : public base::SupportsUserData::Data {};

// 这个用户资料给扩展发下载事件的。
download::AllDownloadItemNotifier::Observer* ExtensionRouterFor(
    Profile* profile) {
#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  DownloadCoreService* core =
      DownloadCoreServiceFactory::GetForBrowserContext(profile);
  return core ? core->GetExtensionEventRouter() : nullptr;
#else
  return nullptr;
#endif
}

download::AllDownloadItemNotifier::Observer* StatusUpdater() {
  return g_browser_process ? g_browser_process->download_status_updater()
                           : nullptr;
}

// 扩展能看到的用户资料：context 的普通用户资料，要求时（或者 context 本身就是无痕的）加上
// 它的无痕（同 downloads_api.cc 的 GetManagers）。
std::vector<Profile*> ProfilesFor(content::BrowserContext* context,
                                  bool include_incognito) {
  Profile* profile = Profile::FromBrowserContext(context);
  std::vector<Profile*> profiles = {profile->GetOriginalProfile()};
  if (profile->HasPrimaryOTRProfile() &&
      (include_incognito || profile->IsOffTheRecord())) {
    profiles.push_back(
        profile->GetPrimaryOTRProfile(/*create_if_needed=*/false));
  }
  return profiles;
}

DownloadService* ServiceFor(content::DownloadManager* manager) {
  return manager ? DownloadService::GetIfRunning(Profile::FromBrowserContext(
                       manager->GetBrowserContext()))
                 : nullptr;
}

}  // namespace

void ReplaceForExtensions(download::DownloadItem* chrome_item,
                          StarryDownloadItem* replacement) {
  chrome_item->SetUserData(kHiddenKey, std::make_unique<HiddenMarker>());
#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  extensions::ExtensionDownloadsEventRouter::MoveItemData(chrome_item,
                                                          replacement);
#endif
}

// DownloadManager 传空的：这几个事件用不到它；DownloadStatusUpdater 拿到 DownloadManager 时
// 会按 Chrome 自己的下载重算「有下载时不关用户资料」，Starry 的自己管（DownloadService）。
void ItemStarted(Profile* profile, StarryDownloadItem* item, bool announce) {
  if (announce) {
    if (auto* router = ExtensionRouterFor(profile)) {
      router->OnDownloadCreated(nullptr, item);
    }
  }
  if (auto* updater = StatusUpdater()) {
    updater->OnDownloadCreated(nullptr, item);
  }
}

void ItemUpdated(Profile* profile, StarryDownloadItem* item, bool extensions) {
  if (extensions) {
    if (auto* router = ExtensionRouterFor(profile)) {
      router->OnDownloadUpdated(nullptr, item);
    }
  }
  if (auto* updater = StatusUpdater()) {
    updater->OnDownloadUpdated(nullptr, item);
  }
}

void ItemRemoved(Profile* profile, StarryDownloadItem* item) {
  if (auto* router = ExtensionRouterFor(profile)) {
    router->OnDownloadRemoved(nullptr, item);
  }
}

// ---- 上游补丁调的（starry_hooks.h） ----

download::DownloadItem* FindDownloadForExtensions(
    content::BrowserContext* context,
    bool include_incognito,
    int id) {
  if (!context || id <= 0) {
    return nullptr;
  }
  for (Profile* profile : ProfilesFor(context, include_incognito)) {
    if (DownloadService* service = DownloadService::GetIfRunning(profile)) {
      if (StarryDownloadItem* item =
              service->FindItem(static_cast<uint32_t>(id))) {
        return item;
      }
    }
  }
  return nullptr;
}

void AppendDownloadsForExtensions(
    content::DownloadManager* manager,
    content::DownloadManager* incognito_manager,
    const std::optional<int>& id,
    std::vector<raw_ptr<download::DownloadItem, VectorExperimental>>* items) {
  for (content::DownloadManager* each : {manager, incognito_manager}) {
    DownloadService* service = ServiceFor(each);
    if (!service) {
      continue;
    }
    if (!id) {
      service->AppendItems(items);
    } else if (*id > 0) {
      if (StarryDownloadItem* item =
              service->FindItem(static_cast<uint32_t>(*id))) {
        items->push_back(item);
      }
    }
  }
}

bool IsHiddenFromExtensions(const download::DownloadItem& item) {
  return item.GetUserData(kHiddenKey) != nullptr;
}

bool IsOffTheRecordDownload(const download::DownloadItem& item) {
  StarryDownloadItem* starry_item = StarryDownloadItem::From(&item);
  return starry_item &&
         starry_item->download()->service()->profile()->IsOffTheRecord();
}

void AddDownloadProgress(int* count,
                         int64_t* received_bytes,
                         int64_t* total_bytes,
                         bool* certain) {
  for (DownloadService* service : DownloadService::Running()) {
    service->AddProgress(count, received_bytes, total_bytes, certain);
  }
}

int DownloadsLostOnShutdown(Profile* profile) {
  DownloadService* service = DownloadService::GetIfRunning(profile);
  return service ? service->LostOnShutdownCount() : 0;
}

}  // namespace starry
