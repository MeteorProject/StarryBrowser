// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/frame/chrome_layout.h"

#include <string>

#include "components/prefs/pref_service.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/design/metrics.h"

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/devtools.h"
#endif

namespace starry {

// static
TabLayout ChromeLayout::TabLayoutFromPrefs(const PrefService* prefs) {
#if defined(STARRY_DEVTOOLS)
  const std::string forced = devtools::ForcedSidebarLayout();
  if (!forced.empty()) {
    if (forced == prefs::kSidebarLayoutTop) {
      return TabLayout::kTop;
    }
    if (forced == prefs::kSidebarLayoutFloating || forced == "autohide") {
      return TabLayout::kFloatingSidebar;
    }
    return TabLayout::kDockedSidebar;
  }
#endif
  const std::string value = prefs->GetString(prefs::kSidebarLayout);
  if (value == prefs::kSidebarLayoutTop) {
    return TabLayout::kTop;
  }
  if (value == prefs::kSidebarLayoutFloating ||
      prefs->GetBoolean(prefs::kSidebarAutoHide)) {
    return TabLayout::kFloatingSidebar;
  }
  return TabLayout::kDockedSidebar;
}

// static
ChromeLayout ChromeLayout::FromPrefs(const PrefService* prefs,
                                     bool has_extensions) {
  ChromeLayout layout;
  layout.tab_layout = TabLayoutFromPrefs(prefs);
  const bool sidebar = layout.is_sidebar();
  layout.auto_hide = layout.floating();
  layout.address_in_toolbar =
      !sidebar || prefs->GetBoolean(prefs::kAddressBarOnTop);
  layout.extensions_in_toolbar =
      (!sidebar || prefs->GetBoolean(prefs::kExtensionsOnTop)) &&
      has_extensions;
  layout.toolbar_navigation =
      layout.address_in_toolbar &&
      layout.tab_layout != TabLayout::kDockedSidebar;
  layout.compact =
      prefs->GetBoolean(prefs::kCompactAddressBar) && layout.address_in_toolbar;
  return layout;
}

int ChromeLayout::content_top_inset() const {
  if (toolbar()) {
    return 0;
  }
  return tab_strip() ? metrics::kContentInset / 4 : metrics::kContentInset;
}

ChromeLayout::AssistantPlacement ChromeLayout::assistant_placement() const {
  if (tab_strip()) {
    return AssistantPlacement::kTabStrip;
  }
  return toolbar() || compact ? AssistantPlacement::kToolbar
                              : AssistantPlacement::kSidebar;
}

int ChromeLayout::content_leading() const {
  return sidebar_column() && !floating() ? 0 : metrics::kContentInset;
}

int ChromeLayout::toolbar_top_inset() const {
  return sidebar_column() && floating() ? metrics::kFloatingSidebarInset : 0;
}

int ChromeLayout::SidebarHeaderHeight(int panel_inset) const {
  if (!compact) {
    return metrics::kSidebarHeaderHeight;
  }
  return (metrics::kContentInset - panel_inset) * 2 +
         metrics::kCompactToolbarHeight;
}

std::optional<gfx::Point> ChromeLayout::WindowButtonsOrigin(
    bool peeking) const {
  const int button = metrics::kWindowButtonHeight;
  if (tab_strip()) {
    return gfx::Point(metrics::kWindowButtonsLeading,
                      (metrics::kTabStripHeight - button) / 2);
  }
  if (auto_hide && !peeking) {
    return std::nullopt;
  }
  const int inset =
      floating() || auto_hide ? metrics::kFloatingSidebarInset : 0;
  return gfx::Point(inset + metrics::kWindowButtonsLeading,
                    inset + (SidebarHeaderHeight(inset) - button) / 2);
}

}  // namespace starry
