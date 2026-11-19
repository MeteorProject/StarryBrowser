// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_BROWSER_TOOLS_H_
#define STARRY_BROWSER_AGENT_BROWSER_TOOLS_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/global_routing_id.h"
#include "ui/gfx/geometry/rect_f.h"

class BrowserWindowInterface;
class Profile;

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

// 浏览器供 AI 调用的工具集，工具名、说明与参数均使用英文（直接供模型阅读）。
// 读页面用注入的 agent-page.js，点击、输入是 DevTools 协议
// 的真实输入，都在用户看不见的后台标签里也能做。
namespace starry::agent {

class CdpSession;

inline constexpr char kAskUserTool[] = "ask_user";

struct ToolImage {
  std::string mime_type;
  std::string base64;
};

struct ToolResult {
  std::string text;
  bool is_error = false;
  std::vector<ToolImage> images;
};
using ToolCallback = base::OnceCallback<void(ToolResult)>;

// 一步的风险：读的直接做；操作在「问答」模式下先问；敏感的（执行脚本、往密码框里
// 输入）总要先问。
enum class StepRisk { kRead, kAction, kSensitive };

// 要做的一步：对话里的工具胶囊、确认卡片、网页上的提示条用。
struct ToolStep {
  std::string tool;
  std::u16string title;
  // 要输入的文字、要打开的网址、要执行的脚本。
  std::u16string detail;
  // 当前标签的网站（「在 example.com 上都允许」）。
  std::string site;
  StepRisk risk = StepRisk::kRead;
  // 要操作的元素在网页里的位置（主文档视口的 CSS 像素）和它所在的标签：网页上框出来。
  std::optional<gfx::RectF> target;
  std::optional<tabs::TabHandle> tab;
  // 自动审核要看的：网页的地址和标题、工具的参数（JSON，密码换掉了）。
  std::string page_url;
  std::string page_title;
  std::string arguments;
};

struct Approval {
  bool allowed = false;
  // 这次对话在这个网站上都允许。
  bool site_wide = false;
  // 拒绝时用户说的要改什么。
  std::string note;
  // 拒绝时告诉模型的话（自动审核拒绝的），空的用「用户拒绝了」那一句。
  std::string message;
};

struct Question {
  std::string question;
  std::string detail;
  std::vector<std::string> options;
};

// 控制暴露哪些工具。
struct ToolOptions {
  // 允许执行 JavaScript：有 browser_evaluate。
  bool scripts = false;
  // 允许上传文件：有 browser_file_upload。
  bool uploads = true;
  // 允许临时无痕标签：browser_tab_new 有 private（无痕窗口里的对话没有，那里本来就是
  // 无痕的）。
  bool private_tabs = true;
  // 内置助手：有 ask_user。
  bool ask_user = false;
};
// 这个用户资料的设置里允许的。
ToolOptions ToolOptionsFor(Profile* profile, bool ask_user);

// 一个标签的正文（同 browser_read_page 读出来的 Markdown）。
struct PageText {
  std::string title;
  std::string url;
  // 正文的开头（最多 max_length 个 UTF-16 字符）。
  std::string text;
  // 正文一共多少字（UTF-16）：比 text 长就是截断了。
  size_t total = 0;
  // 读不了时的说明（英文，给模型看）；这时 text 是空的。
  std::string error;
};
// 读一个标签的正文，不改哪个工具的当前标签、网页上不提示（内置助手里用户 @ 的标签）。
// 休眠的先唤醒，正在载入的等载入完（最多 15 秒）；读的时候照常渲染。
void ReadTabText(tabs::TabInterface* tab,
                 size_t max_length,
                 base::OnceCallback<void(PageText)> callback);

// 给模型的工具定义（Chat Completions 的 function 格式）。
base::ListValue ToolDefinitions(const ToolOptions& options);
// 同一套工具，MCP 的格式（tools/list：name、description、inputSchema、annotations）。
base::ListValue McpToolDefinitions(const ToolOptions& options);
// 给 MCP 客户端里的模型看的使用说明（initialize 的 instructions，英文）。允许临时无痕
// 标签时加上 kPrivateTabsInstructions。
std::string McpInstructions(const ToolOptions& options);
// 允许临时无痕标签时接在说明后面。
extern const char kPrivateTabsInstructions[];

// 一个对话用的一套工具：记着当前标签、快照里的 ref 是哪个 frame 的。
class ToolRunner {
 public:
  class Delegate {
   public:
    // 会改变网页或浏览器的一步：要不要先问用户由对话决定（模式、整站允许）。
    virtual void AuthorizeStep(const ToolStep& step,
                               base::OnceCallback<void(Approval)> callback) = 0;
    virtual void AskUser(const Question& question,
                         base::OnceCallback<void(std::optional<std::string>)>
                             callback) = 0;
    // 开始做一步了（提示条、状态）。
    virtual void StepStarted(const ToolStep& step) {}
    // 网页上的提示条写的是谁（客户端报的名字、「Claude Code」「AI 助手」）。
    virtual std::u16string AgentName() = 0;
    // 是内置助手：岛开着时网页上不显示提示条（岛自己说）。
    virtual bool IsAssistant() = 0;
    // 用户点了网页上提示条的「停止」：外部智能体断开，内置助手停下这一轮。
    virtual void StopRequested() = 0;

