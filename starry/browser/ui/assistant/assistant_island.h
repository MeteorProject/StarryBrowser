// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_ISLAND_H_
#define STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_ISLAND_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

namespace views {
class EventMonitor;
class WebView;
}

namespace starry {

class AssistantCapsules;
class AssistantController;
class AssistantIslandRing;
class AssistantIslandTint;
class AssistantMiniToolbar;
class GlassView;
class StarryBrowserView;

// AI 助手。两种样子，设置里或者标题行的按钮换，所有窗口一起换：
//
// 悬浮（默认）：网页下方正中一条玻璃输入框（宽 560，放不下时左右各留 16），对话从
// 它往上长，最多盖网页的 52%，再长在里面滚动。网页不变窄。同一个窗口里别的进行中的
// 对话是玻璃上面一排胶囊（AssistantCapsules）。打开、关掉：玻璃从输入框正中一个 58
// 的小圆撑开（带一点回弹），关掉时收回小圆再消失。外面一圈：输入框有焦点时一圈
// 渐变线，智能体做事时一段渐变绕着转（AssistantIslandRing）。点网页就收回成一条
// 输入框（智能体接着做），点回岛上展开；有要用户拿主意的事（确认、问题）时不收。
// 只有一条输入框（空的对话、收起了）时网页里没有标题行：鼠标移到岛上时，输入框右上方
// 淡入一排小按钮（AssistantMiniToolbar：新对话、聊天记录、换成侧栏、关闭）。
//
// 侧栏：停在窗口右边宽 392 的一列，内容卡片变窄让出位置（不盖网页）；没有玻璃，直接
// 在窗口的底色上，显示整个对话；胶囊在输入框下面一排。从右边滑进来、滑出去。
//
// 玻璃是实时的 Liquid Glass（GlassView），上面一层主题的面板色；里面的内容（对话、
// 输入框、确认卡片、聊天记录）是一个背景透明的网页（starry://assistant，assistant.js）。
// 悬浮时按网页量出来的高度变高：网页立刻是目标的高度、贴着玻璃下边，玻璃用弹簧
// 跟过去，多出来的部分被玻璃裁掉，动画过程中不重新排版。
//
// ⌘E、侧栏的 ✦：关着时打开；开着、焦点不在里面时把焦点放进输入框；已经在里面就关掉。
// 设置里关掉助手（prefs::kAssistantEnabled）时哪里都打不开，开着的对话存好、结束。
class AssistantIsland : public views::View,
                        public views::ViewTargeterDelegate {
  METADATA_HEADER(AssistantIsland, views::View)

 public:
  explicit AssistantIsland(StarryBrowserView* browser_view);
  AssistantIsland(const AssistantIsland&) = delete;
  AssistantIsland& operator=(const AssistantIsland&) = delete;
  ~AssistantIsland() override;

  AssistantController* controller() { return controller_.get(); }
  void SetTheme(const Theme& theme);

  void Toggle();
  void Open(bool focus);
  void Close();
  bool is_open() const { return open_; }
  bool docked() const { return docked_; }

  // 内容卡片和右侧停靠面板的可用区域（本视图的坐标）。
  void SetLayoutBounds(const gfx::Rect& card, const gfx::Rect& dock);
  // 网页量出来的内容高度。
  void SetContentHeight(int height);
  // 收成一条输入框 / 展开。
  void SetCollapsed(bool collapsed);
  // 网页里的输入框有没有焦点（外面一圈的渐变线）。
  void SetComposerFocused(bool focused);
  // 有图片拖到岛上（外面一圈渐变线，不透明）。
  void SetDragOver(bool over);
  // 收成一条（标题行的 ∨、Esc）：没有对话、有要用户回答的事、停在右边时不收，返回 false。
  bool Collapse();
  // 做事时又按了发送：整个岛左右晃一下（这一轮做完才能发）。
  void Shake();
  // 设置里换了样子（悬浮、侧栏）：开着的直接变过去。
  void PanelStyleChanged();
  bool collapsed() const { return collapsed_; }
  // 玻璃现在的位置（本视图的坐标）。
  gfx::Rect GlassRect() const;
  // 悬浮着开着时玻璃的位置（媒体岛让开它）；关着、停在侧栏时是空的。
  gfx::Rect FloatingRect() const;
  // 小工具条的玻璃（本视图的坐标）；没显示时是空的。
  gfx::Rect MiniToolbarRect() const;
  // 窗口里的鼠标移动（Widget 的坐标）：只有一条输入框时，移到岛上显示小工具条。真的
  // 鼠标移动由窗口的事件监视器转过来（网页上的移动 Views 收不到），调试步骤
  // assistanthover 也走这里。
  void OnWindowMouseMove(const gfx::Point& root_point);
  views::WebView* web_view() { return web_view_; }

  // views::View:
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override;
  void AddedToWidget() override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  class EventWatcher;
  friend class EventWatcher;

  // 展开时的高度（网页就是这么高）；收起时玻璃只有一条输入框高。
  int FullHeight() const;
  int TargetHeight() const;
  // 打开时玻璃的位置（不算打开、关掉的变形）。
  gfx::Rect RestRect() const;
  void UpdateRing();
  // 岛关着、还有对话在做事或者等用户时，各处 ✦ 上的小点。
  void UpdateBadge();
  void UpdateTint();
  void Relayout();
  void LayoutCapsules();
  void EnsureWebView();
  void OnStateChanged();
  void OnConversationActivated(bool focus);
  void OnWindowMousePress(const gfx::Point& screen_point);
  void ApplyPanelStyle();
  bool Enabled() const;
  // 设置里开关了助手。
  void OnEnabledChanged();
  // 聊天记录在玻璃里：收着时先展开。
  void OpenHistory();
  // 悬浮、开着、网页里没有标题行（空的对话或者收起了，没开聊天记录、没有要用户回答
  // 的事）：这时鼠标在岛上才有小工具条。
  bool WantsMiniToolbar() const;
  void SetMiniToolbarShown(bool shown);
  // 摆在输入框右上方、胶囊那一排上面。
  void LayoutMiniToolbar();

  const raw_ptr<StarryBrowserView> browser_view_;
  std::unique_ptr<AssistantController> controller_;
  Theme theme_ = Theme::DarkBase();

  raw_ptr<GlassView> glass_ = nullptr;
  raw_ptr<AssistantIslandTint> tint_ = nullptr;
  raw_ptr<views::View> clip_ = nullptr;
  raw_ptr<views::WebView> web_view_ = nullptr;
  raw_ptr<AssistantIslandRing> ring_ = nullptr;
  raw_ptr<AssistantCapsules> capsules_ = nullptr;
  raw_ptr<AssistantMiniToolbar> mini_toolbar_ = nullptr;

  gfx::Rect card_;
  gfx::Rect dock_;
  int content_height_ = 0;
  bool open_ = false;
  bool docked_ = false;
  bool collapsed_ = false;
  bool composer_focused_ = false;
  bool drag_over_ = false;
  std::unique_ptr<EventWatcher> watcher_;
  std::unique_ptr<views::EventMonitor> monitor_;
  PrefChangeRegistrar enabled_pref_;
  ValueMotion height_;
  // 打开、关掉的变形：0 是小圆（停在右边时是滑出去的样子），1 是打开的样子（回弹时会
  // 超过 1）。
  ValueMotion morph_;
  // 悬浮和侧栏之间换：玻璃从一处滑到另一处（0 是原来的位置，1 是新的）。
  ValueMotion restyle_;
  gfx::Rect restyle_from_;
  // 减弱动态效果时不变形，只淡入淡出。
  LayerMotion open_motion_;
  // 小工具条该不该显示（淡出的过程中已经是 false，不接鼠标）。
  bool mini_toolbar_shown_ = false;
  LayerMotion mini_toolbar_motion_;
  base::WeakPtrFactory<AssistantIsland> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_ISLAND_H_
