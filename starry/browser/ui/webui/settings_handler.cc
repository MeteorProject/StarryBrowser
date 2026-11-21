// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/settings_handler.h"

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "chrome/common/pref_names.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/language/core/browser/language_prefs.h"
#include "components/language/core/browser/pref_names.h"
#include "components/performance_manager/public/user_tuning/prefs.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/search_engines/template_url.h"
#include "components/tabs/public/tab_interface.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/l10n/interface_language.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/new_tab/new_tab.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/privacy/fingerprint_protection.h"
#include "starry/browser/privacy/location_simulation.h"
#include "starry/browser/tab_organizer/organizer_memory.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/chrome_layout.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_controls.h"
#include "starry/browser/ui/location/location_picker.h"
#include "starry/browser/ui/sidebar/utility_buttons.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "starry/browser/web_time_zone.h"
#include "starry/common/version.h"
#include "starry/common/web_time_zone.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace starry {

namespace {

namespace perf_prefs = performance_manager::user_tuning::prefs;

// 一连串的变化（拖滑块、一次改几项）合成一次通知。
constexpr base::TimeDelta kChangedDelay = base::Milliseconds(50);

// 网页语言的选项（第一个是网页优先用的语言），名字用这门语言自己的写法。
constexpr std::string_view kWebLanguages[] = {
    "zh-CN", "zh-TW", "zh-HK", "en-US", "en-GB", "ja", "ko",
    "fr",    "de",    "es",    "it",    "pt-BR", "ru",
};

// 侧栏底部的按钮（同 utility_buttons.cc 的顺序和名字）。
struct ButtonName {
  UtilityButton button;
  std::u16string title;
};

std::vector<ButtonName> ButtonNames() {
  return {
      {UtilityButton::kDownloads, L("common.downloads")},
      {UtilityButton::kHistory, L("common.history")},
      {UtilityButton::kBookmarks, L("common.bookmarks")},
      {UtilityButton::kExtensions, L("common.extensions")},
      {UtilityButton::kAssistant, L("utilityButton.assistant")},
      {UtilityButton::kLayout, L("utilityButton.layout")},
      {UtilityButton::kSettings, L("common.settings")},
  };
}

std::string StartupName(SessionStartupPref::Type type) {
  switch (type) {
    case SessionStartupPref::LAST:
      return "last";
    case SessionStartupPref::URLS:
      return "urls";
    case SessionStartupPref::LAST_AND_URLS:
      return "lastAndUrls";
    case SessionStartupPref::DEFAULT:
      return "newTab";
  }
  return "newTab";
}

std::string ColorSchemeName(ThemeService::BrowserColorScheme scheme) {
  switch (scheme) {
    case ThemeService::BrowserColorScheme::kLight:
      return "light";
    case ThemeService::BrowserColorScheme::kDark:
      return "dark";
    default:
      return "system";
  }
}

// 将用户主目录前缀缩写为 ~。
std::string DisplayPath(const base::FilePath& path) {
  const base::FilePath home = base::GetHomeDir();
  base::FilePath relative;
  if (!home.empty() && home.AppendRelativePath(path, &relative)) {
    return base::StrCat({"~/", relative.AsUTF8Unsafe()});
  }
  return path.AsUTF8Unsafe();
}

std::vector<std::string> AcceptLanguages(PrefService* prefs) {
  return base::SplitString(prefs->GetString(language::prefs::kAcceptLanguages),
                           ",", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY);
}

}  // namespace

SettingsHandler::SettingsHandler(Profile* profile)
    : profile_(profile->GetOriginalProfile()) {}

SettingsHandler::~SettingsHandler() {
  if (select_folder_dialog_) {
    select_folder_dialog_->ListenerDestroyed();
  }
}

void SettingsHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getSettings", &SettingsHandler::HandleGetSettings);
  add("setSetting", &SettingsHandler::HandleSetSetting);
  add("setFingerprintException",
      &SettingsHandler::HandleSetFingerprintException);
  add("setSiteLocation", &SettingsHandler::HandleSetSiteLocation);
  add("pickLocation", &SettingsHandler::HandlePickLocation);
  add("setOrganizerException", &SettingsHandler::HandleSetOrganizerException);
  add("clearArchive", &SettingsHandler::HandleClearArchive);
  add("openArchive", &SettingsHandler::HandleOpenArchive);
  add("setOrganizerRule", &SettingsHandler::HandleSetOrganizerRule);
  add("forgetOrganizerMemory", &SettingsHandler::HandleForgetOrganizerMemory);
  add("checkDefaultBrowser", &SettingsHandler::HandleCheckDefaultBrowser);
  add("setDefaultBrowser", &SettingsHandler::HandleSetDefaultBrowser);
  add("relaunch", &SettingsHandler::HandleRelaunch);
  add("chooseDownloadFolder", &SettingsHandler::HandleChooseDownloadFolder);
  add("startOnboarding", &SettingsHandler::HandleStartOnboarding);
}

