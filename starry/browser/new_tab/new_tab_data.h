// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_NEW_TAB_NEW_TAB_DATA_H_
#define STARRY_BROWSER_NEW_TAB_NEW_TAB_DATA_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "components/ntp_tiles/most_visited_sites.h"
#include "components/sessions/core/session_id.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

class BrowserWindowInterface;
class Profile;

// 新标签页与全屏浮层首页共用的数据源。
namespace starry::new_tab {

// 基于 ntp_tiles::MostVisitedSites；编辑后切换为自定义链接，可 Reset 恢复为常去网站。
class Shortcuts : public ntp_tiles::MostVisitedSites::Observer {
 public:
  struct Tile {
    GURL url;
    std::u16string title;
  };

  Shortcuts(Profile* profile, size_t max, base::RepeatingClosure changed);
  Shortcuts(const Shortcuts&) = delete;
  Shortcuts& operator=(const Shortcuts&) = delete;
  ~Shortcuts() override;

  const std::vector<Tile>& tiles() const { return tiles_; }
  bool customized() const;
  bool Add(const GURL& url, const std::u16string& title);
  bool Rename(const GURL& url, const std::u16string& title);
  bool Remove(const GURL& url);
  void Reset();

  // ntp_tiles::MostVisitedSites::Observer:
  void OnURLsAvailable(
      bool is_user_triggered,
      const std::map<ntp_tiles::SectionType, ntp_tiles::NTPTilesVector>&
          sections) override;
  void OnIconMadeAvailable(const GURL& site_url) override;

 private:
  std::unique_ptr<ntp_tiles::MostVisitedSites> sites_;
  std::vector<Tile> tiles_;
  base::RepeatingClosure changed_;
};

struct ClosedEntry {
  SessionID id = SessionID::InvalidValue();
  GURL url;
  std::u16string title;
  base::Time closed;
  int tab_count = 1;
  bool window = false;
};
std::vector<ClosedEntry> RecentlyClosed(Profile* profile, size_t max);
void RestoreClosed(BrowserWindowInterface* browser,
                   SessionID id,
                   WindowOpenDisposition disposition);

struct ArchiveSummary {
  int today = 0;
  int week = 0;
  int total = 0;
  std::vector<std::pair<GURL, std::u16string>> recent;
};
ArchiveSummary SummarizeArchive(Profile* profile, size_t recent);

std::u16string TimeAgo(base::Time time);

}  // namespace starry::new_tab

#endif  // STARRY_BROWSER_NEW_TAB_NEW_TAB_DATA_H_
