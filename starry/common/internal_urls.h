// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_INTERNAL_URLS_H_
#define STARRY_COMMON_INTERNAL_URLS_H_

#include "content/public/common/url_constants.h"
#include "url/gurl.h"

namespace starry {

inline constexpr char kUIScheme[] = "starry";
inline constexpr char kUIUntrustedScheme[] = "starry-untrusted";

// WebUI keeps Chromium's security origins and resource URLs. Navigation
// entries, copied addresses, bookmarks and restored tabs use the public Starry
// addresses.
inline GURL ToStarryURL(const GURL& url) {
  if (url.SchemeIs(content::kChromeUIUntrustedScheme)) {
    GURL::Replacements replacements;
    replacements.SetSchemeStr(kUIUntrustedScheme);
    return url.ReplaceComponents(replacements);
  }
  if (!url.SchemeIs(content::kChromeUIScheme)) {
    return url;
  }
  GURL::Replacements replacements;
  replacements.SetSchemeStr(kUIScheme);
  if (url.host() == "starry-settings") {
    replacements.SetHostStr("settings");
  } else if (url.host() == "settings") {
    replacements.SetHostStr("browser-settings");
  } else if (url.host() == "starry-assistant") {
    replacements.SetHostStr("assistant");
  } else if (url.host() == "chrome-urls") {
    replacements.SetHostStr("starry-urls");
  }
  return url.ReplaceComponents(replacements);
}

inline GURL ToChromeURL(const GURL& url) {
  if (url.SchemeIs(kUIUntrustedScheme)) {
    GURL::Replacements replacements;
    replacements.SetSchemeStr(content::kChromeUIUntrustedScheme);
    return url.ReplaceComponents(replacements);
  }
  if (!url.SchemeIs(kUIScheme)) {
    return url;
  }
  GURL::Replacements replacements;
  replacements.SetSchemeStr(content::kChromeUIScheme);
  if (url.host() == "settings") {
    replacements.SetHostStr("starry-settings");
  } else if (url.host() == "browser-settings") {
    replacements.SetHostStr("settings");
  } else if (url.host() == "assistant") {
    replacements.SetHostStr("starry-assistant");
  } else if (url.host() == "starry-urls") {
    replacements.SetHostStr("chrome-urls");
  }
  return url.ReplaceComponents(replacements);
}

inline bool IsInternalURL(const GURL& url) {
  return url.SchemeIs(kUIScheme) || url.SchemeIs(kUIUntrustedScheme) ||
         url.SchemeIs(content::kChromeUIScheme) ||
         url.SchemeIs(content::kChromeUIUntrustedScheme);
}

}  // namespace starry

#endif  // STARRY_COMMON_INTERNAL_URLS_H_