void SettingsHandler::OnJavascriptAllowed() {
  const auto changed = base::BindRepeating(&SettingsHandler::ScheduleChanged,
                                           base::Unretained(this));
  profile_prefs_.Init(profile_->GetPrefs());
  for (const char* name : {
           ::prefs::kSearchSuggestEnabled,
           ::prefs::kRestoreOnStartup,
           ::prefs::kBrowserColorScheme,
           ::prefs::kNetworkPredictionOptions,
           ::prefs::kDownloadDefaultDirectory,
           ::prefs::kPromptForDownload,
           ::prefs::kEnableDoNotTrack,
           ::prefs::kCookieControlsMode,
#if BUILDFLAG(IS_LINUX)
           ::prefs::kUseCustomChromeFrame,
#endif
           language::prefs::kAcceptLanguages,
           starry::prefs::kWebTimeZone,
           starry::prefs::kExternalLinks,
           starry::prefs::kSleepRestoredTabs,
           starry::prefs::kFingerprintProtection,
           starry::prefs::kFingerprintProtectionExceptions,
           starry::prefs::kLocationSimulation,
           starry::prefs::kSimulatedLocation,
           starry::prefs::kSimulatedLocationSites,
           starry::prefs::kSidebarLayout,
           starry::prefs::kSidebarAutoHide,
           starry::prefs::kSidebarWidth,
           starry::prefs::kHiddenUtilityButtons,
           starry::prefs::kAddressBarOnTop,
           starry::prefs::kExtensionsOnTop,
           starry::prefs::kCompactAddressBar,
           starry::prefs::kTintFromWebsite,
           starry::prefs::kWindowsControlsStyle,
           starry::prefs::kWindowsControlsAutoHide,
           starry::prefs::kTreeTabs,
           starry::prefs::kTreeTabsMaxDepth,
           starry::prefs::kTreeTabsCloseParent,
           starry::prefs::kTreeTabsAutoCollapse,
           starry::prefs::kAutoArchive,
           starry::prefs::kAutoArchiveHours,
           starry::prefs::kArchiveDuplicates,
           starry::prefs::kOrganizerExceptions,
           starry::prefs::kArchiveRetentionDays,
           starry::prefs::kOrganizerBackend,
           starry::prefs::kOrganizerProvider,
           starry::prefs::kOrganizerModel,
           starry::prefs::kOrganizeHint,
           starry::prefs::kArchiveTopics,
           starry::prefs::kOrganizeSuggest,
           starry::prefs::kOrganizerMemory,
           starry::prefs::kPinFolders,
           starry::prefs::kAssistantKeys,
           starry::prefs::kMediaControlsEnabled,
           starry::prefs::kMediaDocked,
           starry::prefs::kMediaCorner,
           starry::prefs::kMediaAutoPip,
           starry::prefs::kMediaTabMeters,
           starry::prefs::kDownloadMultiConnection,
           starry::prefs::kDownloadConnections,
           starry::prefs::kDownloadMaxConcurrent,
           starry::prefs::kDownloadSpeedLimit,
           starry::prefs::kDownloadAutoRetry,
           starry::prefs::kDownloadResumeOnLaunch,
           starry::prefs::kDownloadNotifyOnCompletion,
           starry::prefs::kNewTabStyle,
           starry::prefs::kNewTabBackground,
           starry::prefs::kNewTabWidgets,
           starry::prefs::kNewTabShortcutRows,
       }) {
    profile_prefs_.Add(name, changed);
  }
  local_prefs_.Init(g_browser_process->local_state());
  local_prefs_.Add(perf_prefs::kMemorySaverModeState, changed);
  local_prefs_.Add(perf_prefs::kMemorySaverModeAggressiveness, changed);
  if (!profile_->IsOffTheRecord()) {
    archive_observation_.Observe(
        tab_organizer::TabArchive::ForProfile(profile_));
  }
  if (TemplateURLService* service =
          TemplateURLServiceFactory::GetForProfile(profile_)) {
    template_url_observation_.Observe(service);
    service->Load();
  }
}

void SettingsHandler::OnJavascriptDisallowed() {
  profile_prefs_.RemoveAll();
  local_prefs_.RemoveAll();
  template_url_observation_.Reset();
  archive_observation_.Reset();
  changed_timer_.Stop();
  weak_factory_.InvalidateWeakPtrs();
}

void SettingsHandler::OnTemplateURLServiceChanged() {
  ScheduleChanged();
}

void SettingsHandler::OnArchiveChanged() {
  ScheduleChanged();
}

void SettingsHandler::ScheduleChanged() {
  if (changed_timer_.IsRunning()) {
    return;
  }
  changed_timer_.Start(FROM_HERE, kChangedDelay,
                       base::BindOnce(
                           [](SettingsHandler* handler) {
                             if (handler->IsJavascriptAllowed()) {
                               handler->FireWebUIListener(
                                   "settings-changed",
                                   base::Value(handler->State()));
                             }
                           },
                           base::Unretained(this)));
}

