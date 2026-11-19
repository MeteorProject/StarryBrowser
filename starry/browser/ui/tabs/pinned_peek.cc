// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/pinned_peek.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/prefs/pref_service.h"
#include "components/url_formatter/elide_url.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/page.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view_targeter.h"

namespace starry {
namespace {
constexpr int kHeader = 36;
bool IsExternalLink(content::WebContents* source, const GURL& url) {
  const auto& from = source->GetLastCommittedURL();
  return from.SchemeIsHTTPOrHTTPS() && url.SchemeIsHTTPOrHTTPS() &&
         Profile::FromBrowserContext(source->GetBrowserContext())
             ->GetPrefs()
             ->GetBoolean(prefs::kPinnedPeek) &&
         !net::registry_controlled_domains::SameDomainOrHost(
             from, url,
             net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}
class PeekThrottle : public content::NavigationThrottle {
 public:
  using NavigationThrottle::NavigationThrottle;
  const char* GetNameForLogging() override { return "StarryPinnedLinkPeek"; }
  ThrottleCheckResult WillStartRequest() override {
    auto* nav = navigation_handle();
    auto* contents = nav->GetWebContents();
    auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
    if (!tab || !tab->IsActivated() || !tab->IsPinned() ||
        !tab->GetBrowserWindowInterface()) {
      return PROCEED;
    }
    auto* view = StarryBrowserView::From(tab->GetBrowserWindowInterface());
    if (!view || !view->pinned_peek()) {
      return PROCEED;
    }
    auto params = content::OpenURLParams::FromNavigationHandle(nav);
    params.frame_tree_node_id = content::FrameTreeNodeId();
    params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    // Run once the cancelled navigation has unwound. Preserve the renderer
    // initiator and referrer for Chromium's normal new-tab navigation.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<content::Page> page, tabs::TabHandle source,
               content::OpenURLParams params) {
              auto* tab = source.Get();
              if (!page || !tab || !tab->IsActivated() || !tab->IsPinned() ||
                  &tab->GetContents()->GetPrimaryPage() != page.get() ||
                  !tab->GetBrowserWindowInterface()) {
                return;
              }
              auto* view =
                  StarryBrowserView::From(tab->GetBrowserWindowInterface());
              if (view && view->pinned_peek()) {
                view->pinned_peek()->Open(source, std::move(params));
              }
            },
            contents->GetPrimaryPage().GetWeakPtr(), tab->GetHandle(),
            std::move(params)));
    return CANCEL_AND_IGNORE;
  }
};
}  // namespace

void PinnedPeek::MaybeAddThrottle(
    content::NavigationThrottleRegistry& registry) {
  auto& nav = registry.GetNavigationHandle();
  if (!nav.IsInPrimaryMainFrame() || !nav.IsRendererInitiated() ||
      !nav.HasUserGesture() || nav.IsPost() || nav.IsFormSubmission() ||
      nav.IsHistory() || nav.WasStartedFromContextMenu() ||
      !ui::PageTransitionCoreTypeIs(nav.GetPageTransition(),
                                    ui::PAGE_TRANSITION_LINK) ||
      !nav.GetURL().SchemeIsHTTPOrHTTPS()) {
    return;
  }
  auto* contents = nav.GetWebContents();
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab || !tab->IsPinned() || !IsExternalLink(contents, nav.GetURL())) {
    return;
  }
  // Only a click in the source document can open a preview. Subframes and
  // navigations with a sandbox or an opaque initiator retain Chromium's path.
  auto* frame = contents->GetPrimaryMainFrame();
  if (!nav.GetInitiatorFrameToken() ||
      *nav.GetInitiatorFrameToken() != frame->GetFrameToken() ||
      frame->IsSandboxed(network::mojom::WebSandboxFlags::kAll) ||
      nav.GetInitiatorOrigin() != frame->GetLastCommittedOrigin()) {
    return;
  }
  registry.AddThrottle(std::make_unique<PeekThrottle>(registry));
}

