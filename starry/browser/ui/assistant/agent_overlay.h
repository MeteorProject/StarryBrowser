// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_AGENT_OVERLAY_H_
#define STARRY_BROWSER_UI_ASSISTANT_AGENT_OVERLAY_H_

#include <map>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

namespace starry {

class AgentBackgroundTasks;
class AgentPaneMarks;
class AgentStatusBar;
class StarryBrowserView;

// 智能体在网页上的提示：
// - 用户看着的每一格（分屏时是那一格）：外圈描边、扫描光带、高亮框、光标
//   （AgentPaneMarks，agent_marks.h），网页底部正中的玻璃提示条（AgentStatusBar）：星光
//   （做事时跳动）、谁在做、正在做的这一步（没在做时是「等待下一步」「网页弹出了对话框」
//   「等待确认：……」），右边「停止」。内置助手的提示条在岛悬浮着开着时不显示（岛自己说），
//   外部智能体的放在岛上面。
// - 内容区右下角：在后台被操作的标签的小卡片（AgentBackgroundTasks）。
//
// 谁在哪个标签里、在做什么在 agent::AgentActivities 里；它变了、换了标签、排版之后都重新摆
// 一遍（Update）。整层铺满窗口，只有提示条和小卡片接鼠标：macOS 上 Views 命中普通视图时
// 点击交给 Views，不给网页。
class AgentOverlay : public views::View, public views::ViewTargeterDelegate {
  METADATA_HEADER(AgentOverlay, views::View)

 public:
  explicit AgentOverlay(StarryBrowserView* browser_view);
  AgentOverlay(const AgentOverlay&) = delete;
  AgentOverlay& operator=(const AgentOverlay&) = delete;
  ~AgentOverlay() override;

  void SetTheme(const Theme& theme);
  // 重新看显示着的每一格里有没有智能体，摆描边、光标、提示条和后台的小卡片：智能体的
  // 状态变了、排版之后、换了标签、岛开了关了。
  void Update();
  // 提示条和后台卡片现在的位置（本视图的坐标；媒体岛让开它们）。
  std::vector<gfx::Rect> ObstacleRects() const;

  // 调试：每一格的描边、光标、高亮框、扫描，提示条（位置、文字、「停止」在哪里），
  // 后台的小卡片。
  std::string DebugDescription() const;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  // 显示着的、有智能体的一格。
  struct Pane {
    tabs::TabHandle tab;
    // 内容卡片的这一格（本视图的坐标）。
    gfx::Rect card;
    bool working = false;

    bool operator==(const Pane&) const = default;
  };

  // 提示条收起：滑下去淡出，放完再拿掉。
  void Leave(AgentStatusBar* bar);
  void Remove(views::View* view);
  // 新的提示条、描边加在小卡片下面。
  size_t TasksIndex() const;

  const raw_ptr<StarryBrowserView> browser_view_;
  Theme theme_ = Theme::DarkBase();
  std::vector<Pane> panes_;
  // 显示着的提示条，每格一个；正在收起的不在这里（还是子视图）。
  std::vector<raw_ptr<AgentStatusBar>> bars_;
  // 每格的描边、光标……；正在淡出的不在这里。
  std::map<tabs::TabHandle, raw_ptr<AgentPaneMarks>> marks_;
  raw_ptr<AgentBackgroundTasks> tasks_ = nullptr;
  base::CallbackListSubscription activities_;
  base::WeakPtrFactory<AgentOverlay> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_AGENT_OVERLAY_H_
