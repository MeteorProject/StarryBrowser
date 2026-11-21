// Copyright 2026 The Starry Browser Authors

#include "starry/browser/new_tab/new_tab_data.h"

#include <algorithm>
#include <utility>

#include "chrome/browser/ntp_tiles/chrome_most_visited_sites_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "chrome/browser/ui/browser_live_tab_context.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/ntp_tiles/section_type.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/sessions/core/tab_restore_types.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/tab_organizer/tab_archive.h"

namespace starry::new_tab {

namespace {

void Describe(const sessions::tab_restore::Tab& tab, ClosedEntry& entry) {
  if (tab.navigations.empty()) {
    return;
  }
  const sessions::SerializedNavigationEntry& navigation =
      tab.navigations[tab.normalized_navigation_index()];
  entry.url = navigation.virtual_url();
  entry.title = navigation.title();
}

}  // namespace

Shortcuts::Shortcuts(Profile* profile,
                     size_t max,
                     base::RepeatingClosure changed)
    : changed_(std::move(changed)) {
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }
  sites_ = ChromeMostVisitedSitesFactory::NewForProfile(profile);
  if (!sites_) {
    return;
  }
  sites_->EnableTileTypes(ntp_tiles::MostVisitedSites::EnableTileTypesOptions()
                              .with_top_sites(true)
                              .with_custom_links(true));
  sites_->SetShortcutsVisible(true);
  sites_->AddMostVisitedURLsObserver(this, max);
}

Shortcuts::~Shortcuts() {
  if (sites_) {
    sites_->RemoveMostVisitedURLsObserver(this);
  }
}

bool Shortcuts::customized() const {
  return sites_ && sites_->IsCustomLinksInitialized();
}

bool Shortcuts::Add(const GURL& url, const std::u16string& title) {
  if (!sites_ || !url.is_valid()) {
    return false;
  }
  return sites_->AddCustomLink(url, title);
}

bool Shortcuts::Rename(const GURL& url, const std::u16string& title) {
  return sites_ && sites_->UpdateCustomLink(url, url, title);
}

bool Shortcuts::Remove(const GURL& url) {
  if (!sites_) {
    return false;
  }
  if (sites_->IsCustomLinksEnabled() && sites_->DeleteCustomLink(url)) {
    return true;
  }
  // Custom links 不可用时退回屏蔽常去站点。
  sites_->AddOrRemoveBlockedUrl(url, /*add_url=*/true);
  return true;
}

void Shortcuts::Reset() {
  if (!sites_) {
    return;
  }
  sites_->UninitializeCustomLinks();
  sites_->ClearBlockedUrls();
}

void Shortcuts::OnURLsAvailable(
    bool is_user_triggered,
    const std::map<ntp_tiles::SectionType, ntp_tiles::NTPTilesVector>&
        sections) {
  tiles_.clear();
  auto personal = sections.find(ntp_tiles::SectionType::PERSONALIZED);
  if (personal != sections.end()) {
    for (const ntp_tiles::NTPTile& tile : personal->second) {
      tiles_.push_back({tile.url, tile.title});
    }
  }
  if (changed_) {
    changed_.Run();
  }
}

void Shortcuts::OnIconMadeAvailable(const GURL& site_url) {
  if (changed_) {
    changed_.Run();
  }
}

std::vector<ClosedEntry> RecentlyClosed(Profile* profile, size_t max) {
  std::vector<ClosedEntry> result;
  if (!profile || profile->IsOffTheRecord()) {
    return result;
  }
  sessions::TabRestoreService* service =
      TabRestoreServiceFactory::GetForProfile(profile);
  if (!service) {
    return result;
  }
  if (!service->IsLoaded()) {
    service->LoadTabsFromLastSession();
  }
  for (const std::unique_ptr<sessions::tab_restore::Entry>& entry :
       service->entries()) {
    if (result.size() >= max) {
      break;
    }
    ClosedEntry item;
    item.id = entry->id;
    item.closed = entry->timestamp;
    switch (entry->type) {
      case sessions::tab_restore::Type::TAB:
        Describe(static_cast<const sessions::tab_restore::Tab&>(*entry), item);
        break;
      case sessions::tab_restore::Type::WINDOW: {
        const auto& window =
            static_cast<const sessions::tab_restore::Window&>(*entry);
        if (window.tabs.empty()) {
          continue;
        }
        const size_t selected = static_cast<size_t>(
            std::clamp(window.selected_tab_index, 0,
                       static_cast<int>(window.tabs.size()) - 1));
        Describe(*window.tabs[selected], item);
        item.window = true;
        item.tab_count = static_cast<int>(window.tabs.size());
        break;
      }
      case sessions::tab_restore::Type::GROUP: {
        const auto& group =
            static_cast<const sessions::tab_restore::Group&>(*entry);
        if (group.tabs.empty()) {
          continue;
        }
        Describe(*group.tabs.front(), item);
        item.tab_count = static_cast<int>(group.tabs.size());
        break;
      }
      case sessions::tab_restore::Type::SPLIT: {
        const auto& split =
            static_cast<const sessions::tab_restore::Split&>(*entry);
        if (split.tabs.empty()) {
          continue;
        }
        Describe(*split.tabs.front(), item);
        item.tab_count = static_cast<int>(split.tabs.size());
        break;
      }
    }
    if (!item.url.is_valid()) {
      continue;
    }
    result.push_back(std::move(item));
  }
  return result;
}

void RestoreClosed(BrowserWindowInterface* browser,
                   SessionID id,
                   WindowOpenDisposition disposition) {
  if (!browser) {
    return;
  }
  sessions::TabRestoreService* service =
      TabRestoreServiceFactory::GetForProfile(browser->GetProfile());
  if (!service) {
    return;
  }
  service->RestoreEntryById(BrowserLiveTabContext::From(browser), id,
                            disposition);
}

ArchiveSummary SummarizeArchive(Profile* profile, size_t recent) {
  ArchiveSummary summary;
  if (!profile || profile->IsOffTheRecord()) {
    return summary;
  }
  const base::Time now = base::Time::Now();
  const base::Time midnight = now.LocalMidnight();
  const base::Time week_ago = now - base::Days(7);
  for (const tab_organizer::ArchivedTab& tab :
       tab_organizer::TabArchive::ForProfile(profile)->tabs()) {
    ++summary.total;
    if (tab.archived >= midnight) {
      ++summary.today;
    }
    if (tab.archived >= week_ago) {
      ++summary.week;
    }
    if (summary.recent.size() < recent) {
      summary.recent.emplace_back(tab.url, tab.title);
    }
  }
  return summary;
}

std::u16string TimeAgo(base::Time time) {
  const base::TimeDelta delta = base::Time::Now() - time;
  if (time.is_null() || delta < base::Minutes(1)) {
    return L("newTab.ago.now");
  }
  if (delta < base::Hours(1)) {
    return L("newTab.ago.minutes",
             {{"count", static_cast<int>(delta.InMinutes())}});
  }
  if (delta < base::Days(1)) {
    return L("newTab.ago.hours",
             {{"count", static_cast<int>(delta.InHours())}});
  }
  return L("newTab.ago.days", {{"count", static_cast<int>(delta.InDays())}});
}

}  // namespace starry::new_tab
