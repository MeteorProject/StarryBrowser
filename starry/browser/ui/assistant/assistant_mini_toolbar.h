// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_MINI_TOOLBAR_H_
#define STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_MINI_TOOLBAR_H_

#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view_targeter_delegate.h"

namespace starry {

class AssistantMiniButton;
class AssistantMiniToolbarBody;

// 悬浮的岛只有一条输入框（空的对话、收起了）时没有标题行：鼠标移到岛上时，输入框右上方
// 淡入这一排按钮：新对话、聊天记录、换成侧栏、关闭。玻璃胶囊高 34，按钮 30 × 28，
// 悬停时一层浅底。什么时候显示、摆在哪里由岛（AssistantIsland）管。
//
// 视图比玻璃大一圈（阴影），只有玻璃本身接鼠标。
class AssistantMiniToolbar : public GlassView,
                             public views::ViewTargeterDelegate {
  METADATA_HEADER(AssistantMiniToolbar, GlassView)

 public:
  struct Callbacks {
    Callbacks();
    Callbacks(Callbacks&&);
    Callbacks& operator=(Callbacks&&);
    ~Callbacks();
    base::RepeatingClosure new_chat;
    base::RepeatingClosure history;
    base::RepeatingClosure dock;
    base::RepeatingClosure close;
  };

  explicit AssistantMiniToolbar(Callbacks callbacks);
  AssistantMiniToolbar(const AssistantMiniToolbar&) = delete;
  AssistantMiniToolbar& operator=(const AssistantMiniToolbar&) = delete;
  ~AssistantMiniToolbar() override;

  // 玻璃的大小：四个按钮 30 宽，左右各留 4；高 34。
  static gfx::Size GlassSize();

  void SetTheme(const Theme& theme);
  // 玻璃在父视图里的位置。
  void SetToolbarBounds(const gfx::Rect& rect);
  gfx::Rect toolbar_bounds() const { return glass_bounds(); }

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  raw_ptr<AssistantMiniToolbarBody> body_ = nullptr;
  std::vector<raw_ptr<AssistantMiniButton>> buttons_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_MINI_TOOLBAR_H_
