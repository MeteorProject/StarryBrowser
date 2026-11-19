// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/content_filter/content_filter_agent.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "starry/common/shared_style_sheet.h"
#include "starry/components/content_filter/procedural_script.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/platform/web_isolated_world_info.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "url/gurl.h"

namespace starry {

namespace {

// 进程里共用的不限网站的选择器（只在主线程用）。
struct GenericSelectors {
  uint64_t version = 0;
  std::vector<std::string> selectors;
  // 做好的样式表：键（版本，加上去掉了哪几条）→ 文字。键写在开头的注释里，Blink 按它
  // 共用解析结果。
  std::vector<std::pair<uint64_t, blink::WebString>> sheets;
};

GenericSelectors& Generic() {
  static base::NoDestructor<GenericSelectors> generic;
  return *generic;
}

// 同一份选择器，最多留几种「去掉几条」的做法（大多数网站一条也不去）。
constexpr size_t kMaxSheets = 4;

uint64_t SheetKey(uint64_t version, const std::vector<uint32_t>& exclusions) {
  uint64_t key = version;
  for (uint32_t id : exclusions) {
    key = (key ^ id) * 1099511628211ull;
  }
  return key;
}

const blink::WebString& GenericSheet(const std::vector<uint32_t>& exclusions) {
  GenericSelectors& generic = Generic();
  const uint64_t key = SheetKey(generic.version, exclusions);
  for (const auto& [sheet_key, sheet] : generic.sheets) {
    if (sheet_key == key) {
      return sheet;
    }
  }
  std::string css = base::StringPrintf(
      "%s%016llx*/\n", std::string(kSharedStyleSheetPrefix).c_str(),
      static_cast<unsigned long long>(key));
  css.reserve(css.size() + generic.selectors.size() * 48);
  size_t next = 0;
  for (size_t i = 0; i < generic.selectors.size(); ++i) {
    if (next < exclusions.size() && exclusions[next] == i) {
      ++next;
      continue;
    }
    css += generic.selectors[i];
    css += "{display:none!important}\n";
  }
  if (generic.sheets.size() >= kMaxSheets) {
    // 第一份是一条不去的（最常用），留着。
    generic.sheets.erase(generic.sheets.begin() + 1);
  }
  generic.sheets.emplace_back(key, blink::WebString::FromUtf8(css));
  return generic.sheets.back().second;
}

// 扩展选择器藏起来、加样式用的属性名：每个文档随机一个（网页没法事先写好样式、脚本
// 对着它）。
std::string RandomAttributeName() {
  std::string name;
  uint64_t bits = base::RandUint64();
  for (int i = 0; i < 12; ++i) {
    name += static_cast<char>('a' + bits % 26);
    bits /= 26;
  }
  return name;
}

void ExecuteScript(blink::WebLocalFrame* frame,
                   int32_t world_id,
                   const std::string& code) {
  const blink::WebScriptSource source(blink::WebString::FromUtf8(code));
  frame->RequestExecuteScript(
      world_id, base::span_from_ref(source),
      blink::mojom::UserActivationOption::kDoNotActivate,
      blink::mojom::EvaluationTiming::kSynchronous,
      blink::mojom::LoadEventBlockingOption::kDoNotBlock,
      blink::WebScriptExecutionCallback(), blink::BackForwardCacheAware::kAllow,
      blink::mojom::WantResultOption::kNoResult,
      blink::mojom::PromiseResultOption::kDoNotWait, blink::WebString());
}

}  // namespace

ContentFilterAgent::ContentFilterAgent(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame),
      content::RenderFrameObserverTracker<ContentFilterAgent>(render_frame) {
  render_frame->GetAssociatedInterfaceRegistry()
      ->AddInterface<mojom::ContentFilterAgent>(base::BindRepeating(
          &ContentFilterAgent::Bind, base::Unretained(this)));
}

ContentFilterAgent::~ContentFilterAgent() = default;

void ContentFilterAgent::Bind(
    mojo::PendingAssociatedReceiver<mojom::ContentFilterAgent> receiver) {
  receivers_.Add(this, std::move(receiver));
}

void ContentFilterAgent::SetGenericSelectors(
    uint64_t version,
    const std::vector<std::string>& selectors) {
  GenericSelectors& generic = Generic();
  if (generic.version == version) {
    return;
  }
  generic.version = version;
  generic.selectors = selectors;
  generic.sheets.clear();
}

void ContentFilterAgent::ActivateForNextDocument(
    mojom::CosmeticFiltersPtr filters) {
  pending_ = std::move(filters);
}

void ContentFilterAgent::DidCreateDocumentElement() {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  const GURL url(frame->GetDocument().Url());
  current_.reset();
  scripts_pending_ = false;
  procedural_pending_ = false;
  if (pending_ && pending_->url == url) {
    current_ = std::move(pending_);
    Apply(*current_);
    return;
  }
  // 不经过浏览器导航的文档（about:blank、srcdoc）：沿用上一层的。
  if (url.IsAboutBlank() || url.IsAboutSrcdoc() || url.is_empty()) {
    blink::WebFrame* parent = frame->Parent();
    if (parent && parent->IsWebLocalFrame()) {
      content::RenderFrame* parent_frame =
          content::RenderFrame::FromWebFrame(parent->ToWebLocalFrame());
      ContentFilterAgent* agent =
          parent_frame ? ContentFilterAgent::Get(parent_frame) : nullptr;
      if (agent && agent->current_) {
        current_ = agent->current_.Clone();
        Apply(*current_);
      }
    }
  }
}

void ContentFilterAgent::RunScriptsAtDocumentStart() {
  if (!current_) {
    return;
  }
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (scripts_pending_) {
    scripts_pending_ = false;
    // 主 world：脚本片段要改的是网页自己看得到的函数和属性。同扩展的 MAIN world
    // 脚本用 RequestExecuteScript（同步跑），不用 ExecuteScript：后者把这段脚本当作看不到
    // 内容的，里面抛的错报给网页时只剩 "Script error."，认不出是拦截抛的、压不住。
    // 网页的 CSP 管不到这里。
    ExecuteScript(frame, blink::kMainDOMWorldId, current_->scripts);
  }
  if (procedural_pending_) {
    procedural_pending_ = false;
    // 扩展选择器在 Chrome 内部用的隔离 world 里跑：网页改不了它用的 DOM 函数（比如
    // 换掉 MutationObserver、textContent），也看不到它。不定义全局的名字，和共用这个
    // world 的（DOM Distiller 这类）互不影响。
    std::string code(content_filter::ProceduralScript());
    code += '(';
    code += current_->procedural;
    code += ",\"";
    code += procedural_attr_;
    code += "\");";
    ExecuteScript(frame, ISOLATED_WORLD_ID_CHROME_INTERNAL, code);
  }
}

void ContentFilterAgent::Apply(const mojom::CosmeticFilters& filters) {
  scripts_pending_ = !filters.scripts.empty();
  blink::WebDocument document = render_frame()->GetWebFrame()->GetDocument();
  if (filters.generic_version && filters.generic_version == Generic().version &&
      !Generic().selectors.empty()) {
    document.InsertStyleSheet(GenericSheet(filters.generic_exclusions),
                              nullptr, blink::WebCssOrigin::kUser);
  }
  if (!filters.css.empty()) {
    document.InsertStyleSheet(blink::WebString::FromUtf8(filters.css), nullptr,
                              blink::WebCssOrigin::kUser);
  }
  procedural_pending_ = !filters.procedural.empty();
  if (procedural_pending_) {
    // 扩展选择器找到的元素加上属性，这张样式表藏起来、加样式（procedural.js）。
    procedural_attr_ = RandomAttributeName();
    std::string css = "[" + procedural_attr_ + "]{display:none!important}\n";
    for (size_t i = 0; i < filters.procedural_styles.size(); ++i) {
      css += "[" + procedural_attr_ + "-" + base::NumberToString(i) + "]{" +
             filters.procedural_styles[i] + "}\n";
    }
    document.InsertStyleSheet(blink::WebString::FromUtf8(css), nullptr,
                              blink::WebCssOrigin::kUser);
  }
}

void ContentFilterAgent::OnDestruct() {
  delete this;
}

}  // namespace starry
