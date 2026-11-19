// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_ASSISTANT_PROMPT_H_
#define STARRY_BROWSER_ASSISTANT_ASSISTANT_PROMPT_H_

#include <optional>
#include <string>
#include <vector>

// 给模型的说明和每条消息附上的浏览器上下文。用英文写：读它的是模型。说明在一个对话里
// 不变（前缀缓存），会变的东西放在每条用户消息前面的 <browser_context> 里。
namespace starry::assistant {

// private_window：对话在无痕窗口里（只看得到那个窗口）；private_tabs：允许临时无痕标签。
std::string SystemPrompt(bool private_window, bool private_tabs);

// 用户在输入框里 @ 的标签：正文的开头，或者读不了的说明。
struct ReferencedTab {
  std::string id;
  std::string title;
  std::string url;
  std::string text;
  // 正文一共多少字（UTF-16）：比 text 长时告诉模型用 browser_read_page 接着读。
  size_t total = 0;
  // 读不了（浏览器自己的页面、开着对话框、关掉了……）。
  std::string error;
};

struct PromptContext {
  std::string tab_id;
  std::string title;
  std::string url;
  // 用户选中的文字。
  std::string selection;
  // 当前标签是浏览器自己的页面或者空白页：工具读不了。
  std::string note;
  // 用户 @ 的标签（按用户加的顺序）。
  std::vector<ReferencedTab> tabs;
};

// 写成 <browser_context> 块；什么都没有时为空。
std::optional<std::string> ContextBlock(const PromptContext& context);

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_ASSISTANT_PROMPT_H_
