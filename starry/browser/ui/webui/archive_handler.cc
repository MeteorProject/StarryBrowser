// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/archive_handler.h"

#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/tab_organizer/tab_organizer.h"
#include "starry/browser/ui/webui/starry_pages.h"

namespace starry {

namespace {

using tab_organizer::ArchivedTab;
using tab_organizer::ArchiveReason;
using tab_organizer::TabArchive;

constexpr base::TimeDelta kChangedDelay = base::Milliseconds(120);

const char* ReasonName(ArchiveReason reason) {
  switch (reason) {
    case ArchiveReason::kIdle:
      return "idle";
    case ArchiveReason::kDuplicate:
      return "duplicate";
    case ArchiveReason::kManual:
      return "manual";
    case ArchiveReason::kAssistant:
      return "assistant";
  }
  return "idle";
}

double TimeValue(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

std::vector<std::string> IdsFrom(const base::ListValue& args) {
  std::vector<std::string> ids;
  if (args.empty() || !args[0].is_list()) {
    return ids;
  }
  for (const base::Value& id : args[0].GetList()) {
    if (id.is_string()) {
      ids.push_back(id.GetString());
    }
  }
  return ids;
}

}  // namespace

ArchiveHandler::ArchiveHandler(Profile* profile)
    : profile_(profile->GetOriginalProfile()) {}

ArchiveHandler::~ArchiveHandler() = default;

void ArchiveHandler::RegisterMessages() {
  const auto add = [this](const char* name, void (ArchiveHandler::*method)(
                                                const base::ListValue&)) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getArchive", &ArchiveHandler::HandleGetArchive);
  add("restoreArchived", &ArchiveHandler::HandleRestore);
  add("removeArchived", &ArchiveHandler::HandleRemove);
  add("clearArchive", &ArchiveHandler::HandleClear);
  add("openTabSettings", &ArchiveHandler::HandleOpenSettings);
}

void ArchiveHandler::OnJavascriptAllowed() {
  observation_.Observe(TabArchive::ForProfile(profile_));
  prefs_.Init(profile_->GetPrefs());
  for (const char* pref :
       {starry::prefs::kAutoArchive, starry::prefs::kAutoArchiveHours,
        starry::prefs::kArchiveDuplicates,
        starry::prefs::kArchiveRetentionDays}) {
    prefs_.Add(pref, base::BindRepeating(&ArchiveHandler::ScheduleChanged,
                                         base::Unretained(this)));
  }
}

void ArchiveHandler::OnJavascriptDisallowed() {
  observation_.Reset();
  prefs_.RemoveAll();
  changed_timer_.Stop();
}

void ArchiveHandler::OnArchiveChanged() {
  ScheduleChanged();
}

base::DictValue ArchiveHandler::Describe() {
  base::ListValue tabs;
  for (const ArchivedTab& tab : TabArchive::ForProfile(profile_)->tabs()) {
    tabs.Append(base::DictValue()
                    .Set("id", tab.id)
                    .Set("url", tab.url.spec())
                    .Set("title", tab.title)
                    .Set("host", tab.url.host())
                    .Set("archived", TimeValue(tab.archived))
                    .Set("lastActive", TimeValue(tab.last_active))
                    .Set("reason", ReasonName(tab.reason))
                    .Set("batch", tab.batch)
                    .Set("thumbnail", tab.thumbnail)
                    .Set("topic", tab.topic));
  }
  const tab_organizer::Settings settings =
      tab_organizer::ReadSettings(profile_);
  return base::DictValue()
      .Set("tabs", std::move(tabs))
      .Set("loaded", TabArchive::ForProfile(profile_)->loaded())
      .Set("autoArchive", settings.auto_archive)
      .Set("idleHours", static_cast<int>(settings.idle.InHours()))
      .Set("duplicates", settings.duplicates)
      .Set("retentionDays", static_cast<int>(settings.retention.InDays()))
      .Set("incognito", Profile::FromWebUI(web_ui())->IsOffTheRecord());
}

void ArchiveHandler::ScheduleChanged() {
  if (!IsJavascriptAllowed() || changed_timer_.IsRunning()) {
    return;
  }
  changed_timer_.Start(
      FROM_HERE, kChangedDelay,
      base::BindOnce(&ArchiveHandler::SendChanged, base::Unretained(this)));
}

void ArchiveHandler::SendChanged() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("archive-changed", Describe());
  }
}

BrowserWindowInterface* ArchiveHandler::TargetBrowser() {
  content::WebContents* contents = web_ui()->GetWebContents();
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (browser && !browser->GetProfile()->IsOffTheRecord() &&
      browser->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
    return browser;
  }
  return ProfileBrowserCollection::GetForProfile(profile_)->FindTabbedBrowser();
}

void ArchiveHandler::HandleGetArchive(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(Describe()));
}

void ArchiveHandler::HandleRestore(const base::ListValue& args) {
  BrowserWindowInterface* browser = TargetBrowser();
  if (!browser) {
    return;
  }
  const std::vector<std::string> ids = IdsFrom(args);
  const bool select = args.size() > 1 && args[1].is_bool() && args[1].GetBool();
  tab_organizer::TabOrganizer* organizer =
      tab_organizer::TabOrganizer::ForProfile(profile_);
  for (size_t i = 0; i < ids.size(); ++i) {
    organizer->Restore(browser, ids[i], select && i + 1 == ids.size());
  }
}

void ArchiveHandler::HandleRemove(const base::ListValue& args) {
  TabArchive::ForProfile(profile_)->Remove(IdsFrom(args));
}

void ArchiveHandler::HandleClear(const base::ListValue& args) {
  TabArchive::ForProfile(profile_)->Clear();
}

void ArchiveHandler::HandleOpenSettings(const base::ListValue& args) {
  content::WebContents* contents = web_ui()->GetWebContents();
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (BrowserWindowInterface* browser =
          tab ? tab->GetBrowserWindowInterface() : TargetBrowser()) {
    OpenStarrySettings(browser, "tabs");
  }
}

}  // namespace starry
