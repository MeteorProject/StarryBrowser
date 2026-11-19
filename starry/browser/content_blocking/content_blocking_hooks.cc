// Copyright 2026 The Starry Browser Authors

#include "starry/browser/content_blocking/content_blocking_hooks.h"

#include "content/public/browser/render_frame_host.h"
#include "starry/browser/content_blocking/content_filter_tab_helper.h"
#include "starry/browser/content_blocking/filter_config.h"
#include "starry/components/content_filter/filter_engine.h"
#include "starry/components/content_filter/request.h"
#include "url/gurl.h"

namespace starry::content_blocking {

namespace {

GURL TopLevelUrl(content::RenderFrameHost* frame, const GURL& fallback) {
  return frame ? frame->GetOutermostMainFrame()->GetLastCommittedURL()
               : fallback;
}

bool Blocks(const FilterConfig& config,
            const GURL& top,
            const GURL& source,
            const GURL& url,
            content_filter::RequestType type) {
  const content_filter::FilterEngine& engine = config.engine();
  uint32_t flags = engine.DocumentFlags(top, GURL());
  if (source != top) {
    flags |= engine.DocumentFlags(source, top);
  }
  content_filter::Request request(url, source, type);
  return engine.MatchRequest(request, flags).blocked();
}

}  // namespace

bool ShouldBlockPopup(content::RenderFrameHost* opener,
                      const GURL& opener_url,
                      const GURL& opener_top_level_url,
                      const GURL& target_url) {
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  if (!config->enabled() || !target_url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  const GURL top = TopLevelUrl(opener, opener_top_level_url);
  if (!top.SchemeIsHTTPOrHTTPS() || !config->ShouldFilter(top)) {
    return false;
  }
  if (!Blocks(*config, top, opener_url, target_url,
              content_filter::kTypePopup)) {
    return false;
  }
  if (opener) {
    RecordBlockedRequest(opener->GetGlobalId());
  }
  return true;
}

bool WantsWebSocket(content::RenderFrameHost* frame) {
  return frame && CurrentFilterConfig()->enabled();
}

bool ShouldBlockWebSocket(content::RenderFrameHost* frame, const GURL& url) {
  if (!frame || !url.SchemeIsWSOrWSS()) {
    return false;
  }
  scoped_refptr<const FilterConfig> config = CurrentFilterConfig();
  const GURL top = TopLevelUrl(frame, GURL());
  if (!config->ShouldFilter(top) || !top.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  if (!Blocks(*config, top, frame->GetLastCommittedURL(), url,
              content_filter::kTypeWebSocket)) {
    return false;
  }
  RecordBlockedRequest(frame->GetGlobalId());
  return true;
}

}  // namespace starry::content_blocking
