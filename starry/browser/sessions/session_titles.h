// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_SESSIONS_SESSION_TITLES_H_
#define STARRY_BROWSER_SESSIONS_SESSION_TITLES_H_

namespace content {
class WebContents;
}

namespace starry {

// 网页的标题也写进会话。Chrome 只在导航提交（这时还没有标题）和网页状态变化（滚动、
// 表单……，静态的网页一直没有）时写这一条导航，标题变了不写，于是恢复以后没载入的
// 标签（休眠的，starry_hooks.h 的 DeferRestoredTabLoad）只能拿网址当标题。标题变了
// 就通知一次（NavigationController::NotifyEntryChanged，会话和最近关闭的标签都更新），
// 不停改标题的网页最多 5 秒一次。第一次导航时挂上（StarryContentBrowserClient）。
void SaveTitlesInSession(content::WebContents* contents);

}  // namespace starry

#endif  // STARRY_BROWSER_SESSIONS_SESSION_TITLES_H_