base::DictValue SettingsHandler::State() {
  PrefService* prefs = profile_->GetPrefs();
  PrefService* local = g_browser_process->local_state();
  base::DictValue state;
  state.Set("version", starry::kVersion);
  state.Set("engineVersion", version_info::GetVersionNumber());
  state.Set("incognito", Profile::FromWebUI(web_ui())->IsOffTheRecord());

  // 通用
  base::DictValue default_browser;
  default_browser.Set("state", default_browser_);
  default_browser.Set("canSet", shell_integration::CanSetAsDefaultBrowser());
  state.Set("defaultBrowser", std::move(default_browser));

  base::ListValue engines;
  std::string engine_id;
  bool engine_managed = false;
  if (TemplateURLService* service =
          TemplateURLServiceFactory::GetForProfile(profile_)) {
    const TemplateURL* current = service->GetDefaultSearchProvider();
    engine_managed = service->is_default_search_managed();
    for (TemplateURL* engine : service->GetTemplateURLs()) {
      if (engine != current && (!service->ShowInDefaultList(engine) ||
                                !service->CanMakeDefault(engine))) {
        continue;
      }
      base::DictValue entry;
      entry.Set("id", base::NumberToString(engine->id().value()));
      entry.Set("name", engine->short_name());
      engines.Append(std::move(entry));
    }
    if (current) {
      engine_id = base::NumberToString(current->id().value());
    }
  }
  state.Set("searchEngines", std::move(engines));
  state.Set("searchEngine", engine_id);
  state.Set("searchEngineManaged", engine_managed);
  state.Set("searchSuggest", prefs->GetBoolean(::prefs::kSearchSuggestEnabled));
  state.Set("startup",
            StartupName(SessionStartupPref::GetStartupPref(profile_).type));
  state.Set("startupManaged", SessionStartupPref::TypeIsManaged(prefs));
  state.Set("sleepRestoredTabs",
            prefs->GetBoolean(starry::prefs::kSleepRestoredTabs));
  // 标签管理。
  state.Set("autoArchive", prefs->GetBoolean(starry::prefs::kAutoArchive));
  state.Set("autoArchiveHours",
            prefs->GetInteger(starry::prefs::kAutoArchiveHours));
  state.Set("archiveDuplicates",
            prefs->GetBoolean(starry::prefs::kArchiveDuplicates));
  state.Set("organizerExceptions",
            prefs->GetList(starry::prefs::kOrganizerExceptions).Clone());
  state.Set("archiveRetentionDays",
            prefs->GetInteger(starry::prefs::kArchiveRetentionDays));
  state.Set("archiveCount",
            profile_->IsOffTheRecord()
                ? 0
                : static_cast<int>(
                      tab_organizer::TabArchive::ForProfile(profile_)
                          ->tabs()
                          .size()));
  state.Set("organizerBackend",
            prefs->GetString(starry::prefs::kOrganizerBackend));
  state.Set("organizerProvider",
            prefs->GetString(starry::prefs::kOrganizerProvider));
  state.Set("organizerModel", prefs->GetString(starry::prefs::kOrganizerModel));
  state.Set("organizeHint", prefs->GetBoolean(starry::prefs::kOrganizeHint));
  state.Set("archiveTopics", prefs->GetBoolean(starry::prefs::kArchiveTopics));
  // 按网站规则提示放进文件夹：开关、规则、可选文件夹与学习状态。
  state.Set("organizeSuggest",
            prefs->GetBoolean(starry::prefs::kOrganizeSuggest));
  {
    std::map<std::string, std::u16string> paths;
    base::ListValue folders;
    for (const pin_folders::Folder& folder : pin_folders::Read(profile_)) {
      paths[folder.id] = folder.path;
      folders.Append(
          base::DictValue().Set("id", folder.id).Set("title", folder.path));
    }
    base::ListValue rules;
    int learned = 0;
    for (const tab_organizer::SiteRule& rule :
         tab_organizer::ReadRules(profile_)) {
      learned += rule.learned ? 1 : 0;
      rules.Append(base::DictValue()
                       .Set("site", rule.site)
                       .Set("folder", rule.folder)
                       .Set("folderTitle", paths[rule.folder])
                       .Set("learned", rule.learned));
    }
    state.Set("pinFolders", std::move(folders));
    state.Set("organizerRules", std::move(rules));
    // 有没有学到的东西（「忘掉学到的」能不能点）。
    const base::DictValue& memory =
        prefs->GetDict(starry::prefs::kOrganizerMemory);
    state.Set("organizerLearned", learned > 0 || memory.FindDict("placed") ||
                                      memory.FindDict("rejected") ||
                                      memory.FindList("names"));
  }
  {
    // 单独选的服务有没有存 Key（没选服务时看 AI 助手用的那家）。
    std::string provider = prefs->GetString(starry::prefs::kOrganizerProvider);
    if (provider.empty()) {
      provider = prefs->GetString(starry::prefs::kAssistantProvider);
    }
    state.Set("organizerHasKey",
              prefs->GetDict(starry::prefs::kAssistantKeys).contains(provider));
  }
  state.Set("externalLinks", prefs->GetString(starry::prefs::kExternalLinks) ==
                                     starry::prefs::kExternalLinksBrowserWindow
                                 ? starry::prefs::kExternalLinksBrowserWindow
                                 : starry::prefs::kExternalLinksMiniWindow);

  const std::vector<std::string> accept = AcceptLanguages(prefs);
  const std::string web_language = accept.empty() ? "" : accept.front();
  base::ListValue languages;
  bool listed = false;
  for (std::string_view code : kWebLanguages) {
    base::DictValue entry;
    entry.Set("id", code);
    entry.Set("name", l10n_util::GetDisplayNameForLocale(
                          std::string(code), std::string(code),
                          /*is_for_ui=*/true));
    languages.Append(std::move(entry));
    listed |= code == web_language;
  }
  if (!listed && !web_language.empty()) {
    base::DictValue entry;
    entry.Set("id", web_language);
    entry.Set("name", l10n_util::GetDisplayNameForLocale(
                          web_language, web_language, /*is_for_ui=*/true));
    languages.Append(std::move(entry));
  }
  state.Set("webLanguages", std::move(languages));
  state.Set("webLanguage", web_language);
  AddWebTimeZoneSettings(profile_, state);

  // 界面语言：选了的（或跟随系统），和这次启动实际用的。两个不一样时页面提示重新启动。
  const std::optional<l10n::Language> chosen = l10n::ChosenLanguage();
  const l10n::Language effective =
      chosen.value_or(l10n::SystemLanguage());
  base::ListValue interface_languages;
  {
    base::DictValue entry;
    entry.Set("id", "system");
    entry.Set("name",
              L("settings.general.interfaceLanguage.system",
                {{"language", l10n::NativeName(l10n::SystemLanguage())}}));
    interface_languages.Append(std::move(entry));
  }
  for (l10n::Language language : l10n::kAllLanguages) {
    base::DictValue entry;
    entry.Set("id", l10n::LanguageCode(language));
    entry.Set("name", l10n::NativeName(language));
    interface_languages.Append(std::move(entry));
  }
  state.Set("interfaceLanguages", std::move(interface_languages));
  state.Set("interfaceLanguage",
            chosen ? std::string(l10n::LanguageCode(*chosen)) : "system");
  state.Set("interfaceLanguageNeedsRelaunch",
            effective != l10n::CurrentLanguage());

  // 外观
  state.Set("colorScheme",
            ColorSchemeName(ThemeServiceFactory::GetForProfile(profile_)
                                ->GetBrowserColorScheme()));
  const TabLayout tab_layout = ChromeLayout::TabLayoutFromPrefs(prefs);
  state.Set("sidebarLayout",
            tab_layout == TabLayout::kTop
                ? starry::prefs::kSidebarLayoutTop
            : tab_layout == TabLayout::kFloatingSidebar
                ? starry::prefs::kSidebarLayoutFloating
                : starry::prefs::kSidebarLayoutDocked);
  state.Set("addressBarOnTop",
            prefs->GetBoolean(starry::prefs::kAddressBarOnTop));
  state.Set("extensionsOnTop",
            prefs->GetBoolean(starry::prefs::kExtensionsOnTop));
  state.Set("compactAddressBar",
            prefs->GetBoolean(starry::prefs::kCompactAddressBar));
  state.Set("tintFromWebsite",
            prefs->GetBoolean(starry::prefs::kTintFromWebsite));
  bool has_window_controls = WindowsControlsEnabled();
#if BUILDFLAG(IS_LINUX)
  if (has_window_controls && web_ui()) {
    if (content::WebContents* contents = web_ui()->GetWebContents()) {
      if (tabs::TabInterface* tab =
              tabs::TabInterface::MaybeGetFromContents(contents)) {
        if (BrowserWindowInterface* browser =
                tab->GetBrowserWindowInterface()) {
          if (StarryBrowserView* view = StarryBrowserView::From(browser)) {
            has_window_controls = view->UsesCustomWindowControls();
          }
        }
      }
    }
  }
#endif
  state.Set("hasWindowControls", has_window_controls);
  const std::string controls_style =
      prefs->GetString(starry::prefs::kWindowsControlsStyle);
  state.Set("windowsControlsStyle",
            controls_style == starry::prefs::kWindowsControlsStyleRail
                ? starry::prefs::kWindowsControlsStyleRail
                : starry::prefs::kWindowsControlsStyleGlass);
  state.Set("windowsControlsAutoHide",
            prefs->GetBoolean(starry::prefs::kWindowsControlsAutoHide));
  const int width = prefs->GetInteger(starry::prefs::kSidebarWidth);
  state.Set("sidebarWidth", width > 0 ? width : metrics::kSidebarWidth);
  state.Set("sidebarWidthDefault", metrics::kSidebarWidth);
  state.Set("pinnedPeek", prefs->GetBoolean(starry::prefs::kPinnedPeek));
  state.Set("tabPreviewsEnabled",
            prefs->GetBoolean(starry::prefs::kTabPreviewsEnabled));
  state.Set("tabPreviewShowsUsage",
            prefs->GetBoolean(starry::prefs::kTabPreviewShowsUsage));
  state.Set("treeTabs", prefs->GetBoolean(starry::prefs::kTreeTabs));
  state.Set("treeTabsMaxDepth",
            prefs->GetInteger(starry::prefs::kTreeTabsMaxDepth));
  state.Set("treeTabsCloseParent",
            prefs->GetInteger(starry::prefs::kTreeTabsCloseParent));
  state.Set("treeTabsAutoCollapse",
            prefs->GetBoolean(starry::prefs::kTreeTabsAutoCollapse));
  state.Set("sidebarWidthMin", metrics::kSidebarMinWidth);
  state.Set("sidebarWidthMax", metrics::kSidebarMaxWidth);
  const base::ListValue& hidden =
      prefs->GetList(starry::prefs::kHiddenUtilityButtons);
  base::ListValue buttons;
  for (const ButtonName& name : ButtonNames()) {
    const char* key = UtilityButtonKey(name.button);
    base::DictValue entry;
    entry.Set("id", key);
    entry.Set("title", name.title);
    entry.Set("hidden", std::ranges::find(hidden, base::Value(key)) !=
                            hidden.end());
    buttons.Append(std::move(entry));
  }
  state.Set("utilityButtons", std::move(buttons));

  // 新标签页
  state.Set("newTabStyle", prefs->GetString(starry::prefs::kNewTabStyle));
  state.Set("newTabBackground",
            prefs->GetString(starry::prefs::kNewTabBackground));
  state.Set("newTabWidgets",
            prefs->GetList(starry::prefs::kNewTabWidgets).Clone());
  state.Set(
      "newTabShortcutRows",
      std::clamp(prefs->GetInteger(starry::prefs::kNewTabShortcutRows), 1, 2));

  // 性能
  state.Set("memorySaver",
            local->GetInteger(perf_prefs::kMemorySaverModeState) ==
                static_cast<int>(perf_prefs::MemorySaverModeState::kEnabled));
  state.Set("memorySaverAggressiveness",
            local->GetInteger(perf_prefs::kMemorySaverModeAggressiveness));
  state.Set("preload", prefs->GetInteger(::prefs::kNetworkPredictionOptions));

  // 媒体
  state.Set("mediaControlsEnabled",
            prefs->GetBoolean(starry::prefs::kMediaControlsEnabled));
  state.Set("mediaDocked", prefs->GetBoolean(starry::prefs::kMediaDocked));
  state.Set("mediaCorner",
            std::clamp(prefs->GetInteger(starry::prefs::kMediaCorner), 0, 3));
  state.Set("mediaAutoPip", prefs->GetBoolean(starry::prefs::kMediaAutoPip));
  state.Set("mediaTabMeters",
            prefs->GetBoolean(starry::prefs::kMediaTabMeters));

  // 下载
  state.Set("downloadDirectory",
            DisplayPath(prefs->GetFilePath(::prefs::kDownloadDefaultDirectory)));
  state.Set("askWhereToSave", prefs->GetBoolean(::prefs::kPromptForDownload));
  state.Set("downloadMultiConnection",
            prefs->GetBoolean(starry::prefs::kDownloadMultiConnection));
  state.Set("downloadConnections",
            prefs->GetInteger(starry::prefs::kDownloadConnections));
  state.Set("downloadMaxConcurrent",
            prefs->GetInteger(starry::prefs::kDownloadMaxConcurrent));
  state.Set("downloadSpeedLimit",
            prefs->GetInteger(starry::prefs::kDownloadSpeedLimit));
  state.Set("downloadAutoRetry",
            prefs->GetBoolean(starry::prefs::kDownloadAutoRetry));
  state.Set("downloadResumeOnLaunch",
            prefs->GetBoolean(starry::prefs::kDownloadResumeOnLaunch));
  state.Set("downloadNotifyOnCompletion",
            prefs->GetBoolean(starry::prefs::kDownloadNotifyOnCompletion));

  // 隐私
  const std::string& fingerprint_mode =
      prefs->GetString(starry::prefs::kFingerprintProtection);
  state.Set("fingerprintProtection",
            IsFingerprintProtectionMode(fingerprint_mode) ? fingerprint_mode
                                                          : "privateBrowsing");
  state.Set(
      "fingerprintExceptions",
      prefs->GetList(starry::prefs::kFingerprintProtectionExceptions).Clone());
  AddLocationSimulationSettings(profile_, state);
  state.Set("cookies", prefs->GetInteger(::prefs::kCookieControlsMode));
  state.Set("doNotTrack", prefs->GetBoolean(::prefs::kEnableDoNotTrack));
  return state;
}

