// Copyright 2026 The Starry Browser Authors

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/exit_type_service.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"

namespace starry {

namespace {

void RestoreAfterDialog(
    base::WeakPtr<BrowserWindowInterface> browser,
    std::unique_ptr<ExitTypeService::CrashedLock> crashed_lock,
    bool open_startup_pages) {
  if (!browser || browser->IsDeleteScheduled() ||
      browser_shutdown::HasShutdownStarted()) {
    return;
  }
  // Keep the crash lock until restore has started, as Chromium's bubble does.
  // Otherwise acknowledging the crash can discard the recoverable session.
  if (open_startup_pages) {
    SessionRestore::OpenStartupPagesAfterCrash(browser.get());
  } else {
    SessionRestore::RestoreSessionAfterCrash(browser.get());
  }
}

}  // namespace

bool ShowSessionCrashedDialog(BrowserWindowInterface* browser,
                              bool skip_tab_checking) {
  StarryBrowserView* view = StarryBrowserView::From(browser);
  if (!view || !view->dialog_host()) {
    return false;
  }
  if (browser->GetProfile()->IsOffTheRecord() || browser->IsDeleteScheduled() ||
      browser_shutdown::HasShutdownStarted() ||
      (!skip_tab_checking &&
       !browser->GetTabStripModel()->GetActiveWebContents())) {
    return true;
  }

  std::unique_ptr<ExitTypeService::CrashedLock> crashed_lock;
  if (ExitTypeService* service =
          ExitTypeService::GetInstanceForProfile(browser->GetProfile())) {
    crashed_lock = service->CreateCrashedLock();
  }
  DialogSpec spec;
  spec.icon = &icons::kExclamationTriangleFill;
  spec.tone = DialogSpec::Tone::kWarning;
  spec.title = L("startup.crash.title");
  spec.message = L("startup.crash.message");
  spec.buttons.push_back({L("startup.crash.dismiss"), ButtonVariant::kTertiary,
                          /*cancel=*/true});
  const SessionStartupPref startup =
      SessionStartupPref::GetStartupPref(browser->GetProfile());
  const bool offers_startup_pages =
      startup.ShouldOpenUrls() && !startup.urls.empty();
  if (offers_startup_pages) {
    spec.buttons.push_back(
        {L("startup.crash.openStartupPages"), ButtonVariant::kSecondary});
  }
  spec.buttons.push_back({L("startup.crash.restore"), ButtonVariant::kFilled});
  view->PresentDialog(
      std::move(spec),
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> browser,
             std::unique_ptr<ExitTypeService::CrashedLock> crashed_lock,
             bool offers_startup_pages, std::optional<size_t> button,
             const std::u16string& input) {
            if (!button || *button == 0) {
              return;
            }
            // DialogHostView closes the card before invoking the answer. Start
            // restore while the crash lock's profile is still owned by this
            // browser, without leaving the lock in a task that may outlive it.
            RestoreAfterDialog(std::move(browser), std::move(crashed_lock),
                               offers_startup_pages && *button == 1);
          },
          browser->GetWeakPtr(), std::move(crashed_lock),
          offers_startup_pages));
  return true;
}

}  // namespace starry
