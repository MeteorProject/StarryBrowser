// Copyright 2026 The Starry Browser Authors

#include "starry/browser/starry_browser_main_extra_parts.h"

#include <algorithm>
#include <string>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "chrome/grit/branded_strings.h"
#include "components/version_info/version_info.h"
#include "starry/browser/agent/agent_service.h"
#include "starry/browser/content_blocking/content_blocking_service.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/mini_window/mini_window_activation.h"
#include "starry/browser/ui/swipe/overscroll_navigation.h"
#include "starry/browser/ui/task_manager/task_manager_window.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "starry/browser/updater/updater_service.h"
#include "starry/browser/web_time_zone.h"
#include "ui/base/resource/resource_bundle.h"

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/devtools.h"
#endif

#if BUILDFLAG(IS_MAC)
#include <dlfcn.h>
#endif

namespace starry {

namespace {

#if BUILDFLAG(IS_MAC)
// macOS 上用某种语言启动 App 的写法是 `-AppleLanguages "(ja)"`（系统读它定 App 的语言，
// Chrome 的界面语言跟着变）。Chrome 把 "(ja)" 当成网址打开一个
// 标签：在 Chrome 处理命令行网址之前拿掉它。
void DropAppleLanguagesValue() {
  // HasSwitch 只认小写的名字，直接看 argv。不带开关的参数（网址）都被挤到了最后，
  // 所以拿掉的是「(…)」这样的参数，不是紧跟在开关后面的那个。
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  const base::CommandLine::StringVector& original = command_line->argv();
  if (std::ranges::find(original, "-AppleLanguages") == original.end() &&
      std::ranges::find(original, "--AppleLanguages") == original.end()) {
    return;
  }
  base::CommandLine::StringVector argv;
  for (const std::string& arg : original) {
    if (!(arg.starts_with("(") && arg.ends_with(")"))) {
      argv.push_back(arg);
    }
  }
  if (argv.size() != original.size()) {
    command_line->InitFromArgv(argv);
  }
}
#endif

}  // namespace

StarryBrowserMainExtraParts::StarryBrowserMainExtraParts() = default;

StarryBrowserMainExtraParts::~StarryBrowserMainExtraParts() = default;

void StarryBrowserMainExtraParts::PreEarlyInitialization() {
#if BUILDFLAG(IS_MAC)
  // Release 没有调试信息，Chromium 崩溃时只打地址。记下框架的载入地址，就能用
  // scripts/symbolize.sh 还原函数名。
  Dl_info info;
  if (dladdr(reinterpret_cast<const void*>(&DropAppleLanguagesValue), &info)) {
    LOG(INFO) << "[starry] framework " << info.dli_fname << " loaded at "
              << info.dli_fbase;
  }
  DropAppleLanguagesValue();
#endif
}

void StarryBrowserMainExtraParts::PreCreateThreads() {
  // Chrome 自己的文字里的产品名（macOS 菜单栏的「关于 / 隐藏 / 退出 ×××」等）：换成
  // BRANDING 里的名字（starry/app/theme/BRANDING）。Chrome 在这之后才建 macOS 的主菜单。
  // 整句文案里的品牌在打包语言资源时替换（starry/build/branding/）。
  const std::u16string name =
      base::UTF8ToUTF16(version_info::GetProductName());
  ui::ResourceBundle& bundle = ui::ResourceBundle::GetSharedInstance();
  bundle.OverrideLocaleStringResource(IDS_PRODUCT_NAME, name);
  bundle.OverrideLocaleStringResource(IDS_SHORT_PRODUCT_NAME, name);
}

void StarryBrowserMainExtraParts::PreProfileInit() {
  // Starry 的设置项、跟着用户资料建的服务要在建第一个用户资料之前注册好。
  prefs::EnsureRegistered();
  DownloadService::EnsureFactoryBuilt();
  // 广告与跟踪拦截：读设置、在后台建引擎，赶在恢复窗口之前（建好之前网页的
  // 请求先等一下）。
  content_blocking::ContentBlockingService::Get()->Start();
  // aura 上触控板、触摸屏左右滑动翻页用 Starry 的指示器（赶在恢复窗口之前）。
  RegisterOverscrollNavigation();
}

void StarryBrowserMainExtraParts::PostProfileInit(Profile* profile,
                                                  bool is_initial_profile) {
  EffectiveWebTimeZone(profile);
#if defined(STARRY_DEVTOOLS)
  if (is_initial_profile) {
    initial_profile_ = profile;
  }
#endif
  // Chrome 的页面已经注册好了：换成 Starry 的（只换一次）。
  RegisterStarryPages();
  // 打开了外部智能体的用户资料：套接字服务开起来。
  agent::AgentService::Get()->SettingsChanged();
}

void StarryBrowserMainExtraParts::PostBrowserStart() {
  // 小窗口（其他 App 里点的链接）出现时把浏览器窗口留在后面：从现在起听应用激活。
  mini_window::StartTrackingActivation();
  // 自动更新：加载持久化状态与已就绪包，按配置启动定时检查。
  updater::UpdaterService::Get()->Start();
#if defined(STARRY_DEVTOOLS)
  devtools::OnBrowserStart(initial_profile_);
#endif
}

void StarryBrowserMainExtraParts::PostMainMessageLoopRun() {
#if defined(STARRY_DEVTOOLS)
  initial_profile_ = nullptr;
#endif
  // 任务管理器的窗口在用户资料拆掉之前关掉（它看着用户资料的主题设置）。
  CloseTaskManagerWindowNow();
  // 断开智能体的连接，删掉套接字文件。
  agent::AgentService::Get()->Shutdown();
  content_blocking::ContentBlockingService::Get()->Shutdown();
  updater::UpdaterService::Get()->Shutdown();
}

void StarryBrowserMainExtraParts::PostDestroyThreads() {
  updater::UpdaterService::Get()->OnPostDestroyThreads();
}

}  // namespace starry
