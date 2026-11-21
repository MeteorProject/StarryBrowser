// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_STARRY_CONTENT_RENDERER_CLIENT_H_
#define STARRY_RENDERER_STARRY_CONTENT_RENDERER_CLIENT_H_

#include <memory>

#include "chrome/renderer/chrome_content_renderer_client.h"

namespace starry {

class WebTimeZoneAgent;

// 网页进程里 Chrome 的 ContentRendererClient 加上 Starry 的部分。
// patches/chrome/app/chrome_main_delegate.cc.patch 让 Chrome 用它代替
// ChromeContentRendererClient。
class StarryContentRendererClient : public ChromeContentRendererClient {
 public:
  StarryContentRendererClient();
  StarryContentRendererClient(const StarryContentRendererClient&) = delete;
  StarryContentRendererClient& operator=(const StarryContentRendererClient&) =
      delete;
  ~StarryContentRendererClient() override;

  // ChromeContentRendererClient:
  void RenderThreadStarted() override;
  void RenderFrameCreated(content::RenderFrame* render_frame) override;
  void RunScriptsAtDocumentStart(content::RenderFrame* render_frame) override;

 private:
  std::unique_ptr<WebTimeZoneAgent> web_time_zone_agent_;
};

}  // namespace starry

#endif  // STARRY_RENDERER_STARRY_CONTENT_RENDERER_CLIENT_H_
