// Copyright 2026 The Starry Browser Authors

#include "starry/browser/new_tab/new_tab.h"

#include <algorithm>
#include <cmath>

#include "base/strings/strcat.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/common/internal_urls.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace starry::new_tab {

namespace {

constexpr int kSearchHeight = 56;
constexpr int kSearchMaxWidth = 640;
constexpr const char* kWidgetIds[] = {
    "clock", "shortcuts", "continue", "playing", "archive", "shield", "note"};

GURL PageUrl() {
  return GURL(base::StrCat(
      {content::kChromeUIScheme, url::kStandardSchemeSeparator, kHost, "/"}));
}

bool IsChromeHost(const GURL& url, std::string_view host) {
  return url.SchemeIs(content::kChromeUIScheme) && url.host() == host;
}

}  // namespace

Style GetStyle(Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return Style::kMinimal;
  }
  const std::string& style =
      profile->GetPrefs()->GetString(prefs::kNewTabStyle);
  if (style == prefs::kNewTabStyleCard) {
    return Style::kCard;
  }
  if (style == prefs::kNewTabStyleOverlay) {
    return Style::kOverlay;
  }
  return Style::kMinimal;
}

bool IsNewTabPage(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  if (IsChromeHost(contents->GetLastCommittedURL(), kHost)) {
    return true;
  }
  content::NavigationEntry* pending =
      contents->GetController().GetPendingEntry();
  if (pending && IsChromeHost(pending->GetURL(), kHost)) {
    return true;
  }
  // 导航尚未启动时只有 virtual URL；极简风格的空白页也显示 chrome://newtab，需排除。
  return GetStyle(Profile::FromBrowserContext(contents->GetBrowserContext())) !=
             Style::kMinimal &&
         IsChromeHost(ToChromeURL(contents->GetVisibleURL()),
                      chrome::kChromeUINewTabHost);
}

bool RewriteURL(GURL* url, content::BrowserContext* context) {
  const GURL chrome_url = ToChromeURL(*url);
  if (!IsChromeHost(chrome_url, chrome::kChromeUINewTabHost) &&
      !IsChromeHost(chrome_url, chrome::kChromeUINewTabPageHost) &&
      !IsChromeHost(chrome_url, kHost)) {
    return false;
  }
  Profile* profile = Profile::FromBrowserContext(context);
  if (GetStyle(profile) == Style::kMinimal) {
    *url = GURL(url::kAboutBlankURL);
  } else {
    *url = PageUrl();
  }
  return true;
}

bool ReverseURL(GURL* url, content::BrowserContext* context) {
  if (!IsChromeHost(*url, kHost)) {
    return false;
  }
  *url = GURL(chrome::kChromeUINewTabURL);
  return true;
}

gfx::Rect DefaultSearchRect(const gfx::Size& page) {
  const int width = std::clamp(page.width() - 32, 0, kSearchMaxWidth);
  const int top =
      std::clamp(static_cast<int>(std::lround(page.height() * 0.28)), 72, 168);
  return gfx::Rect((page.width() - width) / 2, top, width, kSearchHeight);
}

bool IsWidgetId(std::string_view id) {
  return std::ranges::find(kWidgetIds, id) != std::end(kWidgetIds);
}

bool WidgetEnabled(Profile* profile, std::string_view id) {
  if (!profile) {
    return false;
  }
  const base::ListValue& widgets =
      profile->GetOriginalProfile()->GetPrefs()->GetList(prefs::kNewTabWidgets);
  return std::ranges::any_of(widgets, [&](const base::Value& value) {
    return value.is_string() && value.GetString() == id;
  });
}

NewTabState::NewTabState(content::WebContents* contents)
    : content::WebContentsUserData<NewTabState>(*contents) {}

NewTabState::~NewTabState() = default;

void NewTabState::SetSearchDocked(bool docked) {
  if (search_docked_ == docked) {
    return;
  }
  search_docked_ = docked;
  if (docked_listener_) {
    docked_listener_.Run(docked);
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(NewTabState);

}  // namespace starry::new_tab
