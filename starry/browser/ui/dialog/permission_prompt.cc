// Copyright 2026 The Starry Browser Authors
//
// 网站的权限请求（摄像头、麦克风、位置、通知……）用 Starry 的对话框询问，换掉 Chrome 挂在
// 地址栏上的权限气泡和小条。请求的排队、记住选择、何时弹出仍由 Chrome 的
// PermissionRequestManager 负责。

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/permissions/permission_prompt.h"
#include "components/permissions/permission_request.h"
#include "components/permissions/permission_uma_util.h"
#include "components/permissions/permission_util.h"
#include "components/permissions/request_type.h"
#include "components/tabs/public/tab_interface.h"
#include "components/url_formatter/elide_url.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace starry {

namespace {

using permissions::PermissionPrompt;
using permissions::RequestType;

// 刚弹出来的这一会儿不接受「允许」（同 Chrome 权限气泡的 input protection）。
constexpr base::TimeDelta kInputProtection = base::Milliseconds(500);

// 「想要……」后面那一截（各语言的写法要能接在 permissions.prompt.title 的 {action} 里）。没列出来的用 Chrome 自己的说法（跟着 Chrome 的界面语言）。
std::u16string Action(const permissions::PermissionRequest& request) {
  switch (request.request_type()) {
    case RequestType::kCameraStream:
      return L("permissions.action.camera");
    case RequestType::kMicStream:
      return L("permissions.action.microphone");
    case RequestType::kGeolocation:
      return L("permissions.action.location");
    case RequestType::kNotifications:
      return L("permissions.action.notifications");
    case RequestType::kClipboard:
      return L("permissions.action.clipboard");
    case RequestType::kMidiSysex:
      return L("permissions.action.midiSysex");
    case RequestType::kMultipleDownloads:
      return L("permissions.action.multipleDownloads");
    case RequestType::kIdleDetection:
      return L("permissions.action.idleDetection");
    case RequestType::kSensors:
      return L("permissions.action.sensors");
    case RequestType::kWindowManagement:
      return L("permissions.action.windowManagement");
    case RequestType::kVrSession:
      return L("permissions.action.vrSession");
    case RequestType::kArSession:
      return L("permissions.action.arSession");
    case RequestType::kHandTracking:
      return L("permissions.action.handTracking");
    case RequestType::kLocalNetwork:
      return L("permissions.action.localNetwork");
#if !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_IOS)
    case RequestType::kCameraPanTiltZoom:
      return L("permissions.action.cameraPanTiltZoom");
    case RequestType::kLocalFonts:
      return L("permissions.action.localFonts");
    case RequestType::kKeyboardLock:
      return L("permissions.action.keyboardLock");
    case RequestType::kPointerLock:
      return L("permissions.action.pointerLock");
#endif
    default:
      return request.GetMessageTextFragment();
  }
}

const gfx::VectorIcon* IconFor(const permissions::PermissionRequest& request) {
  switch (request.request_type()) {
    case RequestType::kCameraStream:
#if !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_IOS)
    case RequestType::kCameraPanTiltZoom:
#endif
      return &icons::kVideoFill;
    case RequestType::kMicStream:
      return &icons::kMicFill;
    case RequestType::kGeolocation:
      return &icons::kLocationFill;
    case RequestType::kNotifications:
      return &icons::kBellFill;
    case RequestType::kMultipleDownloads:
      return &icons::kArrowDownCircle;
    default:
      return &icons::kLockFill;
  }
}

// 只有这一次（摄像头、麦克风、位置这类可以临时允许的）。
bool CanAllowOnce(const PermissionPrompt::Delegate& delegate) {
  for (const auto& request : delegate.Requests()) {
    const std::optional<ContentSettingsType> type =
        permissions::RequestTypeToContentSettingsType(request->request_type());
    if (!type || !permissions::PermissionUtil::DoesSupportTemporaryGrants(*type)) {
      return false;
    }
  }
  return true;
}

DialogSpec SpecFor(PermissionPrompt::Delegate& delegate, bool allow_once) {
  const auto& requests = delegate.Requests();
  // 摄像头和麦克风一起要时合成一句。
  bool camera = false;
  bool mic = false;
  std::vector<std::u16string> actions;
  for (const auto& request : requests) {
    camera |= request->request_type() == RequestType::kCameraStream;
    mic |= request->request_type() == RequestType::kMicStream;
  }
  for (const auto& request : requests) {
    const RequestType type = request->request_type();
    if (camera && mic &&
        (type == RequestType::kCameraStream || type == RequestType::kMicStream)) {
      if (type == RequestType::kCameraStream) {
        actions.push_back(L("permissions.action.cameraAndMicrophone"));
      }
      continue;
    }
    actions.push_back(Action(*request));
  }

  const std::u16string origin = url_formatter::FormatUrlForSecurityDisplay(
      delegate.GetRequestingOrigin(),
      url_formatter::SchemeDisplay::OMIT_HTTP_AND_HTTPS);
  DialogSpec spec;
  spec.icon = IconFor(*requests.front());
  spec.tone = DialogSpec::Tone::kAccent;
  if (actions.size() == 1) {
    spec.title = L("permissions.prompt.title",
                   {{"origin", origin}, {"action", actions.front()}});
    spec.message = L("permissions.prompt.footer");
  } else {
    spec.title = L("permissions.prompt.titleMultiple", {{"origin", origin}});
    std::vector<std::u16string> lines;
    for (const std::u16string& action : actions) {
      lines.push_back(base::StrCat({u"·  ", action}));
    }
    spec.message = base::StrCat({base::JoinString(lines, u"\n"), u"\n\n",
                                 L("permissions.prompt.footer")});
  }
  spec.buttons.push_back(
      {L("window.mediaCapture.deny"), ButtonVariant::kTertiary,
       /*cancel=*/true});
  if (allow_once) {
    spec.buttons.push_back(
        {L("permissions.prompt.allowOnce"), ButtonVariant::kSecondary});
  }
  spec.buttons.push_back({L("common.allow"), ButtonVariant::kFilled});
  // 回车不允许：用户正打着字时弹出来，不会被顺手允许。
  spec.no_default_button = true;
  spec.input_protection = kInputProtection;
  return spec;
}

class StarryPermissionPrompt;

// 对话框层排队、显示的那一项；拥有它的是 DialogHostView。
class PermissionDialogRequest : public DialogRequest {
 public:
  PermissionDialogRequest(DialogSpec spec,
                          base::WeakPtr<StarryPermissionPrompt> prompt);
  ~PermissionDialogRequest() override;

