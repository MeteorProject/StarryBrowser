// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/internal_pages.h"

#include "chrome/common/webui_url_constants.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/common/url_constants.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/common/internal_urls.h"
#include "url/gurl.h"

namespace starry {

InternalPage InternalPageForUrl(const GURL& visible_url) {
  const GURL url = ToChromeURL(visible_url);
  if (!url.SchemeIs(content::kChromeUIScheme)) {
    return InternalPage::kNone;
  }
  const std::string_view host = url.host();
  if (host == chrome::kChromeUIHistoryHost) {
    return InternalPage::kHistory;
  }
  if (host == chrome::kChromeUIBookmarksHost) {
    return InternalPage::kBookmarks;
  }
  if (host == chrome::kChromeUIDownloadsHost) {
    return InternalPage::kDownloads;
  }
  // chrome://extensions 改写成设置页的「扩展」分类（starry_content_browser_client.cc）。
  if (host == chrome::kChromeUIExtensionsHost ||
      (host == "starry-settings" && url.path().starts_with("/extensions"))) {
    return InternalPage::kExtensions;
  }
  if (host == chrome::kChromeUISettingsHost || host == "starry-settings") {
    return InternalPage::kSettings;
  }
  if (host == "archive") {
    return InternalPage::kArchive;
  }
  return InternalPage::kNone;
}

const gfx::VectorIcon* InternalPageIcon(InternalPage page) {
  switch (page) {
    case InternalPage::kHistory:
      return &icons::kClock;
    case InternalPage::kBookmarks:
      return &icons::kStar;
    case InternalPage::kDownloads:
      return &icons::kArrowDownCircle;
    case InternalPage::kExtensions:
      return &icons::kPuzzlepiece;
    case InternalPage::kSettings:
      // 使用内置齿轮图标（与底部设置按钮一致）。
      return &vector_icons::kSettingsIcon;
    case InternalPage::kArchive:
      return &icons::kArchivebox;
    case InternalPage::kNone:
      return nullptr;
  }
  return nullptr;
}

bool IsBlankTabUrl(const GURL& url) {
  return url.is_empty() || url.IsAboutBlank() ||
         (IsInternalURL(url) &&
          (url.host() == chrome::kChromeUINewTabHost ||
           url.host() == chrome::kChromeUINewTabPageHost));
}

}  // namespace starry
