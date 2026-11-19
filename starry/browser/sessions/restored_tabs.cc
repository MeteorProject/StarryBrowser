// Copyright 2026 The Starry Browser Authors

// 恢复上次的窗口和标签时，没有显示出来的标签先休眠（starry_hooks.h 的
// DeferRestoredTabLoad，chrome/browser/sessions/session_restore.cc 的补丁调用）。
//
// Chrome 把恢复的后台标签交给 performance_manager 的 BackgroundTabLoadingPolicy，在
// 后台按顺序一直载入下去，几十个标签的会话一启动就全部载入、抢网络和内存。这里让它们
// 留在恢复好、还没载入的样子（前进后退列表都在），并标成休眠：侧栏里是休眠的样子，
// 右键菜单是「唤醒」，悬停预览写着已休眠。切过去、或者在分屏里和当前标签一起显示出来
// 时，NavigationController::SetActive 载入它，休眠的标记在开始导航时清掉（同 Chrome
// 以前的休眠：新建一个 WebContents、恢复前进后退列表、SetWasDiscarded）。

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/common/internal_urls.h"
#include "url/gurl.h"

namespace starry {

bool DeferRestoredTabLoad(content::WebContents* contents) {
  Profile* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  if (!prefs::PrefsFor(profile)->GetBoolean(prefs::kSleepRestoredTabs)) {
    return false;
  }
  // 显示着的（比如分屏里和当前标签一起显示的格子）照常载入。
  if (contents->GetVisibility() == content::Visibility::VISIBLE) {
    return false;
  }
  content::NavigationController& controller = contents->GetController();
  if (!controller.NeedsReload()) {
    // 已经开始载入了：macOS 上窗口里第一个恢复的标签插进空的标签列表时会先当一下当前
    // 标签，网页的视图一显示就载入了（WebContentsViewCocoa viewDidUnhide），之后才
    // 换成会话里选中的那个。这时还来不及提交：停掉，和别的一样等到切过去。
    // 已经有网页的不动。
    if (!contents->GetPrimaryMainFrame()->GetLastCommittedURL().is_empty()) {
      return false;
    }
    contents->Stop();
    controller.SetNeedsReload();
  }

  // 同 Chrome 推迟载入的标签：先从数据库里取回网站图标，侧栏里认得出是哪个网页。
  if (favicon::ContentFaviconDriver* favicon =
          favicon::ContentFaviconDriver::FromWebContents(contents)) {
    favicon->FetchFavicon(favicon->GetActiveURL(), /*is_same_document=*/false);
  }

  // 空白标签和浏览器内部页面同样推迟到切换时再载入，但不标记为休眠。
  const GURL& url = contents->GetLastCommittedURL().is_empty()
                        ? contents->GetVisibleURL()
                        : contents->GetLastCommittedURL();
  if (IsBlankTabUrl(url) || InternalPageForUrl(url) != InternalPage::kNone ||
      IsInternalURL(url) || url.SchemeIs("devtools")) {
    return true;
  }

  contents->SetWasDiscarded(true);
  // 标签已经在列表里了，侧栏和顶部标签是按没休眠画的：让它们重画一次。
  if (tabs::TabInterface* tab =
          tabs::TabInterface::MaybeGetFromContents(contents)) {
    if (BrowserWindowInterface* browser = tab->GetBrowserWindowInterface()) {
      browser->GetTabStripModel()->NotifyTabChanged(tab, TabChangeType::kAll);
    }
  }
  return true;
}

}  // namespace starry
