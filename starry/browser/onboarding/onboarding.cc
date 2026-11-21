// Copyright 2026 The Starry Browser Authors

#include "starry/browser/onboarding/onboarding.h"

#include "base/command_line.h"
#include "base/strings/strcat.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_switches.h"
#include "components/prefs/pref_service.h"
#include "content/public/common/url_constants.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "starry/common/internal_urls.h"
#include "url/gurl.h"

namespace starry::onboarding {

namespace {

constexpr int kVersion = 1;
// --starry-onboarding：这次启动里走完过一次就不再显示（不然每个新窗口都是向导）。
bool g_forced_finished = false;

}  // namespace

bool ShouldShow(Profile* profile) {
  if (!profile || profile->IsOffTheRecord() || profile->IsGuestSession() ||
      profile->IsSystemProfile()) {
    return false;
  }
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(kSwitch)) {
    return !g_forced_finished;
  }
  PrefService* prefs = prefs::PrefsFor(profile);
  if (prefs->GetInteger(prefs::kOnboardingVersion) > 0) {
    return false;
  }
  if (prefs->GetBoolean(prefs::kOnboardingPending)) {
    return true;
  }
  // 已有的用户资料（升级上来的）不显示。--no-first-run（scripts/run.sh、测试）不显示：
  // 临时数据目录每次都是新的用户资料。
  if (!profile->IsNewProfile() ||
      command_line.HasSwitch(switches::kNoFirstRun)) {
    return false;
  }
  prefs->SetBoolean(prefs::kOnboardingPending, true);
  return true;
}

void MarkFinished(Profile* profile) {
  g_forced_finished = true;
  if (!profile) {
    return;
  }
  PrefService* prefs = prefs::PrefsFor(profile);
  prefs->SetBoolean(prefs::kOnboardingPending, false);
  prefs->SetInteger(prefs::kOnboardingVersion, kVersion);
}

GURL WelcomeURL() {
  return GURL(
      base::StrCat({content::kChromeUIScheme, url::kStandardSchemeSeparator,
                    kStarryWelcomeHost, "/"}));
}

bool IsWelcomeURL(const GURL& url) {
  const GURL chrome_url = ToChromeURL(url);
  return chrome_url.SchemeIs(content::kChromeUIScheme) &&
         chrome_url.host() == kStarryWelcomeHost;
}

}  // namespace starry::onboarding