void SettingsHandler::HandleGetSettings(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(State()));
}

void SettingsHandler::HandleSetSetting(const base::ListValue& args) {
  if (args.size() < 2 || !args[0].is_string()) {
    return;
  }
  const std::string& key = args[0].GetString();
  const base::Value& value = args[1];
  PrefService* prefs = profile_->GetPrefs();
  PrefService* local = g_browser_process->local_state();

  if (key == "searchEngine" && value.is_string()) {
    TemplateURLService* service =
        TemplateURLServiceFactory::GetForProfile(profile_);
    if (!service) {
      return;
    }
    for (TemplateURL* engine : service->GetTemplateURLs()) {
      if (base::NumberToString(engine->id().value()) == value.GetString() &&
          service->CanMakeDefault(engine)) {
        service->SetUserSelectedDefaultSearchProvider(engine);
        break;
      }
    }
  } else if (key == "searchSuggest" && value.is_bool()) {
    prefs->SetBoolean(::prefs::kSearchSuggestEnabled, value.GetBool());
  } else if (key == "startup" && value.is_string()) {
    SessionStartupPref pref = SessionStartupPref::GetStartupPref(profile_);
    pref.type = value.GetString() == "last" ? SessionStartupPref::LAST
                                            : SessionStartupPref::DEFAULT;
    SessionStartupPref::SetStartupPref(profile_, pref);
  } else if (key == "sleepRestoredTabs" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kSleepRestoredTabs, value.GetBool());
  } else if ((key == "autoArchive" || key == "archiveDuplicates") &&
             value.is_bool()) {
    prefs->SetBoolean(key == "autoArchive" ? starry::prefs::kAutoArchive
                                           : starry::prefs::kArchiveDuplicates,
                      value.GetBool());
  } else if (key == "organizeSuggest" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kOrganizeSuggest, value.GetBool());
  } else if ((key == "organizeHint" || key == "archiveTopics") &&
             value.is_bool()) {
    prefs->SetBoolean(key == "organizeHint" ? starry::prefs::kOrganizeHint
                                            : starry::prefs::kArchiveTopics,
                      value.GetBool());
  } else if (key == "autoArchiveHours" && value.is_int()) {
    // 闲置多久后归档：菜单里的几档（12 小时到 30 天）。
    prefs->SetInteger(starry::prefs::kAutoArchiveHours,
                      std::clamp(value.GetInt(), 1, 24 * 365));
  } else if (key == "archiveRetentionDays" && value.is_int()) {
    prefs->SetInteger(starry::prefs::kArchiveRetentionDays,
                      std::clamp(value.GetInt(), 0, 3650));
  } else if (key == "organizerBackend" && value.is_string()) {
    const std::string& backend = value.GetString();
    if (backend.empty() || backend == "chat" || backend == "claudeCode" ||
        backend == "codex") {
      prefs->SetString(starry::prefs::kOrganizerBackend, backend);
    }
  } else if ((key == "organizerProvider" || key == "organizerModel") &&
             value.is_string()) {
    prefs->SetString(key == "organizerProvider"
                         ? starry::prefs::kOrganizerProvider
                         : starry::prefs::kOrganizerModel,
                     std::string(base::TrimWhitespaceASCII(
                         value.GetString(), base::TRIM_ALL)));
  } else if (key == "externalLinks" && value.is_string()) {
    prefs->SetString(
        starry::prefs::kExternalLinks,
        value.GetString() == starry::prefs::kExternalLinksBrowserWindow
            ? starry::prefs::kExternalLinksBrowserWindow
            : starry::prefs::kExternalLinksMiniWindow);
  } else if (key == "webLanguage" && value.is_string()) {
    // 选的语言放到用户列表最前面，其余的照旧。AcceptLanguages 是派生值，
    // 直接写它会在下一次创建 LanguagePrefs（如网页翻译）时被覆盖。
    language::LanguagePrefs language_prefs(prefs);
    std::vector<std::string> languages;
    language_prefs.GetUserSelectedLanguagesList(&languages);
    std::erase(languages, value.GetString());
    languages.insert(languages.begin(), value.GetString());
    language_prefs.SetUserSelectedLanguagesList(languages);
  } else if (key == "webTimeZone" && value.is_string()) {
    if (value.GetString().empty() || CreateWebTimeZone(value.GetString())) {
      prefs->SetString(starry::prefs::kWebTimeZone, value.GetString());
    }
  } else if (key == "interfaceLanguage" && value.is_string()) {
    std::optional<l10n::Language> language;
    for (l10n::Language candidate : l10n::kAllLanguages) {
      if (l10n::LanguageCode(candidate) == value.GetString()) {
        language = candidate;
      }
    }
    l10n::SetChosenLanguage(language);
    // 存在系统的设置里（macOS 的 AppleLanguages），没有 Chrome 的设置变化通知：自己发。
    ScheduleChanged();
  } else if (key == "colorScheme" && value.is_string()) {
    const std::string& name = value.GetString();
    ThemeServiceFactory::GetForProfile(profile_)->SetBrowserColorScheme(
        name == "light"  ? ThemeService::BrowserColorScheme::kLight
        : name == "dark" ? ThemeService::BrowserColorScheme::kDark
                         : ThemeService::BrowserColorScheme::kSystem);
  } else if (key == "sidebarLayout" && value.is_string()) {
    const std::string& layout = value.GetString();
    prefs->SetBoolean(starry::prefs::kSidebarAutoHide, false);
    prefs->SetString(starry::prefs::kSidebarLayout,
                     layout == starry::prefs::kSidebarLayoutFloating
                         ? starry::prefs::kSidebarLayoutFloating
                     : layout == starry::prefs::kSidebarLayoutTop
                         ? starry::prefs::kSidebarLayoutTop
                         : starry::prefs::kSidebarLayoutDocked);
  } else if (key == "addressBarOnTop" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kAddressBarOnTop, value.GetBool());
  } else if (key == "extensionsOnTop" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kExtensionsOnTop, value.GetBool());
  } else if (key == "compactAddressBar" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kCompactAddressBar, value.GetBool());
  } else if (key == "tintFromWebsite" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kTintFromWebsite, value.GetBool());
  } else if (key == "windowsControlsStyle" && value.is_string()) {
    prefs->SetString(
        starry::prefs::kWindowsControlsStyle,
        value.GetString() == starry::prefs::kWindowsControlsStyleRail
            ? starry::prefs::kWindowsControlsStyleRail
            : starry::prefs::kWindowsControlsStyleGlass);
  } else if (key == "windowsControlsAutoHide" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kWindowsControlsAutoHide, value.GetBool());
  } else if (key == "mediaControlsEnabled" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kMediaControlsEnabled, value.GetBool());
  } else if (key == "mediaDocked" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kMediaDocked, value.GetBool());
  } else if (key == "mediaCorner" && value.is_int() && value.GetInt() >= 0 &&
             value.GetInt() <= 3) {
    prefs->SetInteger(starry::prefs::kMediaCorner, value.GetInt());
  } else if (key == "mediaAutoPip" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kMediaAutoPip, value.GetBool());
  } else if (key == "mediaTabMeters" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kMediaTabMeters, value.GetBool());
  } else if (key == "pinnedPeek" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kPinnedPeek, value.GetBool());
  } else if (key == "tabPreviewsEnabled" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kTabPreviewsEnabled, value.GetBool());
  } else if (key == "treeTabs" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kTreeTabs, value.GetBool());
  } else if (key == "treeTabsMaxDepth" && value.is_int() &&
             (value.GetInt() == 0 ||
              (value.GetInt() >= 2 && value.GetInt() <= 8))) {
    prefs->SetInteger(starry::prefs::kTreeTabsMaxDepth, value.GetInt());
  } else if (key == "treeTabsCloseParent" && value.is_int() &&
             value.GetInt() >= 0 && value.GetInt() <= 2) {
    prefs->SetInteger(starry::prefs::kTreeTabsCloseParent, value.GetInt());
  } else if (key == "treeTabsAutoCollapse" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kTreeTabsAutoCollapse, value.GetBool());
  } else if (key == "tabPreviewShowsUsage" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kTabPreviewShowsUsage, value.GetBool());
  } else if (key == "sidebarWidth" && value.is_int()) {
    // 默认宽度按未自定义处理（存 0）。
    const int width = std::clamp(value.GetInt(), metrics::kSidebarMinWidth,
                                 metrics::kSidebarMaxWidth);
    prefs->SetInteger(starry::prefs::kSidebarWidth,
                      width == metrics::kSidebarWidth ? 0 : width);
  } else if (key == "utilityButtonHidden" && value.is_dict()) {
    const std::string* id = value.GetDict().FindString("id");
    const std::optional<bool> hidden = value.GetDict().FindBool("hidden");
    if (!id || !hidden) {
      return;
    }
    ScopedListPrefUpdate update(prefs, starry::prefs::kHiddenUtilityButtons);
    base::Value entry(*id);
    update->EraseValue(entry);
    if (*hidden) {
      update->Append(std::move(entry));
    }
  } else if (key == "newTabStyle" && value.is_string()) {
    const std::string& style = value.GetString();
    if (style == starry::prefs::kNewTabStyleMinimal ||
        style == starry::prefs::kNewTabStyleCard ||
        style == starry::prefs::kNewTabStyleOverlay) {
      prefs->SetString(starry::prefs::kNewTabStyle, style);
    }
  } else if (key == "newTabBackground" && value.is_string()) {
    // 自己的图片在新标签页上选（要弹文件选择），这里只换这三种。
    const std::string& background = value.GetString();
    if (background == "theme" || background == "sky" || background == "solid") {
      prefs->SetString(starry::prefs::kNewTabBackground, background);
    }
  } else if (key == "newTabShortcutRows" && value.is_int()) {
    prefs->SetInteger(starry::prefs::kNewTabShortcutRows,
                      std::clamp(value.GetInt(), 1, 2));
  } else if (key == "newTabWidget" && value.is_dict()) {
    const std::string* id = value.GetDict().FindString("id");
    const std::optional<bool> on = value.GetDict().FindBool("on");
    if (!id || !on || !new_tab::IsWidgetId(*id)) {
      return;
    }
    ScopedListPrefUpdate update(prefs, starry::prefs::kNewTabWidgets);
    base::Value entry(*id);
    update->EraseValue(entry);
    if (*on) {
      update->Append(std::move(entry));
    }
  } else if (key == "memorySaver" && value.is_bool()) {
    local->SetInteger(
        perf_prefs::kMemorySaverModeState,
        static_cast<int>(value.GetBool()
                             ? perf_prefs::MemorySaverModeState::kEnabled
                             : perf_prefs::MemorySaverModeState::kDisabled));
  } else if (key == "memorySaverAggressiveness" && value.is_int()) {
    local->SetInteger(perf_prefs::kMemorySaverModeAggressiveness,
                      std::clamp(value.GetInt(), 0, 2));
  } else if (key == "preload" && value.is_int()) {
    prefs->SetInteger(::prefs::kNetworkPredictionOptions, value.GetInt());
  } else if (key == "askWhereToSave" && value.is_bool()) {
    prefs->SetBoolean(::prefs::kPromptForDownload, value.GetBool());
  } else if (key == "downloadMultiConnection" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kDownloadMultiConnection, value.GetBool());
  } else if (key == "downloadAutoRetry" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kDownloadAutoRetry, value.GetBool());
  } else if (key == "downloadResumeOnLaunch" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kDownloadResumeOnLaunch, value.GetBool());
  } else if (key == "downloadNotifyOnCompletion" && value.is_bool()) {
    prefs->SetBoolean(starry::prefs::kDownloadNotifyOnCompletion,
                      value.GetBool());
  } else if (key == "downloadConnections" && value.is_int()) {
    prefs->SetInteger(starry::prefs::kDownloadConnections,
                      std::clamp(value.GetInt(), 1, 16));
  } else if (key == "downloadMaxConcurrent" && value.is_int()) {
    prefs->SetInteger(starry::prefs::kDownloadMaxConcurrent,
                      std::clamp(value.GetInt(), 1, 10));
  } else if (key == "downloadSpeedLimit" && value.is_int()) {
    prefs->SetInteger(starry::prefs::kDownloadSpeedLimit,
                      std::max(0, value.GetInt()));
  } else if (key == "fingerprintProtection" && value.is_string() &&
             IsFingerprintProtectionMode(value.GetString())) {
    prefs->SetString(starry::prefs::kFingerprintProtection, value.GetString());
  } else if (key == "locationSimulation" && value.is_string() &&
             IsLocationSimulationMode(value.GetString())) {
    prefs->SetString(starry::prefs::kLocationSimulation, value.GetString());
  } else if (key == "cookies" && value.is_int()) {
    prefs->SetInteger(::prefs::kCookieControlsMode, value.GetInt());
  } else if (key == "doNotTrack" && value.is_bool()) {
    prefs->SetBoolean(::prefs::kEnableDoNotTrack, value.GetBool());
  }
}

