// Copyright 2026 The Starry Browser Authors

#include "starry/browser/content_blocking/content_filter_tab_helper.h"

#include <map>
#include <utility>

#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/scoped_multi_source_observation.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page.h"
#include "content/public/browser/page_user_data.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_process_host_observer.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "starry/browser/content_blocking/filter_config.h"
#include "starry/components/content_filter/filter_engine.h"
#include "starry/components/content_filter/mojom/content_filter.mojom.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "url/gurl.h"

namespace starry::content_blocking {

namespace {

constexpr base::TimeDelta kNotifyDelay = base::Milliseconds(80);

// 一个页面拦下了几个请求。
class BlockedRequests : public content::PageUserData<BlockedRequests> {
 public:
  ~BlockedRequests() override = default;

  int count = 0;

 private:
  friend class content::PageUserData<BlockedRequests>;
  explicit BlockedRequests(content::Page& page)
      : content::PageUserData<BlockedRequests>(page) {}

  PAGE_USER_DATA_KEY_DECL();
};

PAGE_USER_DATA_KEY_IMPL(BlockedRequests);

base::ObserverList<ContentFilterTabHelper::Observer>& Observers() {
  static base::NoDestructor<base::ObserverList<ContentFilterTabHelper::Observer>>
      observers;
  return *observers;
}

// 哪些网页进程已经有了哪一份不限网站的选择器。进程退出时忘掉（再起来的进程要重新送）。
class GenericSelectorsSent : public content::RenderProcessHostObserver {
 public:
  static GenericSelectorsSent& Get() {
    static base::NoDestructor<GenericSelectorsSent> sent;
    return *sent;
  }

  // 这个进程还没有这一份时返回 true（并记下已经送了）。
  bool NeedsSend(content::RenderProcessHost* process, uint64_t version) {
    const int id = process->GetDeprecatedID();
    auto it = versions_.find(id);
    if (it != versions_.end() && it->second == version) {
      return false;
    }
    if (it == versions_.end() && !observation_.IsObservingSource(process)) {
      observation_.AddObservation(process);
    }
    versions_[id] = version;
    return true;
  }

  // content::RenderProcessHostObserver:
  void RenderProcessExited(
      content::RenderProcessHost* host,
      const content::ChildProcessTerminationInfo& info) override {
    versions_.erase(host->GetDeprecatedID());
  }
  void RenderProcessHostDestroyed(content::RenderProcessHost* host) override {
    versions_.erase(host->GetDeprecatedID());
    observation_.RemoveObservation(host);
  }

 private:
  friend class base::NoDestructor<GenericSelectorsSent>;
  GenericSelectorsSent() = default;