   protected:
    virtual ~Delegate() = default;
  };

  ToolRunner(BrowserWindowInterface* browser, Delegate* delegate);
  ToolRunner(const ToolRunner&) = delete;
  ToolRunner& operator=(const ToolRunner&) = delete;
  ~ToolRunner();

  // 这一轮从用户正在看的标签开始（消息里的「这个页面」）。
  void SetCurrentTab(tabs::TabInterface* tab);
  tabs::TabInterface* current_tab() const;
  // 标签的短 ID（browser_tabs 里的那个）。
  static std::string TabId(tabs::TabInterface* tab);
  // 用户自己的标签（browser_tabs 列出的，不含这套工具的临时无痕标签）：这个窗口的在
  // 前面。内置助手的输入框里 @ 的就是这些。
  std::vector<tabs::TabInterface*> UserTabs() const;

  void Run(const std::string& name, base::DictValue args, ToolCallback callback);
  // 停下：没做完的一步作废，后台标签不再保持运行，网页上的提示收起。
  void Cancel();
  // 不在标签里做事了（内置助手的这一轮结束）：网页上的描边、提示条收起。
  void EndActivity();

  // 不改网页的工具（读、等待、截图……）。
  static bool IsReadOnly(const std::string& name);
  // 工具在对话里的说法（「点击「登录」」）；还没定位到元素时用参数里的描述。
  static std::u16string TitleFor(const std::string& name,
                                 const base::DictValue& args);

 private:
  struct RefRange;
  struct Located;
  struct Session;
  class FileChooser;
  class Job;
  class SnapshotJob;
  friend class Job;
  friend class SnapshotJob;

  tabs::TabInterface* ResolveTab(const base::DictValue& args,
                                 std::string* error) const;
  // 这套工具看得到的标签：普通窗口的，后面是自己的临时无痕标签（private_windows.h）。
  std::vector<tabs::TabInterface*> VisibleTabs() const;
  // 是这套工具自己的临时无痕标签。
  bool IsPrivateTab(tabs::TabInterface* tab) const;
  CdpSession* SessionFor(content::WebContents* contents);
  // frame 所在的那个目标的连接：跨进程的 iframe 是它自己的，否则是标签的。
  CdpSession* FrameSessionFor(content::RenderFrameHost* frame);
  std::unique_ptr<Session> NewSession(std::unique_ptr<CdpSession> cdp,
                                      content::WebContents* contents);
  // 连接上来的事件：网页要选文件了（拦下了选文件的面板）。
  void SessionEvent(base::WeakPtr<content::WebContents> contents,
                    base::WeakPtr<CdpSession> session,
                    const std::string& method,
                    const base::DictValue& params);
  // 网页在等智能体选文件。
  FileChooser* FileChooserFor(tabs::TabInterface* tab);
  void DropFileChooser(tabs::TabHandle tab);
  // 设置里允许智能体上传文件：它的操作打开的选文件由它回答。
  bool UploadsAllowed() const;
  // 后台标签在智能体操作期间照常渲染、接收输入。
  void KeepRendering(content::WebContents* contents);
  // 网页上的提示（agent_activity.h）：在 tab 里开始做 title 这一步（pointing：这一步要
  // 指向元素，高亮框从上一个元素滑过去）；在等用户确认。
  void BeginStep(tabs::TabInterface* tab,
                 const std::u16string& title,
                 bool pointing = false);
  void AwaitApproval(const std::u16string& title);
  // 提示条上点了「停止」。
  void StopFromPage();

  // 读页面。
  void Snapshot(tabs::TabInterface* tab,
                int max_length,
                base::OnceCallback<void(base::expected<std::string, std::string>)>
                    callback);
  // scroll：元素不在视口里时先滚过去（拖放要两个元素同时在视口里，第二次不滚）。
  void Locate(
      tabs::TabInterface* tab,
      const std::string& ref,
      base::OnceCallback<void(base::expected<Located, std::string>)> callback,
      bool scroll = true);
  const RefRange* RangeFor(const std::string& ref) const;

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<Delegate> delegate_;
  std::optional<tabs::TabHandle> current_;
  // 上一个当前标签：关掉当前标签（比如链接打开的新标签）时回到它，而不是用户正在看的标签。
  std::optional<tabs::TabHandle> previous_;
  // 当前标签是临时无痕标签：它被别人关掉（用户关了窗口、太久没用）以后不悄悄换到用户的
  // 普通标签上（那样接下来的搜索会进历史），而是报错。
  bool current_private_ = false;
  int next_ref_ = 1;
  std::vector<RefRange> ranges_;
  std::map<content::WebContents*, std::unique_ptr<Session>> sessions_;
  // 跨进程 iframe 的连接：目标的 ID（frame 的 DevTools token）→ 连接。
  std::map<std::string, std::unique_ptr<Session>> frame_sessions_;
  std::map<tabs::TabHandle, std::unique_ptr<FileChooser>> file_choosers_;
  std::map<content::WebContents*, base::ScopedClosureRunner> keep_rendering_;
  // 等连接回答、记下选文件用：Cancel() 不作废它（停下这一轮以后网页还在等文件）。
  base::WeakPtrFactory<ToolRunner> chooser_weak_factory_{this};
  base::WeakPtrFactory<ToolRunner> weak_factory_{this};
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_BROWSER_TOOLS_H_
