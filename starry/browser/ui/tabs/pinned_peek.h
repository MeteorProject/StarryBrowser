// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_PINNED_PEEK_H_
#define STARRY_BROWSER_UI_TABS_PINNED_PEEK_H_

#include "base/callback_list.h"
#include "base/memory/weak_ptr.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

namespace content {
class NavigationThrottleRegistry;
struct OpenURLParams;
}  // namespace content
namespace starry {
class GlyphButton;
class StarryBrowserView;

// Peek uses a real foreground tab. Chrome continues to own navigation, site
// permissions, downloads, beforeunload and the page's browsing state.
class PinnedPeek : public views::View, public views::ViewTargeterDelegate {
  METADATA_HEADER(PinnedPeek, views::View)
 public:
  explicit PinnedPeek(StarryBrowserView* view);
  ~PinnedPeek() override;
  static void MaybeAddThrottle(content::NavigationThrottleRegistry& registry);
  static void LinkOpened(content::WebContents* source,
                         content::WebContents* target,
                         const GURL& url);
  void Open(tabs::TabHandle source, content::OpenURLParams params);
  void Present(tabs::TabInterface* source, tabs::TabInterface* preview);
  void Expand();
  void Close();
  tabs::TabInterface* preview() const { return preview_.Get(); }
  tabs::TabInterface* source() const { return source_.Get(); }
  static gfx::Rect PageBounds(gfx::Rect area);
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override;
  bool DoesIntersectRect(const views::View*,
                         const gfx::Rect& rect) const override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnPaint(gfx::Canvas* canvas) override;
  void Layout(PassKey) override;

 private:
  void Hide();
  void CaptureBackdrop(tabs::TabInterface* source);
  raw_ptr<StarryBrowserView> view_;
  tabs::TabHandle source_;
  tabs::TabHandle preview_;
  raw_ptr<GlyphButton> close_;
  raw_ptr<GlyphButton> expand_;
  gfx::ImageSkia snapshot_;
  base::CallbackListSubscription deactivation_;
  base::CallbackListSubscription source_detach_;
  base::WeakPtrFactory<PinnedPeek> weak_{this};
};
}  // namespace starry
#endif  // STARRY_BROWSER_UI_TABS_PINNED_PEEK_H_