void PinnedPeek::LinkOpened(content::WebContents* source,
                            content::WebContents* target,
                            const GURL& url) {
  if (!target || !IsExternalLink(source, url)) {
    return;
  }
  auto* owner = tabs::TabInterface::MaybeGetFromContents(source);
  if (!owner || !owner->IsPinned() || !owner->GetBrowserWindowInterface()) {
    return;
  }
  auto* window = StarryBrowserView::From(owner->GetBrowserWindowInterface());
  if (!window || !window->pinned_peek()) {
    return;
  }
  if (owner->IsActivated()) {
    window->pinned_peek()->CaptureBackdrop(owner);
  }
  // target=_blank is initially reported before Chromium inserts the new tab.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<content::WebContents> source,
             base::WeakPtr<content::WebContents> target) {
            if (!source || !target) {
              return;
            }
            auto* owner =
                tabs::TabInterface::MaybeGetFromContents(source.get());
            auto* tab = tabs::TabInterface::MaybeGetFromContents(target.get());
            if (!owner || !owner->IsPinned() || !tab || !tab->IsActivated() ||
                !owner->GetBrowserWindowInterface() ||
                owner->GetBrowserWindowInterface() !=
                    tab->GetBrowserWindowInterface()) {
              return;
            }
            auto* view =
                StarryBrowserView::From(owner->GetBrowserWindowInterface());
            if (view && view->pinned_peek() &&
                view->pinned_peek()->preview() != tab) {
              view->pinned_peek()->Present(owner, tab);
            }
          },
          source->GetWeakPtr(), target->GetWeakPtr()));
}

PinnedPeek::PinnedPeek(StarryBrowserView* view) : view_(view) {
  SetVisible(false);
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  AddAccelerator(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));
  expand_ = AddChildView(std::make_unique<GlyphButton>(
      base::BindRepeating(&PinnedPeek::Expand, base::Unretained(this)),
      icons::kArrowUpLeftAndArrowDownRight, 14, 28));
  close_ = AddChildView(std::make_unique<GlyphButton>(
      base::BindRepeating(&PinnedPeek::Close, base::Unretained(this)),
      icons::kXmark, 14, 28));
  expand_->SetTooltipText(L("tabs.expandPeek"));
  expand_->GetViewAccessibility().SetName(L("tabs.expandPeek"));
  close_->SetTooltipText(L("tabs.closePeek"));
  close_->GetViewAccessibility().SetName(L("tabs.closePeek"));
}
PinnedPeek::~PinnedPeek() = default;

void PinnedPeek::CaptureBackdrop(tabs::TabInterface* source) {
  if (auto* surface = source->GetContents()->GetRenderWidgetHostView()) {
    surface->CopyFromSurface(
        {}, {}, base::Milliseconds(200),
        base::BindOnce(
            [](base::WeakPtr<PinnedPeek> peek, tabs::TabHandle source,
               const content::CopyFromSurfaceResult& result) {
              if (peek && peek->source_ == source && result.has_value()) {
                peek->snapshot_ =
                    gfx::ImageSkia::CreateFrom1xBitmap(result->bitmap);
                peek->SchedulePaint();
              }
            },
            weak_.GetWeakPtr(), source->GetHandle()));
  }
}

void PinnedPeek::Open(tabs::TabHandle handle, content::OpenURLParams params) {
  auto* owner = handle.Get();
  if (!owner) {
    return;
  }
  Hide();
  CaptureBackdrop(owner);
  auto* target = owner->GetContents()->OpenURL(params, {});
  auto* preview =
      target ? tabs::TabInterface::MaybeGetFromContents(target) : nullptr;
  if (handle.Get() && preview && preview != handle.Get() &&
      preview->IsActivated() &&
      preview->GetBrowserWindowInterface() ==
          handle.Get()->GetBrowserWindowInterface()) {
    Present(handle.Get(), preview);
  }
}

