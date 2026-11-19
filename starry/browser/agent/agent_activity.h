// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_AGENT_ACTIVITY_H_
#define STARRY_BROWSER_AGENT_AGENT_ACTIVITY_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/timer/timer.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"

namespace content {
class WebContents;
}

// 智能体在哪个标签里、在做什么：网页外面的描边、光标、高亮框、扫描光带、底部的提示条
// （ui/assistant/agent_overlay.h），标签上的渐变环（ui/assistant/agent_ring.h），
// 后台标签的小卡片（ui/assistant/agent_background_tasks.h）都看它。
// 一套工具（ToolRunner）一个：从它第一次操作一个标签开始，到断开、换到别的标签、内置
// 助手的这一轮结束为止。只在 UI 线程上用。
namespace starry::agent {

struct AgentActivity {
  // 谁在做（客户端报的名字、「Claude Code」「AI 助手」）。
  std::u16string client;
  // 正在做的这一步（「点击「登录」」）；没有时在等下一步。
  std::optional<std::u16string> action;
  // 在等用户确认这一步（「等待确认：点击「登录」」）。
  std::optional<std::u16string> approval;
  // 内置助手在做：岛开着时不显示提示条（岛自己说）。
  bool assistant = false;
  // 网页在等智能体选文件（它的点击打开了选文件）。
  bool file_chooser = false;

  // 网页上的光标和要操作的元素：主文档视口里的 CSS 像素，标签显示着时 AgentOverlay
  // 换算到窗口上。没有光标时不画（一步做完 3.7 秒后淡出），没有元素时不框（做完 1.2 秒
  // 后收起）。
  std::optional<gfx::PointF> cursor;
  std::optional<gfx::RectF> target;
  // 每点下一次加一（光标轻压、波纹）；每读一次页面加一（扫过的光带）。
  int clicks = 0;
  int scans = 0;
  // 在输入：高亮框的边粗一点。
  bool typing = false;
  // 按着鼠标在拖动：光标压低一点、尖上一个小圆点；这一趟光标和高亮框按拖动的曲线
  // 滑到目标上，和真的拖动事件同步。
  bool holding = false;

  bool working() const { return action.has_value(); }
  // 提示条上的那一句：在做的这一步；没在做时是在等的事（确认、网页的对话框、
  // 选文件），都没有时是「等待下一步」。
  std::u16string Line(content::WebContents* contents) const;
};

class AgentActivities {
 public:
  static AgentActivities* Get();

  AgentActivities(const AgentActivities&) = delete;
  AgentActivities& operator=(const AgentActivities&) = delete;

  // owner（一套工具）在 tab 里开始做一步：说明换成 action。换了标签时原来的那个收起。
  // stop 是提示条上的「停止」。pointing：这一步要指向元素（接着会 Point），上一步的
  // 高亮框留着、从那里滑过去；否则上一步的高亮框这时收起。
  void Begin(const void* owner,
             tabs::TabInterface* tab,
             std::u16string client,
             bool assistant,
             std::u16string action,
             base::RepeatingClosure stop,
             bool pointing = false);
  // 在等用户确认这一步：不算在做事（描边不亮、星光不跳），提示条写 text。
  void AwaitApproval(const void* owner, std::u16string text);
  // 这一步做完了：说明留 1.2 秒再回到「等待下一步」（高亮框随之收起），光标再过 2.5 秒
  // 淡出。
  void Finish(const void* owner);
  // owner 所在的标签里网页在不在等它选文件。
  void SetFileChooser(const void* owner, bool open);
  // 不在这个标签里了（断开、这一轮结束、停下）。
  void End(const void* owner);

  // 网页上的光标与动作标记。坐标是主文档视口里的 CSS 像素。
  // 光标移到元素上（cursor 是要点的那一点）、框出它。
  void Point(const void* owner,
             const gfx::RectF& target,
             const gfx::PointF& cursor);
  // 只挪光标（滚动整个页面时移到视口中间），高亮框不变。
  void MoveCursor(const void* owner, const gfx::PointF& cursor);
  // 点下：光标轻压、尖上放出一圈波纹。
  void Press(const void* owner);
  // 读页面：一道光带扫过。
  void Scan(const void* owner);
  // 开始输入：高亮框的边粗一点。
  void SetTyping(const void* owner);
  // 按住拖的东西滑到目标上（target、cursor 是目标的）；EndDrag 松开。
  void BeginDrag(const void* owner,
                 const gfx::RectF& target,
                 const gfx::PointF& cursor);
  void EndDrag(const void* owner);

  // tab 里的智能体（几个都在时是最近做过一步的那个）；没有时为空。
  const AgentActivity* For(const tabs::TabInterface* tab) const;
  // 有智能体的每个标签和它里面的智能体（同 For），按标签开始被操作的先后。已经关掉的
  // 标签不算。给后台标签的小卡片。
  std::vector<std::pair<tabs::TabInterface*, const AgentActivity*>> All() const;
  // 提示条的「停止」：断开在 tab 里的智能体（内置助手是停下这一轮）。晚一步执行：
  // 提示条会随之拿掉，不在它自己的点击回调里删它。
  void Stop(const tabs::TabInterface* tab);

  // 有变化（开始、换字、结束，光标、高亮，网页的对话框开了关了，标签关掉、挪走了）。
  base::CallbackListSubscription AddObserver(base::RepeatingClosure callback);
  // 别的地方改了谁看得到这些标签（用户把智能体的无痕窗口拿了出来）：重新摆一遍。
  void NotifySoon();

 private:
  friend class base::NoDestructor<AgentActivities>;

  struct Entry {
    Entry();
    Entry(Entry&&);
    Entry& operator=(Entry&&);
    ~Entry();

    tabs::TabHandle tab;
    AgentActivity activity;
    base::RepeatingClosure stop;
    // 第几步：做完 1.2 秒后还是这一步才回到「等待下一步」。
    int step = 0;
    // 最近做过一步的先算（几个智能体在同一个标签里时）。
    int sequence = 0;
    // 这个智能体开始操作这个标签的先后（小卡片的顺序不跟着每一步跳）。
    int started = 0;
    std::unique_ptr<base::OneShotTimer> idle;
    // 标签关掉、挪到别的窗口时重新摆提示。
    base::CallbackListSubscription detach;
  };

  AgentActivities();
  ~AgentActivities();

  Entry* MutableEntry(const void* owner);
  const Entry* EntryFor(const tabs::TabInterface* tab) const;
  void Idle(const void* owner, int step);
  void HideCursor(const void* owner, int step);
  void Notify();

  std::map<const void*, Entry> entries_;
  int next_sequence_ = 1;
  bool notify_posted_ = false;
  base::RepeatingClosureList observers_;
  base::CallbackListSubscription dialogs_;
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_AGENT_ACTIVITY_H_
