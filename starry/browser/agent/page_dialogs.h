// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_PAGE_DIALOGS_H_
#define STARRY_BROWSER_AGENT_PAGE_DIALOGS_H_

#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "content/public/browser/javascript_dialog_manager.h"

namespace content {
class WebContents;
}

// 网页打开的 alert / confirm / prompt：弹出时 ui/dialog/javascript_dialog.cc 登记在
// 这里，回答了、网页跳走了、标签关了就拿掉。智能体的工具看它：有对话框的标签上先要
// browser_handle_dialog（网页的脚本停在对话框上，别的调用只会等到超时）；动作弹出了
// 对话框时不再等下去，把对话框的内容告诉模型。
namespace starry::agent {

struct PageDialog {
  content::JavaScriptDialogType type = content::JAVASCRIPT_DIALOG_TYPE_ALERT;
  std::u16string message;
};

// 这个网页现在开着的对话框；没有时为空。
const PageDialog* FindPageDialog(content::WebContents* contents);
// 回答它（Chrome 的 JavaScriptTabModalDialogManager 关掉对话框、把结果交给网页）：
// accept 是「确定」；prompt_text 是 prompt 里填的字，空指针时用输入框里现在的字。没有对话框
// 时返回 false。
bool AnswerPageDialog(content::WebContents* contents,
                      bool accept,
                      const std::u16string* prompt_text);
// 对话框的种类（alert、confirm、prompt）。
const char* PageDialogKind(const PageDialog& dialog);
// 给模型看的说明（英文）：对话框的内容，先用 browser_handle_dialog 回答。
std::string PageDialogNotice(const PageDialog& dialog);
// 哪个网页的对话框开了、关了。
base::CallbackListSubscription AddPageDialogObserver(
    base::RepeatingCallback<void(content::WebContents*)> callback);

// javascript_dialog.cc 用：登记、拿掉。
void PageDialogOpened(content::WebContents* contents, PageDialog dialog);
void PageDialogClosed(content::WebContents* contents);

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_PAGE_DIALOGS_H_