void SettingsHandler::HandleSetFingerprintException(
    const base::ListValue& args) {
  if (args.size() != 2 || !args[0].is_string() || !args[1].is_bool()) {
    return;
  }
  SetFingerprintProtectionException(profile_, args[0].GetString(),
                                    args[1].GetBool());
}

void SettingsHandler::HandleSetSiteLocation(const base::ListValue& args) {
  if (args.size() != 2 || !args[0].is_string()) {
    return;
  }
  SiteLocationRule rule;
  if (args[1].is_dict()) {
    const std::string* mode = args[1].GetDict().FindString("mode");
    if (!mode || *mode != "real") {
      return;
    }
    rule.mode = SiteLocationMode::kReal;
  } else if (!args[1].is_none()) {
    return;
  }
  SetSiteLocationRule(profile_, args[0].GetString(), rule);
}

void SettingsHandler::HandlePickLocation(const base::ListValue& args) {
  AllowJavascript();
  if (args.size() != 2 || !args[0].is_string() || !args[1].is_string()) {
    return;
  }
  const std::string callback_id = args[0].GetString();
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  const std::string& site = args[1].GetString();
  ShowLocationPicker(
      tab ? tab->GetBrowserWindowInterface() : nullptr,
      site.empty() ? std::nullopt : std::optional(site),
      base::BindOnce(
          [](base::WeakPtr<SettingsHandler> handler, std::string callback_id,
             bool saved) {
            if (handler && handler->IsJavascriptAllowed()) {
              handler->ResolveJavascriptCallback(
                  base::Value(callback_id), base::Value(saved));
            }
          },
          weak_factory_.GetWeakPtr(), callback_id));
}

