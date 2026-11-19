// Copyright 2026 The Starry Browser Authors

// 网页的 alert / confirm / prompt：Chrome 的 JavaScriptTabModalDialogManager 管什么时候
// 弹、什么时候关（标签切走、关掉、网页跳走），界面是 Starry 的对话框
// （starry_hooks.h 的 CreateJavaScriptDialog）。

#include <memory>
#include <string>
#include <utility>

#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/javascript_dialogs/tab_modal_dialog_view.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/agent/page_dialogs.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "ui/views/view_tracker.h"

namespace starry {

namespace {

class JavaScriptDialog;

// 对话框显示在窗口里时交给对话框层（DialogHostView）的那一张：对话框层拥有它，按钮、
// Esc 转给 JavaScriptDialog。网页的对话框先没了时它什么也不做。
class DialogCard : public DialogRequest {
 public:
  DialogCard(base::WeakPtr<JavaScriptDialog> dialog, DialogSpec spec)
      : dialog_(std::move(dialog)), spec_(std::move(spec)) {}

  // DialogRequest:
  const DialogSpec& spec() const override { return spec_; }
  void Answer(size_t button, const std::u16string& input) override;
  void Dismissed() override;

 private:
  base::WeakPtr<JavaScriptDialog> dialog_;
  const DialogSpec spec_;
};

// 网页的一个对话框。标签显示着（是它的窗口里的当前标签，或者分屏里的一格）时才放进那个
// 窗口的对话框层：智能体在后台的标签里做事时 Chrome 因为挂着调试器（CDP）不压掉后台
// 标签的对话框，这些先不显示，免得盖在用户正在看的网页上；智能体用
// browser_handle_dialog 回答它（agent/page_dialogs.h），用户切过去时再显示。显示着时
// 用户切走也先收起来，切回来再显示（挂着调试器时 Chrome 不替它关掉）。
//
// 网页拥有它（JavaScriptDialogHolder）；回答了、被关掉了就晚一点删掉。
class JavaScriptDialog : public javascript_dialogs::TabModalDialogView,
                         public content::WebContentsObserver {
 public:
  JavaScriptDialog(
      content::WebContents* contents,
      tabs::TabInterface* tab,
      const std::u16string& title,
      content::JavaScriptDialogType type,
      const std::u16string& message,
      const std::u16string& default_prompt,
      content::JavaScriptDialogManager::DialogClosedCallback callback,
      base::OnceClosure force_closed)
      : content::WebContentsObserver(contents),
        callback_(std::move(callback)),
        force_closed_(std::move(force_closed)),
        modal_ui_(tab->ShowModalUI()) {
    spec_.icon = &icons::kGlobe;
    spec_.title = title;
    spec_.message = message;
    if (type == content::JAVASCRIPT_DIALOG_TYPE_PROMPT) {
      spec_.field = default_prompt;
    }
    if (type != content::JAVASCRIPT_DIALOG_TYPE_ALERT) {
      spec_.buttons.push_back(
          {L("common.cancel"), ButtonVariant::kTertiary, true});
    }
    spec_.buttons.push_back({L("common.ok"), ButtonVariant::kFilled, false});
    agent::PageDialog page_dialog;
    page_dialog.type = type;
    page_dialog.message = message;
    agent::PageDialogOpened(contents, std::move(page_dialog));
  }

  ~JavaScriptDialog() override {
    Withdraw();
    if (!finished_ && web_contents()) {
      agent::PageDialogClosed(web_contents());
    }
  }

  base::WeakPtr<JavaScriptDialog> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // 标签显示着时放进它的窗口。
  void ShowIfVisible() {
    content::WebContents* contents = web_contents();
    if (finished_ || card_ || !contents ||
        contents->GetVisibility() == content::Visibility::HIDDEN) {
      return;
    }
    tabs::TabInterface* tab =
        tabs::TabInterface::MaybeGetFromContents(contents);
    BrowserWindowInterface* browser =
        tab ? tab->GetBrowserWindowInterface() : nullptr;
    StarryBrowserView* view =
        browser ? StarryBrowserView::From(browser) : nullptr;
    if (!view || !view->dialog_host()) {
      return;
    }
    auto card = std::make_unique<DialogCard>(GetWeakPtr(), spec_);
    card_ = card.get();
    host_.SetView(view->dialog_host());
    view->dialog_host()->Present(std::move(card));
  }

  // 用户在卡片上点了按钮（或 Esc）。
  void Answered(bool accepted, const std::u16string& input) {
    card_ = nullptr;
    host_.SetView(nullptr);
    // 用户答了：不是被强制关掉的。
    force_closed_.Reset();
    content::JavaScriptDialogManager::DialogClosedCallback callback =
        std::move(callback_);
    Finish();
    if (callback) {
      std::move(callback).Run(accepted, accepted ? input : std::u16string());
    }
  }

  // 卡片没回答就被拿掉了（窗口关了）。
  void Dismissed() {
    card_ = nullptr;
    host_.SetView(nullptr);
    callback_.Reset();
    base::OnceClosure force_closed = std::move(force_closed_);
    Finish();
    if (force_closed) {
      std::move(force_closed).Run();
    }
  }

  // javascript_dialogs::TabModalDialogView:
  void CloseDialogWithoutCallback() override {
    callback_.Reset();
    force_closed_.Reset();
    Finish();
  }

  std::u16string GetUserInput() override {
    DialogHostView* host = static_cast<DialogHostView*>(host_.view());
    return card_ && host && host->current() == card_
               ? host->CurrentInput()
               : spec_.field.value_or(u"");
  }

  // content::WebContentsObserver:
  void OnVisibilityChanged(content::Visibility visibility) override {
    if (visibility == content::Visibility::HIDDEN) {
      Withdraw();
    } else {
      ShowIfVisible();
    }
  }

  void WebContentsDestroyed() override { Finish(); }

 private:
  // 卡片从对话框层拿下来（不回答）。
  void Withdraw() {
    DialogHostView* host = static_cast<DialogHostView*>(host_.view());
    if (card_ && host) {
      // 会把卡片删掉（晚一点）。
      host->Remove(card_);
    }
    card_ = nullptr;
    host_.SetView(nullptr);
  }

  // 结束了：收起卡片，告诉智能体对话框没了，晚一点删掉自己。
  void Finish();

  DialogSpec spec_;
  content::JavaScriptDialogManager::DialogClosedCallback callback_;
  base::OnceClosure force_closed_;
  std::unique_ptr<tabs::ScopedTabModalUI> modal_ui_;
  views::ViewTracker host_;
  // 对话框层拥有它。
  raw_ptr<DialogCard> card_ = nullptr;
  bool finished_ = false;
  base::WeakPtrFactory<JavaScriptDialog> weak_factory_{this};
};

void DialogCard::Answer(size_t button, const std::u16string& input) {
  if (dialog_) {
    dialog_->Answered(!spec_.buttons[button].cancel, input);
  }
}

void DialogCard::Dismissed() {
  if (dialog_) {
    dialog_->Dismissed();
  }
}

// 网页现在的对话框（一个网页同时只有一个：TabModalDialogManager 只管一个）。
class JavaScriptDialogHolder
    : public content::WebContentsUserData<JavaScriptDialogHolder> {
 public:
  ~JavaScriptDialogHolder() override = default;

  JavaScriptDialog* Set(std::unique_ptr<JavaScriptDialog> dialog) {
    dialog_ = std::move(dialog);
    return dialog_.get();
  }

  // 删掉 dialog（还是现在这个时）：晚一点，它可能正在自己的方法里。
  void Release(JavaScriptDialog* dialog) {
    if (dialog_.get() == dialog) {
      base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
          FROM_HERE, std::move(dialog_));
    }
  }

