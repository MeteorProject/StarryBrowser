// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_FILTER_TAB_HELPER_H_
#define STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_FILTER_TAB_HELPER_H_

#include "base/observer_list_types.h"
#include "base/timer/timer.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace starry::content_blocking {

// 代理拦下了一个请求（界面线程）：记到发出它的页面上。
void RecordBlockedRequest(content::GlobalRenderFrameHostId frame);
// 拦下了一个 iframe 的导航。
void RecordBlockedNavigation(content::FrameTreeNodeId frame_tree_node_id);

// 每个 WebContents 一个（第一次导航时建，StarryContentBrowserClient::
// CreateThrottlesForNavigation）：
// - 每个文档提交前把它的元素隐藏送到网页进程（content_filter.mojom），网页进程第一次
//   用到时先送一次不限网站的选择器；
// - 每个页面拦下了几个请求（用于地址栏盾牌计数），
//   记在页面上，前进后退回到缓存的页面时数字跟着回来。
class ContentFilterTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<ContentFilterTabHelper> {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // 这个标签显示的页面拦下的数变了（或者换了页面）。
    virtual void OnBlockedCountChanged(content::WebContents* contents) = 0;
  };

  ContentFilterTabHelper(const ContentFilterTabHelper&) = delete;
  ContentFilterTabHelper& operator=(const ContentFilterTabHelper&) = delete;
  ~ContentFilterTabHelper() override;

  static void MaybeCreate(content::WebContents* contents);
  // 正在显示的页面拦下了几个请求。
  static int BlockedCount(content::WebContents* contents);
  static void AddObserver(Observer* observer);
  static void RemoveObserver(Observer* observer);

  // 计数变了：稍后通知（一个页面开头常常一下子拦下几十个，合在一起）。
  void ScheduleNotify();

  // content::WebContentsObserver:
  void ReadyToCommitNavigation(content::NavigationHandle* handle) override;
  void PrimaryPageChanged(content::Page& page) override;

 private:
  friend class content::WebContentsUserData<ContentFilterTabHelper>;

  explicit ContentFilterTabHelper(content::WebContents* contents);
  void Notify();

  base::OneShotTimer notify_timer_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace starry::content_blocking

#endif  // STARRY_BROWSER_CONTENT_BLOCKING_CONTENT_FILTER_TAB_HELPER_H_