  const DialogSpec& spec() const override { return spec_; }
  void Answer(size_t button, const std::u16string& input) override;
  void Dismissed() override;

 private:
  DialogSpec spec_;
  base::WeakPtr<StarryPermissionPrompt> prompt_;
};

class StarryPermissionPrompt : public PermissionPrompt {
 public:
  StarryPermissionPrompt(StarryBrowserView* view, Delegate* delegate)
      : view_(view),
        delegate_(delegate),
        allow_once_(CanAllowOnce(*delegate)) {
    auto request = std::make_unique<PermissionDialogRequest>(
        SpecFor(*delegate, allow_once_), weak_factory_.GetWeakPtr());
    request_ = request.get();
    view->dialog_host()->Present(std::move(request));
  }

  StarryPermissionPrompt(const StarryPermissionPrompt&) = delete;
  StarryPermissionPrompt& operator=(const StarryPermissionPrompt&) = delete;

  ~StarryPermissionPrompt() override {
    // 换了标签、请求被别处处理掉了：对话框拿掉，不回答。
    if (request_ && view_ && view_->dialog_host()) {
      view_->dialog_host()->Remove(request_);
    }
  }

  void OnAnswer(size_t button) {
    request_ = nullptr;
    // 按钮：不允许、[仅这一次、]允许。
    const size_t allow = allow_once_ ? 2 : 1;
    const ::PromptOptions options = std::monostate();
    if (button == allow) {
      delegate_->Accept(options);
    } else if (allow_once_ && button == 1) {
      delegate_->AcceptThisTime(options);
    } else {
      delegate_->Deny(options);
    }
  }

  void OnDismissed() {
    request_ = nullptr;
    delegate_->Dismiss(std::monostate());
  }

  void OnRequestGone(PermissionDialogRequest* request) {
    if (request_ == request) {
      request_ = nullptr;
    }
  }

  // PermissionPrompt:
  bool UpdateAnchor() override { return true; }
  TabSwitchingBehavior GetTabSwitchingBehavior() override {
    // 对话框是整个窗口的：切到别的标签时先拿掉，切回来再问。
    return kDestroyPromptButKeepRequestPending;
  }
  permissions::PermissionPromptDisposition GetPromptDisposition()
      const override {
    return permissions::PermissionPromptDisposition::ANCHORED_BUBBLE;
  }
  bool IsAskPrompt() const override { return true; }
  std::optional<gfx::Rect> GetViewBoundsInScreen() const override {
    return std::nullopt;
  }
  std::vector<permissions::ElementAnchoredBubbleVariant> GetPromptVariants()
      const override {
    return {};
  }
  std::optional<permissions::feature_params::PermissionElementPromptPosition>
  GetPromptPosition() const override {
    return std::nullopt;
  }

 private:
  raw_ptr<StarryBrowserView> view_;
  raw_ptr<Delegate> delegate_;
  const bool allow_once_;
  raw_ptr<PermissionDialogRequest> request_ = nullptr;
  base::WeakPtrFactory<StarryPermissionPrompt> weak_factory_{this};
};

PermissionDialogRequest::PermissionDialogRequest(
    DialogSpec spec,
    base::WeakPtr<StarryPermissionPrompt> prompt)
    : spec_(std::move(spec)), prompt_(std::move(prompt)) {}

PermissionDialogRequest::~PermissionDialogRequest() {
  if (prompt_) {
    prompt_->OnRequestGone(this);
  }
}

void PermissionDialogRequest::Answer(size_t button,
                                     const std::u16string& input) {
  if (prompt_) {
    prompt_->OnAnswer(button);
  }
}

void PermissionDialogRequest::Dismissed() {
  if (prompt_) {
    prompt_->OnDismissed();
  }
}

}  // namespace

std::unique_ptr<permissions::PermissionPrompt> CreatePermissionPrompt(
    content::WebContents* web_contents,
    permissions::PermissionPrompt::Delegate* delegate) {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_contents);
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  StarryBrowserView* view = browser ? StarryBrowserView::From(browser) : nullptr;
  if (!view || !view->dialog_host() || delegate->Requests().empty()) {
    return nullptr;
  }
  return std::make_unique<StarryPermissionPrompt>(view, delegate);
}

}  // namespace starry
