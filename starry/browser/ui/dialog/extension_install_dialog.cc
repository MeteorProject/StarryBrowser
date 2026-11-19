// Copyright 2026 The Starry Browser Authors
//
// 安装扩展、批准新权限的确认：用 Starry 的对话框问，换掉 Chrome 的扩展安装对话框。
// 从应用店安装、装 .crx、新版本要更多权限、扩展请求可选权限、别的程序装上的扩展都走这里。
// 家长、管理员审批这类要填理由的还是 Chrome 的。

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/extensions/extension_install_prompt_show_params.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/install_prompt_data.h"
#include "extensions/common/extension.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"

namespace starry {

namespace {

using extensions::ExtensionInstallPromptClient;
using extensions::InstallPromptData;

// 刚弹出来的这一会儿不接受「添加」（同 Chrome 安装对话框推迟启用按钮）。
constexpr base::TimeDelta kInputProtection = base::Milliseconds(500);

StarryBrowserView* ViewFor(ExtensionInstallPromptShowParams& params) {
  if (content::WebContents* contents = params.GetParentWebContents()) {
    if (tabs::TabInterface* tab =
            tabs::TabInterface::MaybeGetFromContents(contents)) {
      if (BrowserWindowInterface* browser = tab->GetBrowserWindowInterface()) {
        return StarryBrowserView::From(browser);
      }
    }
  }
  if (gfx::NativeWindow window = params.GetParentWindow()) {
    return views::AsViewClass<StarryBrowserView>(
        BrowserView::GetBrowserViewForNativeWindow(window));
  }
  return nullptr;
}

std::optional<DialogSpec> SpecFor(const InstallPromptData& prompt) {
  if (!prompt.extension() || prompt.requires_parent_permission()) {
    return std::nullopt;
  }
  const std::u16string name = base::UTF8ToUTF16(prompt.extension()->name());
  DialogSpec spec;
  spec.icon = &icons::kPuzzlepiece;
  spec.tone = DialogSpec::Tone::kAccent;
  std::u16string accept = L("app.extensionInstall.confirm");
  std::u16string cancel = L("common.cancel");
  switch (prompt.type()) {
    case InstallPromptData::INSTALL_PROMPT:
    case InstallPromptData::REMOTE_INSTALL_PROMPT:
    case InstallPromptData::REPAIR_PROMPT:
      spec.title = L("app.extensionInstall.title", {{"name", name}});
      break;
    case InstallPromptData::RE_ENABLE_PROMPT:
      spec.title = L("app.extensionReenable.title", {{"name", name}});
      accept = L("app.extensionReenable.confirm");
      break;
    case InstallPromptData::PERMISSIONS_PROMPT:
      spec.title = L("app.extensionPermissions.title", {{"name", name}});
      accept = L("common.allow");
      cancel = L("app.extensionPermissions.deny");
      break;
    case InstallPromptData::EXTERNAL_INSTALL_PROMPT:
      spec.title = L("app.extensionExternal.title", {{"name", name}});
      accept = L("app.extensionExternal.enable");
      cancel = L("common.remove");
      break;
    default:
      return std::nullopt;
  }
  const size_t count = prompt.GetPermissionCount();
  if (count == 0) {
    spec.message = L("app.extensionInstall.noPermissions");
  } else {
    std::vector<std::u16string> lines = {
        prompt.type() == InstallPromptData::PERMISSIONS_PROMPT
            ? L("app.extensionPermissions.listHeader")
            : L("app.extensionInstall.listHeader")};
    for (size_t i = 0; i < count; ++i) {
      lines.push_back(base::StrCat({u"·  ", prompt.GetPermission(i)}));
    }
    spec.message = base::JoinString(lines, u"\n");
  }
  spec.buttons = {
      {cancel, ButtonVariant::kTertiary, /*cancel=*/true},
      {accept, ButtonVariant::kFilled},
  };
  spec.no_default_button = true;
  spec.input_protection = kInputProtection;
  return spec;
}

}  // namespace

bool ShowExtensionInstallDialog(
    std::unique_ptr<ExtensionInstallPromptShowParams>& show_params,
    ExtensionInstallPromptClient::DoneCallback& done_callback,
    std::unique_ptr<InstallPromptData>& prompt) {
  StarryBrowserView* view = ViewFor(*show_params);
  if (!view || !view->dialog_host()) {
    return false;
  }
  std::optional<DialogSpec> spec = SpecFor(*prompt);
  if (!spec) {
    return false;
  }
  prompt->OnDialogOpened();
  view->PresentDialog(
      std::move(*spec),
      base::BindOnce(
          [](std::unique_ptr<ExtensionInstallPromptShowParams> show_params,
             ExtensionInstallPromptClient::DoneCallback done_callback,
             std::unique_ptr<InstallPromptData> prompt,
             std::optional<size_t> button, const std::u16string& input) {
            using Result = ExtensionInstallPromptClient::Result;
            Result result = Result::USER_CANCELED;
            if (button == 1u) {
              result = Result::ACCEPTED;
              prompt->OnDialogAccepted();
            } else {
              prompt->OnDialogCanceled();
              if (!button) {
                result = Result::ABORTED;
              }
            }
            std::move(done_callback)
                .Run(ExtensionInstallPromptClient::DoneCallbackPayload(result));
          },
          std::move(show_params), std::move(done_callback), std::move(prompt)));
  return true;
}

}  // namespace starry
