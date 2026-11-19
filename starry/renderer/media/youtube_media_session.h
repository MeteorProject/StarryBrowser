// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_MEDIA_YOUTUBE_MEDIA_SESSION_H_
#define STARRY_RENDERER_MEDIA_YOUTUBE_MEDIA_SESSION_H_

namespace content {
class RenderFrame;
}

namespace starry {

// YouTube 的顶层页面里，鼠标停在推荐上时的静音预览、频道页的预告片这些播放器
// 不把媒体会话（标题、播放状态、按钮）抢成自己的（youtube_media_session.js）。
// 文档开始时（RunScriptsAtDocumentStart，网页的脚本跑起来之前）调。
void MaybeGuardYouTubeMediaSession(content::RenderFrame* render_frame);

}  // namespace starry

#endif  // STARRY_RENDERER_MEDIA_YOUTUBE_MEDIA_SESSION_H_
