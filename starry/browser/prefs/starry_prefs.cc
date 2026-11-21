// Copyright 2026 The Starry Browser Authors

#include "starry/browser/prefs/starry_prefs.h"

#include <memory>
#include <string>
#include <utility>

#include "base/no_destructor.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"

namespace starry::prefs {

namespace {

// 只为了注册设置项：Chrome 建用户资料时（ProfileImpl 的构造函数）让依赖管理器里的
// 每个工厂 RegisterProfilePrefs。它不建任何服务。
class StarryPrefsFactory : public ProfileKeyedServiceFactory {
 public:
  static StarryPrefsFactory* GetInstance() {
    static base::NoDestructor<StarryPrefsFactory> instance;
    return instance.get();
  }

  StarryPrefsFactory() : ProfileKeyedServiceFactory("StarryPrefs") {}
  StarryPrefsFactory(const StarryPrefsFactory&) = delete;
  StarryPrefsFactory& operator=(const StarryPrefsFactory&) = delete;
  ~StarryPrefsFactory() override = default;

 private:
  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override {
    return nullptr;
  }

  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override {
    registry->RegisterStringPref(kWebTimeZone, std::string());
    registry->RegisterStringPref(kFingerprintProtection, "privateBrowsing");
    registry->RegisterListPref(kFingerprintProtectionExceptions);
    registry->RegisterStringPref(kLocationSimulation, "off");
    registry->RegisterDictionaryPref(kSimulatedLocation);
    registry->RegisterDictionaryPref(kSimulatedLocationSites);
    registry->RegisterStringPref(kExternalLinks, kExternalLinksMiniWindow);
    registry->RegisterIntegerPref(kMiniWindowWidth, 0);
    registry->RegisterIntegerPref(kMiniWindowHeight, 0);
    registry->RegisterBooleanPref(kPinnedPeek, true);
    registry->RegisterDictionaryPref(kFavorites);
    registry->RegisterBooleanPref(kPinsCollapsed, false);
    registry->RegisterDictionaryPref(kPinFolders);
    registry->RegisterDictionaryPref(kFavoriteFolders);
    registry->RegisterStringPref(kSidebarLayout, kSidebarLayoutDocked);
    registry->RegisterBooleanPref(kSidebarAutoHide, false);
    registry->RegisterIntegerPref(kSidebarWidth, 0);
    registry->RegisterBooleanPref(kAddressBarOnTop, false);
    registry->RegisterBooleanPref(kExtensionsOnTop, false);
    registry->RegisterBooleanPref(kCompactAddressBar, false);
    registry->RegisterBooleanPref(kTintFromWebsite, true);
    registry->RegisterStringPref(kWindowsControlsStyle,
                                 kWindowsControlsStyleGlass);
    registry->RegisterBooleanPref(kWindowsControlsAutoHide, false);
    registry->RegisterListPref(kHiddenUtilityButtons);
    registry->RegisterBooleanPref(kSleepRestoredTabs, true);
    registry->RegisterBooleanPref(kTabPreviewsEnabled, true);
    registry->RegisterBooleanPref(kTabPreviewShowsUsage, true);
    registry->RegisterBooleanPref(kTreeTabs, true);
    registry->RegisterIntegerPref(kTreeTabsMaxDepth, 4);
    registry->RegisterIntegerPref(kTreeTabsCloseParent, 0);
    registry->RegisterBooleanPref(kTreeTabsAutoCollapse, false);
    registry->RegisterBooleanPref(kAutoArchive, true);
    registry->RegisterIntegerPref(kAutoArchiveHours, 24 * 7);
    registry->RegisterBooleanPref(kArchiveDuplicates, true);
    registry->RegisterListPref(kOrganizerExceptions);
    registry->RegisterIntegerPref(kArchiveRetentionDays, 90);
    registry->RegisterStringPref(kOrganizerBackend, std::string());
    registry->RegisterStringPref(kOrganizerProvider, std::string());
    registry->RegisterStringPref(kOrganizerModel, std::string());
    registry->RegisterBooleanPref(kOrganizeHint, true);
    registry->RegisterBooleanPref(kArchiveTopics, true);
    registry->RegisterBooleanPref(kOrganizeSuggest, true);
    registry->RegisterDictionaryPref(kOrganizerMemory);
    registry->RegisterBooleanPref(kMediaControlsEnabled, true);
    registry->RegisterBooleanPref(kMediaAutoPip, true);
    registry->RegisterBooleanPref(kMediaTabMeters, true);
    registry->RegisterBooleanPref(kMediaDocked, false);
    registry->RegisterIntegerPref(kMediaCorner, 3);
    registry->RegisterStringPref(kAssistantProvider, std::string());
    registry->RegisterDictionaryPref(kAssistantModels);
    registry->RegisterDictionaryPref(kAssistantKeys);
    registry->RegisterStringPref(kAssistantCustomUrl, std::string());
    registry->RegisterStringPref(kAssistantMode, "ask");
    registry->RegisterStringPref(kAssistantBackend, "chat");
    registry->RegisterDictionaryPref(kAssistantLocalPaths);
    registry->RegisterDictionaryPref(kAssistantLocalModels);
    registry->RegisterDictionaryPref(kAssistantReasoning);
    registry->RegisterStringPref(kAssistantPanelStyle, "float");
    registry->RegisterBooleanPref(kAssistantEnabled, true);
    registry->RegisterBooleanPref(kAgentAllowExternal, false);
    registry->RegisterBooleanPref(kAgentAllowScripts, false);
    registry->RegisterBooleanPref(kAgentAllowUploads, true);
    registry->RegisterBooleanPref(kAgentAllowPrivateTabs, true);
    registry->RegisterBooleanPref(kDownloadMultiConnection, true);
    registry->RegisterIntegerPref(kDownloadConnections, 8);
    registry->RegisterIntegerPref(kDownloadMaxConcurrent, 3);
    registry->RegisterIntegerPref(kDownloadSpeedLimit, 0);
    registry->RegisterBooleanPref(kDownloadAutoRetry, true);
    registry->RegisterBooleanPref(kDownloadResumeOnLaunch, true);
    registry->RegisterBooleanPref(kDownloadNotifyOnCompletion, true);
    registry->RegisterBooleanPref(kOnboardingPending, false);
    registry->RegisterIntegerPref(kOnboardingVersion, 0);
    registry->RegisterStringPref(kNewTabStyle, kNewTabStyleMinimal);
    registry->RegisterStringPref(kNewTabBackground, "theme");
    base::ListValue widgets;
    for (const char* id :
         {"clock", "shortcuts", "continue", "playing", "archive", "shield"}) {
      widgets.Append(id);
    }
    registry->RegisterListPref(kNewTabWidgets, std::move(widgets));
    registry->RegisterIntegerPref(kNewTabShortcutRows, 1);
    registry->RegisterStringPref(kNewTabNote, std::string());
    registry->RegisterDictionaryPref(kBlockedStats);
  }
};

}  // namespace

void EnsureRegistered() {
  StarryPrefsFactory::GetInstance();
}

PrefService* PrefsFor(Profile* profile) {
  return profile->GetOriginalProfile()->GetPrefs();
}

}  // namespace starry::prefs
