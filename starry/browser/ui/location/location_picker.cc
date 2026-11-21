// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/location/location_picker.h"

#include <memory>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "chrome/browser/profiles/profile.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/privacy/location_simulation.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/controls/webview/webview.h"
#include "url/gurl.h"

namespace starry {

namespace {

constexpr int kPickerWidth = 760;
constexpr int kPickerHeight = 540;
constexpr char kPickerUrl[] = "chrome://location-picker/";

// 同 AI 助手的岛（assistant_island.cc）：网页没处理的键交给窗口和菜单栏，⌘C、⌘V、
// ⌘A、⌘Z 才能用（macOS 上它们经菜单栏的「编辑」发回网页）。
class PickerWebView : public views::WebView {
  METADATA_HEADER(PickerWebView, views::WebView)

 public:
  using views::WebView::WebView;

  bool HandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override {
    views::FocusManager* focus_manager = GetFocusManager();
    return focus_manager &&
           unhandled_keyboard_event_handler_.HandleKeyboardEvent(event,
                                                                 focus_manager);
  }

 private:
  views::UnhandledKeyboardEventHandler unhandled_keyboard_event_handler_;
};

BEGIN_METADATA(PickerWebView)
END_METADATA

class LocationPickerRequest;

// 挂在选点页的 WebContents 上：选点页的 handler 由此找到网站和对话框。
class PickerOwnerData : public content::WebContentsUserData<PickerOwnerData>,
                        public LocationPickerOwner {
 public:
  ~PickerOwnerData() override = default;

  const std::string& site() const override { return site_; }
  StarryBrowserView* browser_view() const override { return view_; }
  void Finish(bool saved) override;

 private:
  friend class content::WebContentsUserData<PickerOwnerData>;

  PickerOwnerData(content::WebContents* contents,
                  std::string site,
                  StarryBrowserView* view,
                  base::WeakPtr<LocationPickerRequest> request)
      : content::WebContentsUserData<PickerOwnerData>(*contents),
        site_(std::move(site)),
        view_(view),
        request_(std::move(request)) {}

  const std::string site_;
  // 选点页的 WebContents 在这个窗口的视图里，比它先没。
  const raw_ptr<StarryBrowserView> view_;
  base::WeakPtr<LocationPickerRequest> request_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PickerOwnerData);

class LocationPickerRequest : public DialogRequest {
 public:
  LocationPickerRequest(StarryBrowserView* view,
                        std::string site,
                        LocationPickedCallback callback)
      : view_(view), site_(std::move(site)), callback_(std::move(callback)) {
    spec_.title = L("location.picker.windowTitle");
    spec_.content_size = gfx::Size(kPickerWidth, kPickerHeight);
  }

  // DialogRequest:
  const DialogSpec& spec() const override { return spec_; }
  void Answer(size_t button, const std::u16string& input) override {}
  void Dismissed() override {
    if (callback_) {
      std::move(callback_).Run(false);
    }
  }

  std::unique_ptr<views::View> CreateContentView() override {
    auto web_view =
        std::make_unique<PickerWebView>(view_->browser()->GetProfile());
    content::WebContents* contents = web_view->GetWebContents();
    content::WebContentsUserData<PickerOwnerData>::CreateForWebContents(
        contents, site_, view_.get(), weak_factory_.GetWeakPtr());
    // 地图载出来之前透出卡片的玻璃。
    contents->SetPageBaseBackgroundColor(SK_ColorTRANSPARENT);
    web_view->LoadInitialURL(GURL(base::StrCat(
        {kPickerUrl, site_.empty() ? "" : "?site=",
         base::EscapeQueryParamValue(site_, /*use_plus=*/false)})));
    return web_view;
  }

  void Finish(bool saved) {
    LocationPickedCallback callback = std::move(callback_);
    // 卡片淡出以后才删（网页正在调用这里，WebContents 不能马上没）；本对象晚一点删。
    view_->dialog_host()->Remove(this);
    if (callback) {
      std::move(callback).Run(saved);
    }
  }

 private:
  const raw_ptr<StarryBrowserView> view_;
  const std::string site_;
  LocationPickedCallback callback_;
  DialogSpec spec_;
  base::WeakPtrFactory<LocationPickerRequest> weak_factory_{this};
};

void PickerOwnerData::Finish(bool saved) {
  if (request_) {
    request_->Finish(saved);
  }
}

}  // namespace

void ShowLocationPicker(BrowserWindowInterface* browser,
                        std::optional<std::string> site,
                        LocationPickedCallback callback) {
  StarryBrowserView* view =
      browser ? StarryBrowserView::From(browser) : nullptr;
  std::string normalized = site ? NormalizeLocationSiteInput(*site) : "";
  if (!view || !view->dialog_host() || (site && normalized.empty())) {
    if (callback) {
      std::move(callback).Run(false);
    }
    return;
  }
  view->dialog_host()->Present(std::make_unique<LocationPickerRequest>(
      view, std::move(normalized), std::move(callback)));
}

// static
LocationPickerOwner* LocationPickerOwner::FromWebContents(
    content::WebContents* contents) {
  return contents
             ? content::WebContentsUserData<PickerOwnerData>::FromWebContents(
                   contents)
             : nullptr;
}

}  // namespace starry