void PinnedPeek::Present(tabs::TabInterface* source, tabs::TabInterface* tab) {
  source_ = source->GetHandle();
  preview_ = tab->GetHandle();
  auto* model = view_->browser()->GetTabStripModel();
  model->SetOpenerOfTabAt(model->GetIndexOfTab(tab), source);
  deactivation_ = tab->RegisterWillDeactivate(base::BindRepeating(
      [](PinnedPeek* peek, tabs::TabInterface*) { peek->Hide(); },
      base::Unretained(this)));
  source_detach_ = source->RegisterWillDetach(base::BindRepeating(
      [](PinnedPeek* peek, tabs::TabInterface*,
         tabs::TabInterface::DetachReason) { peek->Expand(); },
      base::Unretained(this)));
  SetVisible(true);
  view_->InvalidateLayout();
  model->NotifyTabChanged(tab, TabChangeType::kAll);
}

void PinnedPeek::Hide() {
  weak_.InvalidateWeakPtrs();
  deactivation_ = {};
  source_detach_ = {};
  preview_ = {};
  source_ = {};
  snapshot_ = {};
  SetVisible(false);
  view_->InvalidateLayout();
}
void PinnedPeek::Expand() {
  auto* tab = preview();
  Hide();
  if (tab) {
    view_->browser()->GetTabStripModel()->NotifyTabChanged(tab,
                                                           TabChangeType::kAll);
  }
}
void PinnedPeek::Close() {
  auto* model = view_->browser()->GetTabStripModel();
  if (preview() && preview()->IsActivated()) {
    model->CloseWebContentsAt(model->GetIndexOfTab(preview()),
                              TabCloseTypes::CLOSE_USER_GESTURE |
                                  TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
  }
}
bool PinnedPeek::AcceleratorPressed(const ui::Accelerator&) {
  if (!GetVisible()) {
    return false;
  }
  Close();
  return true;
}
gfx::Rect PinnedPeek::PageBounds(gfx::Rect area) {
  const int x = std::min(64, area.width() / 12);
  const int y = std::min(48, area.height() / 14);
  area.Inset(gfx::Insets::TLBR(y + kHeader, x, y, x));
  return area;
}
bool PinnedPeek::DoesIntersectRect(const views::View*,
                                   const gfx::Rect& rect) const {
  return !PageBounds(GetLocalBounds()).Contains(rect);
}
bool PinnedPeek::OnMousePressed(const ui::MouseEvent& event) {
  auto card = PageBounds(GetLocalBounds());
  card.Inset(gfx::Insets::TLBR(-kHeader, 0, 0, 0));
  if (!card.Contains(event.location())) {
    Close();
  }
  return true;
}
void PinnedPeek::Layout(PassKey) {
  const auto page = PageBounds(GetLocalBounds());
  close_->SetBounds(page.right() - 34, page.y() - kHeader + 4, 28, 28);
  expand_->SetBounds(page.right() - 68, page.y() - kHeader + 4, 28, 28);
  for (auto* button : {expand_.get(), close_.get()}) {
    button->SetColors(view_->theme().on_surface,
                      view_->theme().OnSurface(0.08f));
  }
}
void PinnedPeek::OnPaint(gfx::Canvas* canvas) {
  const auto page = PageBounds(GetLocalBounds());
  canvas->Save();
  canvas->ClipRect(page, SkClipOp::kDifference);
  if (!snapshot_.isNull()) {
    canvas->DrawImageInt(snapshot_, 0, 0, snapshot_.width(), snapshot_.height(),
                         0, 0, width(), height(), true);
  }
  canvas->FillRect(GetLocalBounds(), SkColorSetA(SK_ColorBLACK, 100));
  gfx::Rect header(page.x(), page.y() - kHeader, page.width(), kHeader);
  canvas->FillRect(header, view_->theme().surface_panel);
  header.Inset(gfx::Insets::TLBR(0, 12, 0, 74));
  if (auto* tab = preview()) {
    canvas->DrawStringRectWithFlags(url_formatter::FormatUrlForSecurityDisplay(
                                        tab->GetContents()->GetVisibleURL()),
                                    UiFont(13), view_->theme().on_surface,
                                    header, gfx::Canvas::TEXT_ALIGN_LEFT);
  }
  canvas->Restore();
}
BEGIN_METADATA(PinnedPeek)
END_METADATA
}  // namespace starry