  std::map<int, uint64_t> versions_;
  base::ScopedMultiSourceObservation<content::RenderProcessHost,
                                     content::RenderProcessHostObserver>
      observation_{this};
};

void Count(content::RenderFrameHost* frame) {
  if (!frame) {
    return;
  }
  content::RenderFrameHost* main = frame->GetOutermostMainFrame();
  BlockedRequests::GetOrCreateForPage(main->GetPage())->count++;
  if (!main->GetPage().IsPrimary()) {
    return;
  }
  if (auto* helper = ContentFilterTabHelper::FromWebContents(
          content::WebContents::FromRenderFrameHost(main))) {
    helper->ScheduleNotify();
  }
}

// 页面（和它的上层）的整页例外。
uint32_t FrameDocumentFlags(const content_filter::FilterEngine& engine,
                            const GURL& url,
                            content::RenderFrameHost* parent) {
  uint32_t flags =
      engine.DocumentFlags(url, parent ? parent->GetLastCommittedURL() : GURL());
  for (content::RenderFrameHost* frame = parent; frame;
       frame = frame->GetParentOrOuterDocument()) {
    content::RenderFrameHost* above = frame->GetParentOrOuterDocument();
    flags |= engine.DocumentFlags(frame->GetLastCommittedURL(),
                                  above ? above->GetLastCommittedURL() : GURL());
  }
  return flags;
}

}  // namespace

void RecordBlockedRequest(content::GlobalRenderFrameHostId frame) {
  Count(content::RenderFrameHost::FromID(frame));
}

void RecordBlockedNavigation(content::FrameTreeNodeId frame_tree_node_id) {
  content::WebContents* contents =
      content::WebContents::FromFrameTreeNodeId(frame_tree_node_id);
  if (!contents) {
    return;
  }
  content::RenderFrameHost* frame =
      contents->UnsafeFindFrameByFrameTreeNodeId(frame_tree_node_id);
  Count(frame ? frame : contents->GetPrimaryMainFrame());
}

ContentFilterTabHelper::ContentFilterTabHelper(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<ContentFilterTabHelper>(*contents) {}

ContentFilterTabHelper::~ContentFilterTabHelper() = default;

// static
void ContentFilterTabHelper::MaybeCreate(content::WebContents* contents) {
  CreateForWebContents(contents);
}

// static
int ContentFilterTabHelper::BlockedCount(content::WebContents* contents) {
  if (!contents) {
    return 0;
  }
  BlockedRequests* blocked =
      BlockedRequests::GetForPage(contents->GetPrimaryPage());
  return blocked ? blocked->count : 0;
}

// static
void ContentFilterTabHelper::AddObserver(Observer* observer) {
  Observers().AddObserver(observer);
}

// static
void ContentFilterTabHelper::RemoveObserver(Observer* observer) {
  Observers().RemoveObserver(observer);
}

void ContentFilterTabHelper::ScheduleNotify() {
  if (!notify_timer_.IsRunning()) {
    notify_timer_.Start(FROM_HERE, kNotifyDelay,
                        base::BindOnce(&ContentFilterTabHelper::Notify,
                                       base::Unretained(this)));
  }
}

void ContentFilterTabHelper::Notify() {
  for (Observer& observer : Observers()) {
    observer.OnBlockedCountChanged(web_contents());
  }
}

void ContentFilterTabHelper::PrimaryPageChanged(content::Page& page) {
  notify_timer_.Stop();
  Notify();
}

void ContentFilterTabHelper::ReadyToCommitNavigation(
    content::NavigationHandle* handle) {
  if (handle->IsSameDocument() || handle->IsErrorPage()) {
    return;
  }
  const GURL& url = handle->GetURL();
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  content::RenderFrameHost* frame = handle->GetRenderFrameHost();
  if (!frame) {
    return;
  }
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  if (!config->enabled() || !config->ready()) {
    return;
  }
  content::RenderFrameHost* parent = handle->GetParentFrameOrOuterDocument();
  const GURL top =
      parent ? parent->GetOutermostMainFrame()->GetLastCommittedURL() : url;
  if (!config->ShouldFilter(top)) {
    return;
  }
  const content_filter::FilterEngine& engine = config->engine();
  const uint32_t flags = FrameDocumentFlags(engine, url, parent);
  content_filter::CosmeticResult result = engine.Cosmetics(url, flags);
  if (!result.generic && result.css.empty() && result.procedural.empty() &&
      result.scripts.empty()) {
    return;
  }
  mojo::AssociatedRemote<mojom::ContentFilterAgent> agent;
  frame->GetRemoteAssociatedInterfaces()->GetInterface(&agent);
  const uint64_t version = result.generic ? engine.generic_version() : 0;
  if (version &&
      GenericSelectorsSent::Get().NeedsSend(frame->GetProcess(), version)) {
    agent->SetGenericSelectors(version, engine.generic_selectors());
  }
  agent->ActivateForNextDocument(mojom::CosmeticFilters::New(
      url, version, std::move(result.generic_exclusions),
      std::move(result.css), std::move(result.procedural),
      std::move(result.procedural_styles), std::move(result.scripts)));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ContentFilterTabHelper);

}  // namespace starry::content_blocking