// setOrganizerException(site, add)：加上、拿掉一个不整理的网站（写成网址的取主机名，
// 小写、去掉开头的 www.）。
void SettingsHandler::HandleSetOrganizerException(
    const base::ListValue& args) {
  if (args.size() != 2 || !args[0].is_string() || !args[1].is_bool()) {
    return;
  }
  std::string site(
      base::TrimWhitespaceASCII(args[0].GetString(), base::TRIM_ALL));
  if (site.find("://") != std::string::npos) {
    site = GURL(site).host();
  }
  site = base::ToLowerASCII(site);
  if (base::StartsWith(site, "www.")) {
    site = site.substr(4);
  }
  if (site.empty()) {
    return;
  }
  PrefService* prefs = profile_->GetPrefs();
  base::ListValue sites = prefs->GetList(starry::prefs::kOrganizerExceptions)
                              .Clone();
  sites.EraseValue(base::Value(site));
  if (args[1].GetBool()) {
    sites.Append(site);
  }
  prefs->SetList(starry::prefs::kOrganizerExceptions, std::move(sites));
}

// setOrganizerRule(site, folder)：加一条按网站的规则（folder 是空的：拿掉这个网站的规则）。
void SettingsHandler::HandleSetOrganizerRule(const base::ListValue& args) {
  if (args.size() != 2 || !args[0].is_string() || !args[1].is_string() ||
      profile_->IsOffTheRecord()) {
    return;
  }
  if (args[1].GetString().empty()) {
    tab_organizer::RemoveRule(profile_, args[0].GetString());
  } else {
    tab_organizer::AddRule(profile_, args[0].GetString(), args[1].GetString(),
                           /*learned=*/false);
  }
}

