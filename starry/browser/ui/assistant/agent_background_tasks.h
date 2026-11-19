// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_AGENT_BACKGROUND_TASKS_H_
#define STARRY_BROWSER_UI_ASSISTANT_AGENT_BACKGROUND_TASKS_H_

#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

namespace starry {

// 智能体在后台操作的标签：内容区右下角一列玻璃小卡片，写着哪个标签、哪个智能体
// 在做什么。
// - 这个窗口里被操作、又没显示着的标签；智能体的临时无痕标签不在任何用户的窗口里，每个
//   浏览器窗口都列出它们（标题后面一个无痕的紫色小标记），除非用户正看着那个无痕窗口。
// - 卡片：图标（外面一圈智能体的渐变环）、标题，下面一行是星光（做事时跳动）、智能体和
//   它正在做的事，右边「停止」。点卡片切到那个标签看它做（无痕标签是把智能体的无痕窗口
//   拿出来），「停止」断开那个智能体。悬停时轻轻放大。
// - 最多三张，多的写「还有 N 个标签页」。新的从右边滑进来，没了的滑出去，留下的挪到新
//   位置。
// 铺满窗口，只有卡片接鼠标。
class AgentBackgroundTasks : public views::View,
                             public views::ViewTargeterDelegate {
  METADATA_HEADER(AgentBackgroundTasks, views::View)

 public:
  struct Entry {
    tabs::TabHandle tab;
    std::u16string title;
    ui::ImageModel favicon;
    std::u16string client;
    // 正在做的这一步；没在做时是在等的事。
    std::u16string line;
    bool working = false;
    // 智能体的临时无痕标签：点它把那个窗口拿出来。
    bool is_private = false;
  };

  AgentBackgroundTasks();
  AgentBackgroundTasks(const AgentBackgroundTasks&) = delete;
  AgentBackgroundTasks& operator=(const AgentBackgroundTasks&) = delete;
  ~AgentBackgroundTasks() override;

  void SetTheme(const Theme& theme);
  // area：内容区（本视图的坐标）。bottom：卡片的底边离内容区底边多远（平时 14，显示着的
  // 标签里有提示条时 64，岛盖着底边时再加上岛）。
  void Update(const std::vector<Entry>& entries,
              const gfx::Rect& area,
              int bottom);
  bool HitsCard(const gfx::Rect& rect) const;
  // 显示着的卡片（本视图的坐标）。
  std::vector<gfx::Rect> CardRects() const;

  // 调试：每张卡片（标签、文字、玻璃的位置）和「还有 N 个」。
  std::string DebugDescription() const;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  class Card;
  class More;

  void LeaveCard(Card* card);
  void RemoveLater(views::View* view);

  Theme theme_ = Theme::DarkBase();
  // 显示着的卡片（从上到下）；正在滑出去的不在这里（还是子视图）。
  std::vector<raw_ptr<Card>> cards_;
  raw_ptr<More> more_ = nullptr;
  base::WeakPtrFactory<AgentBackgroundTasks> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_AGENT_BACKGROUND_TASKS_H_
