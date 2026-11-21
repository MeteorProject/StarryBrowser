// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_STARRY_BROWSER_MAIN_EXTRA_PARTS_H_
#define STARRY_BROWSER_STARRY_BROWSER_MAIN_EXTRA_PARTS_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

#if defined(STARRY_DEVTOOLS)
#include "base/memory/raw_ptr.h"
#endif

class Profile;

namespace starry {

// Starry 在浏览器进程启动流程里的步骤。窗口照常由 Chrome 建（启动、会话恢复、新窗口），
// 界面换成 Starry 的是在建窗口的地方（starry_hooks.h）。
class StarryBrowserMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  StarryBrowserMainExtraParts();
  StarryBrowserMainExtraParts(const StarryBrowserMainExtraParts&) = delete;
  StarryBrowserMainExtraParts& operator=(const StarryBrowserMainExtraParts&) =
      delete;
  ~StarryBrowserMainExtraParts() override;

  // ChromeBrowserMainExtraParts:
  void PreEarlyInitialization() override;
  void PreCreateThreads() override;
  void PreProfileInit() override;
  void PostProfileInit(Profile* profile, bool is_initial_profile) override;
  void PostBrowserStart() override;
  void PostMainMessageLoopRun() override;
  void PostDestroyThreads() override;

#if defined(STARRY_DEVTOOLS)
 private:
  raw_ptr<Profile> initial_profile_ = nullptr;
#endif
};

}  // namespace starry

#endif  // STARRY_BROWSER_STARRY_BROWSER_MAIN_EXTRA_PARTS_H_
