// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/page_dialogs.h"

#include <map>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "components/javascript_dialogs/tab_modal_dialog_manager.h"

namespace starry::agent {

namespace {

// 只在 UI 线程上用。键只用来认是哪个网页，不解引用（网页关掉前会先拿掉）。
std::map<content::WebContents*, PageDialog>& Dialogs() {
  static base::NoDestructor<std::map<content::WebContents*, PageDialog>> map;
  return *map;
}

base::RepeatingCallbackList<void(content::WebContents*)>& Observers() {
  static base::NoDestructor<
      base::RepeatingCallbackList<void(content::WebContents*)>>
      list;
  return *list;
}

}  // namespace

const PageDialog* FindPageDialog(content::WebContents* contents) {
  auto it = Dialogs().find(contents);
  return it == Dialogs().end() ? nullptr : &it->second;
}

bool AnswerPageDialog(content::WebContents* contents,
                      bool accept,
                      const std::u16string* prompt_text) {
  if (!FindPageDialog(contents)) {
    return false;
  }
  auto* manager =
      javascript_dialogs::TabModalDialogManager::FromWebContents(contents);
  // 登记着的对话框都是经 TabModalDialogManager 建出来的（它的 dialog_ 就是那一个），
  // 不给文字时它取输入框里现在的字。
  return manager &&
         manager->HandleJavaScriptDialog(contents, accept, prompt_text);
}

const char* PageDialogKind(const PageDialog& dialog) {
  switch (dialog.type) {
    case content::JAVASCRIPT_DIALOG_TYPE_ALERT:
      return "alert";
    case content::JAVASCRIPT_DIALOG_TYPE_CONFIRM:
      return "confirm";
    case content::JAVASCRIPT_DIALOG_TYPE_PROMPT:
      return "prompt";
  }
  return "alert";
}

std::string PageDialogNotice(const PageDialog& dialog) {
  std::u16string message = dialog.message;
  if (message.size() > 500) {
    message = message.substr(0, 500) + u"…";
  }
  return base::StrCat({"The page opened a JavaScript ", PageDialogKind(dialog),
                       " dialog: \"", base::UTF16ToUTF8(message),
                       "\". Respond with browser_handle_dialog before doing "
                       "anything else in this tab."});
}

base::CallbackListSubscription AddPageDialogObserver(
    base::RepeatingCallback<void(content::WebContents*)> callback) {
  return Observers().Add(std::move(callback));
}

void PageDialogOpened(content::WebContents* contents, PageDialog dialog) {
  Dialogs()[contents] = std::move(dialog);
  Observers().Notify(contents);
}

void PageDialogClosed(content::WebContents* contents) {
  if (Dialogs().erase(contents)) {
    Observers().Notify(contents);
  }
}

}  // namespace starry::agent