void SettingsHandler::HandleForgetOrganizerMemory(const base::ListValue& args) {
  if (!profile_->IsOffTheRecord()) {
    tab_organizer::Forget(profile_);
  }
}

void SettingsHandler::HandleClearArchive(const base::ListValue& args) {
  if (!profile_->IsOffTheRecord()) {
    tab_organizer::TabArchive::ForProfile(profile_)->Clear();
  }
}

void SettingsHandler::HandleOpenArchive(const base::ListValue& args) {
  // 整页的「已归档」：在设置页所在的窗口里打开（已经开着就切过去）。
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser) {
    browser =
        ProfileBrowserCollection::GetForProfile(profile_)->FindTabbedBrowser();
  }
  if (browser) {
    OpenStarryArchive(browser);
  }
}

void SettingsHandler::HandleRelaunch(const base::ListValue& args) {
  // 同 Chrome 设置页的「重新启动」：恢复窗口和标签页。
  chrome::AttemptRestart();
}

void SettingsHandler::HandleStartOnboarding(const base::ListValue& args) {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  StarryBrowserView* view =
      tab && tab->GetBrowserWindowInterface()
          ? StarryBrowserView::From(tab->GetBrowserWindowInterface())
          : nullptr;
  if (view) {
    view->StartOnboarding();
  }
}

