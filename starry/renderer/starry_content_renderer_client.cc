// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/starry_content_renderer_client.h"

#include "content/public/renderer/render_thread.h"
#include "starry/renderer/content_filter/content_filter_agent.h"
#include "starry/renderer/media/youtube_media_session.h"
#include "starry/renderer/web_time_zone_agent.h"

namespace starry {

StarryContentRendererClient::StarryContentRendererClient() = default;

StarryContentRendererClient::~StarryContentRendererClient() = default;

void StarryContentRendererClient::RenderThreadStarted() {
  ChromeContentRendererClient::RenderThreadStarted();
  // 模拟位置的网站用那个位置的时区：浏览器在导航提交前推过来。
  web_time_zone_agent_ = std::make_unique<WebTimeZoneAgent>();
  content::RenderThread::Get()->AddObserver(web_time_zone_agent_.get());
}

void StarryContentRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  ChromeContentRendererClient::RenderFrameCreated(render_frame);
  // 元素隐藏和脚本片段。frame 销毁时它自己删掉。
  new ContentFilterAgent(render_frame);
}

void StarryContentRendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
  // YouTube 的预览、预告片不抢媒体会话。最先跑：
  // 只包几个函数，不会删掉 frame。
  MaybeGuardYouTubeMediaSession(render_frame);
  // 脚本片段排在扩展的 document_start 脚本前面（之后的代码可能删掉 frame）。
  if (ContentFilterAgent* agent = ContentFilterAgent::Get(render_frame)) {
    agent->RunScriptsAtDocumentStart();
  }
  ChromeContentRendererClient::RunScriptsAtDocumentStart(render_frame);
}

}  // namespace starry
