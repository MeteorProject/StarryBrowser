// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_CONTENT_FILTER_CONTENT_FILTER_AGENT_H_
#define STARRY_RENDERER_CONTENT_FILTER_CONTENT_FILTER_AGENT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "starry/components/content_filter/mojom/content_filter.mojom.h"

namespace starry {

// 网页进程里一个 frame 的元素隐藏。浏览器在
// 导航提交前送来这个文档的规则（ActivateForNextDocument），文档的根元素一建好（网页的
// 第一段脚本、第一次排版之前）就插进两张用户样式表：
// - 不限网站的：进程里共用一份解析好的（starry/renderer/shared_style_sheets.h），这个
//   网站上不用的几条去掉时另做一份、也缓存起来；
// - 限定这个网站的：每个文档自己解析（通常几条到几十条）。
// 用户样式表的 `!important` 压得过网页自己的样式。接着（RunScriptsAtDocumentStart，同一个
// 时候，只是 DidCreateDocumentElement 里不许跑脚本）在网页的主 world 里跑脚本片段，在隔离的
// world 里跑扩展选择器（procedural.js：找到的元素加上这个文档随机的属性，这里插的第三张
// 样式表对着属性藏起来、加样式）。
// about:blank、srcdoc 的 iframe（不经过浏览器的导航，常被广告脚本拿来写广告）沿用上一层
// 文档的。
class ContentFilterAgent
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<ContentFilterAgent>,
      public mojom::ContentFilterAgent {
 public:
  explicit ContentFilterAgent(content::RenderFrame* render_frame);
  ContentFilterAgent(const ContentFilterAgent&) = delete;
  ContentFilterAgent& operator=(const ContentFilterAgent&) = delete;
  ~ContentFilterAgent() override;

  // mojom::ContentFilterAgent:
  void SetGenericSelectors(uint64_t version,
                           const std::vector<std::string>& selectors) override;
  void ActivateForNextDocument(mojom::CosmeticFiltersPtr filters) override;

  // content::RenderFrameObserver:
  void DidCreateDocumentElement() override;
  void OnDestruct() override;

  // StarryContentRendererClient::RunScriptsAtDocumentStart 调：跑这个文档的脚本片段。
  void RunScriptsAtDocumentStart();

 private:
  void Bind(mojo::PendingAssociatedReceiver<mojom::ContentFilterAgent> receiver);
  void Apply(const mojom::CosmeticFilters& filters);

  mojo::AssociatedReceiverSet<mojom::ContentFilterAgent> receivers_;
  // 下一个文档的（还没提交）。
  mojom::CosmeticFiltersPtr pending_;
  // 现在这个文档用着的（子 frame 的 about:blank 沿用）。
  mojom::CosmeticFiltersPtr current_;
  // current_ 的脚本片段、扩展选择器还没跑。
  bool scripts_pending_ = false;
  bool procedural_pending_ = false;
  // 这个文档的扩展选择器用的属性名。
  std::string procedural_attr_;
};

}  // namespace starry

#endif  // STARRY_RENDERER_CONTENT_FILTER_CONTENT_FILTER_AGENT_H_