void SettingsHandler::HandleCheckDefaultBrowser(const base::ListValue& args) {
  AllowJavascript();
  default_browser_worker_ =
      base::MakeRefCounted<shell_integration::DefaultBrowserWorker>();
  default_browser_worker_->StartCheckIsDefault(
      base::BindOnce(&SettingsHandler::OnDefaultBrowserState,
                     weak_factory_.GetWeakPtr()));
}

void SettingsHandler::HandleSetDefaultBrowser(const base::ListValue& args) {
  AllowJavascript();
  default_browser_ = "checking";
  ScheduleChanged();
  default_browser_worker_ =
      base::MakeRefCounted<shell_integration::DefaultBrowserWorker>();
  default_browser_worker_->StartSetAsDefault(
      base::BindOnce(&SettingsHandler::OnDefaultBrowserState,
                     weak_factory_.GetWeakPtr()));
}

void SettingsHandler::OnDefaultBrowserState(
    shell_integration::DefaultWebClientState state) {
  default_browser_ = state == shell_integration::IS_DEFAULT ? "default"
                     : state == shell_integration::NOT_DEFAULT ||
                             state == shell_integration::OTHER_MODE_IS_DEFAULT
                         ? "notDefault"
                         : "unknown";
  ScheduleChanged();
}

void SettingsHandler::HandleChooseDownloadFolder(const base::ListValue& args) {
  content::WebContents* contents = web_ui()->GetWebContents();
  if (select_folder_dialog_) {
    return;
  }
  select_folder_dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(contents));
  select_folder_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_FOLDER,
      L("settings.downloads.location.choose"),
      profile_->GetPrefs()->GetFilePath(::prefs::kDownloadDefaultDirectory),
      nullptr, 0, base::FilePath::StringType(),
      contents->GetTopLevelNativeWindow());
}

void SettingsHandler::FileSelected(const ui::SelectedFileInfo& file,
                                   int index) {
  select_folder_dialog_ = nullptr;
  // 同 Chrome 的设置页：下载和「另存为」都用这个文件夹。
  PrefService* prefs = profile_->GetPrefs();
  prefs->SetFilePath(::prefs::kDownloadDefaultDirectory, file.path());
  prefs->SetFilePath(::prefs::kSaveFileDefaultDirectory, file.path());
}

void SettingsHandler::FileSelectionCanceled() {
  select_folder_dialog_ = nullptr;
}

}  // namespace starry
