// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CAPSULES_H_
#define STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CAPSULES_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "starry/browser/ui/assistant/assistant_controller.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

namespace starry {

class AssistantCapsuleView;

// 同一个窗口里别的进行中的对话：悬浮时是岛上面一排玻璃小胶囊，居中，放不下的收进最后
// 一个「还有 N 个」（点开聊天记录）；侧栏时在输入框下面一排平分宽度。
//
// 胶囊高 34：左边状态（在做是转圈，等用户是黄色的感叹号，做完了是勾），标题，后面一句
// 状态；悬停时右边出现 ×（停下并收起）。点胶囊切过去。新的从下面冒出来（缩放 0.9、往下
// 10，弹簧 0.34/0.78），没了的缩回去淡出，留下的滑到新位置（弹簧 0.42/0.86）。
//
// 这个视图铺满整个岛、在网页上面，所以只有胶囊接鼠标（DoesIntersectRect）：不然岛里的
// 网页收不到点击（macOS 上 Views 命中普通视图时点击交给 Views，不给网页的 NSView）。
class AssistantCapsules : public views::View,
                          public views::ViewTargeterDelegate {
  METADATA_HEADER(AssistantCapsules, views::View)

 public:
  struct Callbacks {
    Callbacks();
    Callbacks(Callbacks&&);
    Callbacks& operator=(Callbacks&&);
    ~Callbacks();
    base::RepeatingCallback<void(const std::string&)> open;
    base::RepeatingCallback<void(const std::string&)> dismiss;
    base::RepeatingClosure more;
  };

  explicit AssistantCapsules(Callbacks callbacks);
  AssistantCapsules(const AssistantCapsules&) = delete;
  AssistantCapsules& operator=(const AssistantCapsules&) = delete;
  ~AssistantCapsules() override;

  void SetTheme(const Theme& theme);
  // 新的一组：新的冒出来，没了的缩回去。
  void Update(const std::vector<AssistantController::Capsule>& capsules);
  // 悬浮：一排居中在 center_x，下边在 bottom，最宽 room。
  void LayoutFloating(int center_x, int bottom, int room);
  // 侧栏：在 row 里平分宽度。
  void LayoutDocked(const gfx::Rect& row);
  bool has_capsules() const { return !capsules_.empty(); }
  // 胶囊在不在这里（本视图的坐标）：只有胶囊接鼠标。
  bool HitsCapsule(const gfx::Rect& rect) const;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  friend class AssistantCapsuleView;

  void Leave(AssistantCapsuleView* capsule);
  // 缩回去放完了：拿掉（还在、没有又回来时）。
  void Remove(AssistantCapsuleView* capsule);
  // 换了位置的滑过去，刚出来的冒出来。
  void Place(AssistantCapsuleView* capsule, const gfx::Rect& rect);

  Callbacks callbacks_;
  Theme theme_ = Theme::DarkBase();
  std::vector<raw_ptr<AssistantCapsuleView>> capsules_;
  raw_ptr<AssistantCapsuleView> overflow_ = nullptr;
  base::WeakPtrFactory<AssistantCapsules> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CAPSULES_H_