 private:
  friend class content::WebContentsUserData<JavaScriptDialogHolder>;
  explicit JavaScriptDialogHolder(content::WebContents* contents)
      : content::WebContentsUserData<JavaScriptDialogHolder>(*contents) {}

  std::unique_ptr<JavaScriptDialog> dialog_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(JavaScriptDialogHolder);

void JavaScriptDialog::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  Withdraw();
  modal_ui_.reset();
  content::WebContents* contents = web_contents();
  Observe(nullptr);
  if (!contents) {
    return;
  }
  agent::PageDialogClosed(contents);
  if (JavaScriptDialogHolder* holder =
          JavaScriptDialogHolder::FromWebContents(contents)) {
    holder->Release(this);
  }
}

}  // namespace

base::WeakPtr<javascript_dialogs::TabModalDialogView> CreateJavaScriptDialog(
    content::WebContents* parent_web_contents,
    const std::u16string& title,
    content::JavaScriptDialogType dialog_type,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    content::JavaScriptDialogManager::DialogClosedCallback& dialog_callback,
    base::OnceClosure& dialog_force_closed_callback) {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(parent_web_contents);
  if (!tab || !tab->CanShowModalUI()) {
    return nullptr;
  }
  BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
  StarryBrowserView* view = browser ? StarryBrowserView::From(browser) : nullptr;
  if (!view || !view->dialog_host()) {
    return nullptr;
  }
  JavaScriptDialogHolder::CreateForWebContents(parent_web_contents);
  JavaScriptDialog* dialog =
      JavaScriptDialogHolder::FromWebContents(parent_web_contents)
          ->Set(std::make_unique<JavaScriptDialog>(
              parent_web_contents, tab, title, dialog_type, message_text,
              default_prompt_text, std::move(dialog_callback),
              std::move(dialog_force_closed_callback)));
  dialog->ShowIfVisible();
  return dialog->GetWeakPtr();
}

}  // namespace starry
