// Copyright 2026 The Starry Browser Authors

#include "starry/browser/starry_hooks.h"

#include "content/public/browser/navigation_handle.h"

namespace starry {

bool ShouldRecordHistoryNavigation(content::NavigationHandle* navigation) {
  // replaceState updates the current page (e.g. a Discourse topic's reading
  // position). Chromium records these updates whenever the URL changes, filling
  // history with visits to the same page. Keep the original visit instead.
  // pushState and cross-document replacements still represent navigations.
  return !navigation->IsSameDocument() || !navigation->DidReplaceEntry();
}

}  // namespace starry
