// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_PAGE_COMMANDS_H_
#define STARRY_BROWSER_AGENT_PAGE_COMMANDS_H_

#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/common/chrome_isolated_world_ids.h"

namespace content {
class RenderFrameHost;
}

// 在网页里跑智能体的页面工具（starry/browser/resources/agent/agent-page.js）：快照、
// 定位元素、准备输入、选择选项、正文、选中的文字。脚本跑在 Starry 自己的 isolated world
// 里，网页的脚本看不到它，它也不改网页的全局对象。
namespace starry::agent {

// Starry 智能体用的 isolated world：浏览器只认 Chrome 预先定义的几个（不大于
// ISOLATED_WORLD_ID_MAX），用 Chrome 内部功能的那一个。脚本只在全局对象上加
// __starryAgent、__starryFrameTokens，和同一个 world 里的别的脚本互不影响。
inline constexpr int32_t kAgentWorldId = ISOLATED_WORLD_ID_CHROME_INTERNAL;

using PageResult = base::expected<base::Value, std::string>;
using PageCallback = base::OnceCallback<void(PageResult)>;

// 在 frame 里跑 agent-page.js 的一个命令，返回它的结果（value）或者错误的说明。网页卡住、
// 弹出对话框时调用不返回：10 秒后当作失败。
void RunPageCommand(content::RenderFrameHost* frame,
                    std::string_view command,
                    base::DictValue args,
                    PageCallback callback);

// 在 frame 的 agent world 里跑一段脚本，返回最后一个表达式的值；超时同上。
void RunInAgentWorld(content::RenderFrameHost* frame,
                     const std::string& script,
                     base::OnceCallback<void(std::optional<base::Value>)> callback);

// 跨源 iframe 里先装上的脚本：收父页面发来的口令（agent-page.js 的 crossFrame），记下来，
// 浏览器凭口令认出是哪个 frame。
extern const char kFrameListenerScript[];
// 读出这个 frame 收到过的口令（JSON 数组）。
extern const char kFrameTokensScript[];

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_PAGE_COMMANDS_H_
