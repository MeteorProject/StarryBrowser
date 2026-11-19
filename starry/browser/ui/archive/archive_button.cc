// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/archive/archive_button.h"

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/archive/archive_notice.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/sidebar/sidebar_view.h"
#include "ui/base/metadata/metadata_impl_macros.h"

namespace starry {

namespace {

// 侧栏最窄时，顶栏也要同时容纳红绿灯和三个导航按钮。
constexpr int kArchiveButtonSize = 24;

}  // namespace

ArchiveButton::ArchiveButton(BrowserWindowInterface* browser)
    : IconButton(base::BindRepeating(
                     [](BrowserWindowInterface* browser) {
                       StarryBrowserView* view =
                           StarryBrowserView::From(browser);
                       if (view && view->sidebar()) {
                         if (ArchiveNotice* notice = view->archive_notice()) {
                           notice->Dismiss();
                         }
                         view->sidebar()->ShowArchive(true);
                       }
                     },
                     browser),
                 icons::kArchivebox,
                 L("archive.button.help"),
                 kArchiveButtonSize) {
  SetWindowDraggable(this, false);
  SetFocusBehavior(FocusBehavior::ALWAYS);
  if (!browser->GetProfile()->IsOffTheRecord()) {
    observation_.Observe(
        tab_organizer::TabArchive::ForProfile(browser->GetProfile()));
  }
  OnArchiveChanged();
}

ArchiveButton::~ArchiveButton() = default;

void ArchiveButton::SetTabsPageVisible(bool visible) {
  tabs_page_visible_ = visible;
  OnArchiveChanged();
}

void ArchiveButton::OnArchiveChanged() {
  SetVisible(tabs_page_visible_ && observation_.IsObserving() &&
             !observation_.GetSource()->tabs().empty());
}

BEGIN_METADATA(ArchiveButton)
END_METADATA

}  // namespace starry
