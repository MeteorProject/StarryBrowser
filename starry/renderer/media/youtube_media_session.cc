// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/media/youtube_media_session.h"

#include <string_view>

#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "content/public/renderer/render_frame.h"
#include "starry/renderer/media/youtube_media_session_script.h"
#include "third_party/blink/public/platform/web_isolated_world_info.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace starry {

namespace {

bool IsYouTube(const GURL& url) {
  if (!url.SchemeIs(url::kHttpsScheme)) {
    return false;
  }
  const std::string_view host = url.host();
  return host == "youtube.com" || base::EndsWith(host, ".youtube.com");
}

}  // namespace

void MaybeGuardYouTubeMediaSession(content::RenderFrame* render_frame) {
  // 只有顶层页面：嵌在别的网站里的播放器一个页面一个，不会互相抢。
  if (!render_frame || !render_frame->IsMainFrame()) {
    return;
  }
  blink::WebLocalFrame* frame = render_frame->GetWebFrame();
  if (!IsYouTube(GURL(frame->GetDocument().Url()))) {
    return;
  }
  // 主 world，同步跑（同脚本片段，见 ContentFilterAgent）：要包的是网页自己用的
  // navigator.mediaSession，得在 YouTube 的脚本之前。
  const blink::WebScriptSource source(
      blink::WebString::FromUtf8(YouTubeMediaSessionScript()));
  frame->RequestExecuteScript(
      blink::kMainDOMWorldId, base::span_from_ref(source),
      blink::mojom::UserActivationOption::kDoNotActivate,
      blink::mojom::EvaluationTiming::kSynchronous,
      blink::mojom::LoadEventBlockingOption::kDoNotBlock,
      blink::WebScriptExecutionCallback(), blink::BackForwardCacheAware::kAllow,
      blink::mojom::WantResultOption::kNoResult,
      blink::mojom::PromiseResultOption::kDoNotWait, blink::WebString());
}

}  // namespace starry
