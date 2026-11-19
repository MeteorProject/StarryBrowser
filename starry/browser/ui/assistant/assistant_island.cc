// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/assistant_island.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/file_select_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/assistant/assistant_settings.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/assistant/assistant_capsules.h"
#include "starry/browser/ui/assistant/assistant_controller.h"
#include "starry/browser/ui/assistant/assistant_island_ring.h"
#include "starry/browser/ui/assistant/assistant_mini_toolbar.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/spring.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animation_element.h"
#include "ui/compositor/layer_animation_sequence.h"
#include "ui/compositor/layer_animator.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_observer.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/event_monitor.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC)
#include "starry/browser/ui/assistant/assistant_shortcut_mac.h"
#endif

namespace starry {

namespace {

// 宽 560，放不下时左右各留 16；离内容卡片底边 16；一行输入框高 58；最多盖网页的 52%。
constexpr int kMaxWidth = 560;
constexpr int kSideMargin = 16;
constexpr int kBottomMargin = 16;
constexpr int kMinHeight = 58;
constexpr float kMaxFraction = 0.52f;
// 有胶囊时玻璃最高矮一排（胶囊高度 34 + 间距 8）。
constexpr int kCapsuleRow = 42;
constexpr int kCapsuleHeight = 34;
constexpr int kRowGap = 8;
// 只有一条输入框时是胶囊（29），有对话时 26。
constexpr float kBarRadius = 29;
constexpr float kSheetRadius = 26;
// 打开、关掉时变形的小圆：58，在输入框下面 8。
constexpr double kMorphSide = 58;
constexpr double kMorphDrop = 8;
// 弹簧参数（response, dampingFraction）：玻璃变高、变矮不回弹；打开带一点回弹；
// 关掉不回弹。停在右边时打开也用不回弹的。
constexpr Spring kSheet{0.351, 0.95};
constexpr Spring kOpen{0.39, 0.713};
constexpr Spring kClose{0.307, 0.976};
// 减弱动态效果时只淡入淡出。
constexpr double kFade = 0.18;

constexpr char kPageUrl[] = "starry://assistant/";

gfx::Rect Lerp(const gfx::Rect& from, const gfx::Rect& to, double t) {
  const auto mix = [t](int a, int b) {
    return static_cast<int>(std::round(a + (b - a) * t));
  };
  return gfx::Rect(mix(from.x(), to.x()), mix(from.y(), to.y()),
                   std::max(0, mix(from.width(), to.width())),
                   std::max(0, mix(from.height(), to.height())));
}

}  // namespace

// 助手的网页：「选择图片…」要弹出选文件的对话框、网页没处理的按键要交回浏览器
// （views::WebView 自己都不管）。
class AssistantWebView : public views::WebView {
  METADATA_HEADER(AssistantWebView, views::WebView)

 public:
  using views::WebView::WebView;

  // content::WebContentsDelegate:
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params) override {
    FileSelectHelper::RunFileChooser(render_frame_host, std::move(listener),
                                     params);
  }

  // 同 Chrome 的标签（BrowserView::HandleKeyboardEvent）：先看窗口的快捷键，再交给
  // 系统。macOS 上 ⌘C、⌘V、⌘X、⌘A、⌘Z 是这样转到菜单栏的「编辑」，再以 copy: 这些
  // 发回网页的；不交出去网页里就不能拷贝、粘贴，⌘W、⌘T 这些也没反应。
  bool HandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override {
    views::FocusManager* focus_manager = GetFocusManager();
    return focus_manager &&
           unhandled_keyboard_event_handler_.HandleKeyboardEvent(event,
                                                                 focus_manager);
  }

 private:
  views::UnhandledKeyboardEventHandler unhandled_keyboard_event_handler_;
};

BEGIN_METADATA(AssistantWebView)
END_METADATA

// 玻璃上面一层主题的面板色：只有输入框时薄一些（46%），有对话时厚一些（64%）。
class AssistantIslandTint : public views::View {
  METADATA_HEADER(AssistantIslandTint, views::View)

 public:
  AssistantIslandTint() {
    SetCanProcessEventsWithinSubtree(false);
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  void Set(const Theme& theme, bool thick) {
    theme_ = theme;
    thick_ = thick;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    // 不透明度：输入框 46%，对话 64%；深色用面板色，浅色用亮的底色。
    const float alpha = thick_ ? 0.64f : 0.46f;
    const SkColor base =
        theme_.is_dark ? theme_.surface_panel : theme_.surface_bright;
    fill.setColor(SkColorSetA(base, static_cast<U8CPU>(alpha * 255)));
    const float radius = std::min(radius_, bounds.height() / 2);
    canvas->DrawRoundRect(bounds, radius, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius - 0.5f, border);
  }

  void SetRadius(float radius) {
    if (radius_ != radius) {
      radius_ = radius;
      SchedulePaint();
    }
  }

 private:
  Theme theme_ = Theme::DarkBase();
  bool thick_ = false;
  float radius_ = kBarRadius;
};

BEGIN_METADATA(AssistantIslandTint)
END_METADATA

// views::View 也有 OnEvent，所以窗口的事件由这个小对象转给岛。
class AssistantIsland::EventWatcher : public ui::EventObserver {
 public:
  explicit EventWatcher(AssistantIsland* island) : island_(island) {}
  void OnEvent(const ui::Event& event) override {
    if (!event.IsLocatedEvent()) {
      return;
    }
    const gfx::Point point = event.AsLocatedEvent()->root_location();
    if (event.type() == ui::EventType::kMousePressed) {
      island_->OnWindowMousePress(point);
    } else {
      island_->OnWindowMouseMove(point);
    }
  }

 private:
  const raw_ptr<AssistantIsland> island_;
};

AssistantIsland::AssistantIsland(StarryBrowserView* browser_view)
    : browser_view_(browser_view),
      controller_(std::make_unique<AssistantController>(browser_view)),
      watcher_(std::make_unique<EventWatcher>(this)),
      height_(kMinHeight,
              base::BindRepeating(&AssistantIsland::Relayout,
                                  base::Unretained(this))),
      morph_(0,
             base::BindRepeating(&AssistantIsland::Relayout,
                                 base::Unretained(this))),
      restyle_(1,
               base::BindRepeating(&AssistantIsland::Relayout,
                                   base::Unretained(this))) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  glass_ = AddChildView(std::make_unique<GlassView>());
  glass_->SetCornerRadius(kBarRadius, true);
  SetWindowDraggable(glass_, false);
  tint_ = AddChildView(std::make_unique<AssistantIslandTint>());
  clip_ = AddChildView(std::make_unique<views::View>());
  clip_->SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  clip_->layer()->SetMasksToBounds(true);
  clip_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(kBarRadius));
  SetWindowDraggable(clip_, false);
  ring_ = AddChildView(std::make_unique<AssistantIslandRing>());
  AssistantCapsules::Callbacks callbacks;
  callbacks.open = base::BindRepeating(
      [](AssistantIsland* island, const std::string& id) {
        island->controller_->Activate(id, /*show_tab=*/true);
      },
      base::Unretained(this));
  callbacks.dismiss = base::BindRepeating(
      [](AssistantIsland* island, const std::string& id) {
        island->controller_->Dismiss(id);
      },
      base::Unretained(this));
  callbacks.more = base::BindRepeating(&AssistantIsland::OpenHistory,
                                       base::Unretained(this));
  capsules_ =
      AddChildView(std::make_unique<AssistantCapsules>(std::move(callbacks)));
  AssistantMiniToolbar::Callbacks mini_callbacks;
  mini_callbacks.new_chat = base::BindRepeating(
      [](AssistantIsland* island) { island->controller_->NewChat(); },
      base::Unretained(this));
  mini_callbacks.history = base::BindRepeating(&AssistantIsland::OpenHistory,
                                               base::Unretained(this));
  mini_callbacks.dock = base::BindRepeating(
      [](AssistantIsland* island) {
        island->controller_->SetPanelStyle(assistant::kPanelDock);
      },
      base::Unretained(this));
  mini_callbacks.close =
      base::BindRepeating(&AssistantIsland::Close, base::Unretained(this));
  mini_toolbar_ = AddChildView(
      std::make_unique<AssistantMiniToolbar>(std::move(mini_callbacks)));
  mini_toolbar_->SetVisible(false);
  mini_toolbar_->layer()->SetOpacity(0);
  SetVisible(false);
  docked_ =
      assistant::ReadSettings(browser_view->browser()->GetProfile()).docked();
  ApplyPanelStyle();
  controller_->SetStateChangedCallback(base::BindRepeating(
      &AssistantIsland::OnStateChanged, weak_factory_.GetWeakPtr()));
  controller_->SetActivatedCallback(base::BindRepeating(
      &AssistantIsland::OnConversationActivated, weak_factory_.GetWeakPtr()));
  enabled_pref_.Init(prefs::PrefsFor(browser_view->browser()->GetProfile()));
  enabled_pref_.Add(prefs::kAssistantEnabled,
                    base::BindRepeating(&AssistantIsland::OnEnabledChanged,
                                        base::Unretained(this)));
}

AssistantIsland::~AssistantIsland() {
  monitor_.reset();
}

void AssistantIsland::SetTheme(const Theme& theme) {
  theme_ = theme;
  GlassStyle style;
  style.dark = theme.is_dark;
  style.luma = theme.is_dark ? 0.1f : 0.9f;
  glass_->SetStyle(style);
  capsules_->SetTheme(theme);
  mini_toolbar_->SetTheme(theme);
  UpdateTint();
}

void AssistantIsland::UpdateTint() {
  tint_->Set(theme_,
             controller_->has_conversation() || controller_->history_shown());
}

void AssistantIsland::ApplyPanelStyle() {
  // 侧栏没有玻璃：直接在窗口的底色上。
  glass_->SetVisible(!docked_);
  tint_->SetVisible(!docked_);
}

void AssistantIsland::AddedToWidget() {
  views::View::AddedToWidget();
  if (views::FocusManager* focus_manager = GetFocusManager()) {
    focus_manager->RegisterAccelerator(
        ui::Accelerator(ui::VKEY_E, ui::EF_PLATFORM_ACCELERATOR),
        ui::AcceleratorManager::kHighPriority, this);
  }
#if BUILDFLAG(IS_MAC)
  // ⌘E 在 Chrome 的菜单栏里是「用所选内容查找」：让给助手。
  ReleaseCommandEShortcut();
#endif
}

bool AssistantIsland::AcceleratorPressed(const ui::Accelerator& accelerator) {
  // 助手关掉了：⌘E 交给网页。
  if (!Enabled()) {
    return false;
  }
  Toggle();
  return true;
}

bool AssistantIsland::Enabled() const {
  return assistant::Enabled(browser_view_->browser()->GetProfile());
}

void AssistantIsland::OnEnabledChanged() {
  if (Enabled()) {
    return;
  }
  // 收起来，每个对话存好、结束。
  Close();
  controller_->CloseAll();
}

void AssistantIsland::EnsureWebView() {
  if (web_view_) {
    return;
  }
  Profile* profile = browser_view_->browser()->GetProfile();
  web_view_ = clip_->AddChildView(std::make_unique<AssistantWebView>(profile));
  content::WebContents* contents = web_view_->GetWebContents();
  AssistantController::SetPageOwner(contents, browser_view_);
  // 网页的底是透明的：玻璃（侧栏时是窗口的底色）透出来。
  contents->SetPageBaseBackgroundColor(SK_ColorTRANSPARENT);
  web_view_->LoadInitialURL(GURL(kPageUrl));
  Relayout();
}

void AssistantIsland::Toggle() {
  if (!open_) {
    Open(/*focus=*/true);
    return;
  }
  if (web_view_ && !web_view_->HasFocus()) {
    web_view_->RequestFocus();
    controller_->FocusComposer();
    return;
  }
  Close();
}

void AssistantIsland::Open(bool focus) {
  // 设置里关掉了：哪里都打不开（✦ 也藏起来了）。
  if (!Enabled()) {
    return;
  }
  EnsureWebView();
  if (collapsed_) {
    SetCollapsed(false);
  }
  if (!open_) {
    open_ = true;
    collapsed_ = false;
    // 关到一半又打开：从当时的样子接着变（ValueMotion 接着当时的速度）。
    const bool was_closing = GetVisible();
    SetVisible(true);
    if (GetWidget() && !monitor_) {
      // 点网页时收起；鼠标移到岛上、移开时显示、藏起小工具条（移出窗口是 exited）。
      monitor_ = views::EventMonitor::CreateWindowMonitor(
          watcher_.get(), GetWidget()->GetNativeWindow(),
          {ui::EventType::kMousePressed, ui::EventType::kMouseMoved,
           ui::EventType::kMouseExited});
    }
    if (docked_) {
      browser_view_->SetAssistantDocked(true);
    }
    height_.SetValue(TargetHeight());
    restyle_.SetValue(1);
    if (motion::ReduceMotion()) {
      morph_.SetValue(1);
      Relayout();
      open_motion_.AnimateOpacityFrom(layer(),
                                      was_closing ? layer()->opacity() : 0, 1,
                                      MotionCurve::EaseOut(kFade));
    } else {
      if (!was_closing) {
        morph_.SetValue(0);
      }
      morph_.AnimateTo(1, MotionCurve::Of(docked_ ? kSheet : kOpen));
      Relayout();
    }
    UpdateRing();
    browser_view_->SetAssistantOpen(true);
    OnStateChanged();
    UpdateBadge();
  }
  if (focus) {
    web_view_->RequestFocus();
    controller_->FocusComposer();
  }
}

void AssistantIsland::Close() {
  if (!open_) {
    return;
  }
  open_ = false;
  monitor_.reset();
  SetMiniToolbarShown(false);
  const bool had_focus = web_view_ && web_view_->HasFocus();
  browser_view_->SetAssistantDocked(false);
  if (motion::ReduceMotion()) {
    open_motion_.AnimateOpacity(layer(), 0, MotionCurve::EaseOut(kFade),
                                base::BindOnce(
                                    [](base::WeakPtr<AssistantIsland> island) {
                                      if (island && !island->open_) {
                                        island->SetVisible(false);
                                      }
                                    },
                                    weak_factory_.GetWeakPtr()));
  } else {
    // 收回小圆（停在右边时滑出去）再消失（Relayout 里看到变形停在 0 就藏起来）。
    morph_.AnimateTo(0, MotionCurve::Of(kClose));
  }
  UpdateRing();
  browser_view_->SetAssistantOpen(false);
  UpdateBadge();
  // 焦点在岛里：还给网页。
  if (had_focus) {
    if (tabs::TabInterface* tab =
            browser_view_->browser()->GetActiveTabInterface()) {
      tab->GetContents()->Focus();
    }
  }
}

void AssistantIsland::PanelStyleChanged() {
  const bool docked =
      assistant::ReadSettings(browser_view_->browser()->GetProfile()).docked();
  if (docked == docked_) {
    return;
  }
  const gfx::Rect from = GlassRect();
  docked_ = docked;
  collapsed_ = false;
  ApplyPanelStyle();
  if (!open_) {
    Relayout();
    return;
  }
  // 开着：玻璃从原来的位置滑到新的位置（弹簧 320/34），内容卡片跟着变宽、变窄。
  browser_view_->SetAssistantDocked(docked_);
  height_.SetValue(TargetHeight());
  if (motion::ReduceMotion() || from.IsEmpty()) {
    restyle_.SetValue(1);
  } else {
    restyle_from_ = from;
    restyle_.SetValue(0);
    restyle_.AnimateTo(1, MotionCurve::Of(kSheet));
  }
  UpdateRing();
  Relayout();
}

void AssistantIsland::SetLayoutBounds(const gfx::Rect& card,
                                      const gfx::Rect& dock) {
  if (card_ == card && dock_ == dock) {
    return;
  }
  card_ = card;
  dock_ = dock;
  if (!height_.is_animating()) {
    height_.SetValue(TargetHeight());
  }
  Relayout();
}

void AssistantIsland::SetContentHeight(int height) {
  if (content_height_ == height) {
    return;
  }
  content_height_ = height;
  UpdateTint();
  const int target = TargetHeight();
  if (open_ && GetVisible() && !docked_ && !motion::ReduceMotion()) {
    height_.AnimateTo(target, MotionCurve::Of(kSheet));
  } else {
    height_.SetValue(target);
  }
  Relayout();
}

int AssistantIsland::FullHeight() const {
  // 有胶囊时最高矮一排，给胶囊留出地方。
  const int capsules = capsules_->has_capsules() ? kCapsuleRow : 0;
  const int max = std::max(
      kMinHeight, static_cast<int>(card_.height() * kMaxFraction) - capsules);
  return std::clamp(content_height_ > 0 ? content_height_ : kMinHeight,
                    kMinHeight, max);
}

int AssistantIsland::TargetHeight() const {
  return collapsed_ ? kMinHeight : FullHeight();
}

void AssistantIsland::SetCollapsed(bool collapsed) {
  if (collapsed && docked_) {
    return;
  }
  if (collapsed_ == collapsed) {
    return;
  }
  collapsed_ = collapsed;
  if (motion::ReduceMotion()) {
    height_.SetValue(TargetHeight());
  } else {
    height_.AnimateTo(TargetHeight(), MotionCurve::Of(kSheet));
  }
  Relayout();
}

void AssistantIsland::OnWindowMousePress(const gfx::Point& root_point) {
  if (!open_ || !GetWidget() || docked_) {
    return;
  }
  gfx::Point point = root_point;
  views::View::ConvertPointFromWidget(this, &point);
  if (GlassRect().Contains(point)) {
    // 点回岛上：展开。
    SetCollapsed(false);
    return;
  }
  if (capsules_->HitsCapsule(gfx::Rect(point, gfx::Size(1, 1))) ||
      MiniToolbarRect().Contains(point)) {
    return;
  }
  // 点网页（或窗口里别的地方）：收成一条；有要用户回答的事时不收。
  if (controller_->has_conversation() && !controller_->waiting_for_user()) {
    SetCollapsed(true);
  }
}

gfx::Rect AssistantIsland::RestRect() const {
  if (docked_) {
    return dock_;
  }
  if (card_.IsEmpty()) {
    return gfx::Rect();
  }
  const int width = std::max(0, std::min(kMaxWidth,
                                         card_.width() - 2 * kSideMargin));
  const int height = static_cast<int>(std::round(height_.value()));
  return gfx::Rect(card_.CenterPoint().x() - width / 2,
                   card_.bottom() - kBottomMargin - height, width, height);
}

gfx::Rect AssistantIsland::GlassRect() const {
  gfx::Rect rest = RestRect();
  if (rest.IsEmpty()) {
    return rest;
  }
  // 换样子：从原来的位置滑过来。
  if (restyle_.is_animating() || restyle_.value() < 1) {
    rest = Lerp(restyle_from_, rest, std::clamp(restyle_.value(), 0.0, 1.2));
  }
  const double progress = morph_.value();
  if (std::abs(progress - 1) < 1e-4) {
    return rest;
  }
  if (docked_) {
    // 从右边滑进来。
    const int shift = static_cast<int>(
        std::round((rest.width() + metrics::kContentInset) * (1 - progress)));
    return rest + gfx::Vector2d(shift, 0);
  }
  // 从小圆到打开的样子：宽、高、下边用同一个进度，中线不动。
  const double width = kMorphSide + (rest.width() - kMorphSide) * progress;
  const double height = kMorphSide + (rest.height() - kMorphSide) * progress;
  const double closed_bottom = rest.bottom() + kMorphDrop;
  const double bottom =
      closed_bottom + (rest.bottom() - closed_bottom) * progress;
  return gfx::Rect(
      static_cast<int>(std::round(rest.CenterPoint().x() - width / 2)),
      static_cast<int>(std::round(bottom - height)),
      static_cast<int>(std::round(std::max(0.0, width))),
      static_cast<int>(std::round(std::max(0.0, height))));
}

gfx::Rect AssistantIsland::FloatingRect() const {
  if (!open_ || docked_ || !GetVisible()) {
    return gfx::Rect();
  }
  return GlassRect();
}

void AssistantIsland::Relayout() {
  // 玻璃动了：媒体岛看要不要让开（它只在要让的距离变了时才动）。
  browser_view_->MediaObstaclesChanged();
  const gfx::Rect rest = RestRect();
  if (rest.IsEmpty()) {
    return;
  }
  const gfx::Rect glass = GlassRect();
  const double progress = docked_
                              ? morph_.value()
                              : (glass.width() - kMorphSide) /
                                    std::max(1.0, rest.width() - kMorphSide);
  if (docked_) {
    clip_->SetBoundsRect(glass);
    clip_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF());
    ring_->SetRadius(0);
    ring_->SetBoundsRect(glass);
    if (web_view_) {
      web_view_->SetBounds(0, 0, rest.width(), rest.height());
    }
    if (!motion::ReduceMotion()) {
      layer()->SetOpacity(std::clamp(progress / 0.5, 0.0, 1.0));
      clip_->layer()->SetOpacity(1);
    }
  } else {
    // 只有一条输入框时是胶囊，有对话时 26；变形时不超过高度的一半。
    const bool bar = collapsed_ || FullHeight() <= kMinHeight;
    const float radius =
        std::min(bar ? kBarRadius : kSheetRadius, glass.height() / 2.0f);
    glass_->SetCornerRadius(radius, true);
    glass_->SetGlassBounds(glass);
    tint_->SetRadius(radius);
    tint_->SetBoundsRect(glass);
    clip_->SetBoundsRect(glass);
    clip_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(radius));
    ring_->SetRadius(radius);
    ring_->SetBoundsRect(glass);
    if (web_view_) {
      // 网页立刻是展开时的高度和打开时的宽度、贴着玻璃下边居中，玻璃变高、收起、
      // 变形时多出来的部分被裁掉（收起时只露出最下面的输入框），动画里不重新排版。
      const int full = FullHeight();
      web_view_->SetBounds((glass.width() - rest.width()) / 2,
                           glass.height() - full, rest.width(), full);
    }
    if (!motion::ReduceMotion()) {
      // 透明度按变形的进度算：玻璃几乎马上出现，里面的东西撑开过半才淡入、
      // 到 0.9 时全显。
      layer()->SetOpacity(std::clamp(progress / 0.15, 0.0, 1.0));
      clip_->layer()->SetOpacity(std::clamp((progress - 0.5) / 0.4, 0.0, 1.0));
    }
  }
  LayoutCapsules();
  // 展开了、有了对话、开了聊天记录、换成侧栏、关掉：标题行回来了或者不用了。
  if (!WantsMiniToolbar()) {
    SetMiniToolbarShown(false);
  }
  LayoutMiniToolbar();
  if (!motion::ReduceMotion() && !open_ && !morph_.is_animating() &&
      morph_.value() < 0.01) {
    SetVisible(false);
  }
}

void AssistantIsland::LayoutCapsules() {
  capsules_->SetBoundsRect(GetLocalBounds());
  const gfx::Rect glass = GlassRect();
  if (glass.IsEmpty()) {
    return;
  }
  if (docked_) {
    // 侧栏：输入框下面一排，平分宽度。
    capsules_->LayoutDocked(gfx::Rect(glass.x() + 8,
                                      glass.bottom() - kCapsuleHeight - 8,
                                      glass.width() - 16, kCapsuleHeight));
    capsules_->layer()->SetOpacity(1);
    return;
  }
  // 悬浮：玻璃上面一排，居中；变形时跟着里面的内容淡入淡出。
  const gfx::Rect rest = RestRect();
  capsules_->LayoutFloating(rest.CenterPoint().x(), glass.y() - kRowGap,
                            card_.width() - 32);
  const double progress =
      (glass.width() - kMorphSide) / std::max(1.0, rest.width() - kMorphSide);
  capsules_->layer()->SetOpacity(static_cast<float>(
      motion::ReduceMotion() ? 1
                             : std::clamp((progress - 0.5) / 0.4, 0.0, 1.0)));
}

void AssistantIsland::SetComposerFocused(bool focused) {
  if (composer_focused_ != focused) {
    composer_focused_ = focused;
    UpdateRing();
  }
  // 焦点进输入框时展开。
  if (focused && collapsed_) {
    SetCollapsed(false);
  }
}

void AssistantIsland::SetDragOver(bool over) {
  if (drag_over_ != over) {
    drag_over_ = over;
    UpdateRing();
  }
}

bool AssistantIsland::Collapse() {
  if (!open_ || docked_ || !controller_->has_conversation() ||
      controller_->waiting_for_user()) {
    return false;
  }
  SetCollapsed(true);
  return true;
}

void AssistantIsland::Shake() {
  if (motion::ReduceMotion() || !open_) {
    return;
  }
  // 左右偏移 -4、4、-3、2、0，平均分在 0.28 秒里，线性插值。
  auto sequence = std::make_unique<ui::LayerAnimationSequence>();
  for (int offset : {-4, 4, -3, 2, 0}) {
    gfx::Transform transform;
    transform.Translate(offset, 0);
    auto element = ui::LayerAnimationElement::CreateTransformElement(
        transform, base::Milliseconds(56));
    element->set_tween_type(gfx::Tween::LINEAR);
    sequence->AddElement(std::move(element));
  }
  layer()->GetAnimator()->StartAnimation(sequence.release());
}

void AssistantIsland::UpdateRing() {
  AssistantIslandRing::Mode mode = AssistantIslandRing::Mode::kNone;
  if (open_ && drag_over_) {
    // 拖着图片过来：一圈渐变线（侧栏时也有）。
    mode = AssistantIslandRing::Mode::kFocus;
  } else if (docked_) {
    mode = AssistantIslandRing::Mode::kNone;
  } else if (open_ && controller_->running() &&
             !controller_->waiting_for_user()) {
    mode = AssistantIslandRing::Mode::kBusy;
  } else if (open_ && composer_focused_) {
    mode = AssistantIslandRing::Mode::kFocus;
  }
  ring_->SetMode(mode);
}

void AssistantIsland::UpdateBadge() {
  // 关着时有对话等用户是警示色，在做事是渐变。
  IconBadge badge = IconBadge::kNone;
  if (!open_) {
    if (controller_->any_waiting()) {
      badge = IconBadge::kAttention;
    } else if (controller_->any_running()) {
      badge = IconBadge::kActivity;
    }
  }
  browser_view_->SetAssistantBadge(badge);
}

void AssistantIsland::OnStateChanged() {
  UpdateRing();
  UpdateTint();
  UpdateBadge();
  const bool had_capsules = capsules_->has_capsules();
  capsules_->Update(controller_->history_shown()
                        ? std::vector<AssistantController::Capsule>()
                        : controller_->Capsules());
  if (had_capsules != capsules_->has_capsules() && !docked_) {
    // 玻璃最高的高度变了。
    if (open_ && !motion::ReduceMotion()) {
      height_.AnimateTo(TargetHeight(), MotionCurve::Of(kSheet));
    } else {
      height_.SetValue(TargetHeight());
    }
  }
  Relayout();
  // 要用户回答的事（确认、问题）：岛自动打开，不抢输入焦点；是显示着的对话就展开。
  if (controller_->any_waiting() && !open_) {
    Open(/*focus=*/false);
  }
  if (controller_->waiting_for_user()) {
    SetCollapsed(false);
  }
}

void AssistantIsland::OnConversationActivated(bool focus) {
  if (!open_) {
    Open(focus);
    return;
  }
  SetCollapsed(false);
  if (focus && web_view_) {
    web_view_->RequestFocus();
  }
}

void AssistantIsland::OpenHistory() {
  SetCollapsed(false);
  controller_->OpenHistoryPanel();
}

bool AssistantIsland::WantsMiniToolbar() const {
  return open_ && !docked_ && !controller_->history_shown() &&
         !controller_->waiting_for_user() &&
         (collapsed_ || !controller_->has_conversation());
}

void AssistantIsland::OnWindowMouseMove(const gfx::Point& root_point) {
  if (!GetWidget()) {
    return;
  }
  if (!WantsMiniToolbar()) {
    SetMiniToolbarShown(false);
    return;
  }
  gfx::Point point = root_point;
  views::View::ConvertPointFromWidget(this, &point);
  // 工具条可能在胶囊上面：从玻璃移过去时经过的空隙也算在岛上（玻璃和工具条外扩
  // 8、10 合起来的外接矩形）。
  gfx::Rect area = GlassRect();
  if (mini_toolbar_shown_) {
    gfx::Rect toolbar = mini_toolbar_->toolbar_bounds();
    toolbar.Inset(gfx::Insets::VH(-10, -8));
    area.Union(toolbar);
  }
  SetMiniToolbarShown(area.Contains(point));
}

void AssistantIsland::SetMiniToolbarShown(bool shown) {
  if (mini_toolbar_shown_ == shown) {
    return;
  }
  mini_toolbar_shown_ = shown;
  if (shown) {
    mini_toolbar_->SetVisible(true);
    LayoutMiniToolbar();
  }
  // 淡入淡出 0.18 秒，淡出完再藏起来。
  mini_toolbar_motion_.AnimateOpacity(
      mini_toolbar_->layer(), shown ? 1 : 0, motion::Hover(),
      base::BindOnce(
          [](base::WeakPtr<AssistantIsland> island) {
            if (island && !island->mini_toolbar_shown_) {
              island->mini_toolbar_->SetVisible(false);
            }
          },
          weak_factory_.GetWeakPtr()));
}

void AssistantIsland::LayoutMiniToolbar() {
  const gfx::Rect glass = GlassRect();
  if (!mini_toolbar_->GetVisible() || docked_ || glass.IsEmpty()) {
    return;
  }
  const gfx::Size size = AssistantMiniToolbar::GlassSize();
  int bottom = glass.y() - kRowGap;
  if (capsules_->has_capsules()) {
    bottom -= kCapsuleHeight + kRowGap;
  }
  mini_toolbar_->SetToolbarBounds(gfx::Rect(glass.right() - size.width() - 8,
                                            bottom - size.height(),
                                            size.width(), size.height()));
}

gfx::Rect AssistantIsland::MiniToolbarRect() const {
  return mini_toolbar_shown_ ? mini_toolbar_->toolbar_bounds() : gfx::Rect();
}

bool AssistantIsland::DoesIntersectRect(const views::View* target,
                                        const gfx::Rect& rect) const {
  return open_ &&
         (GlassRect().Intersects(rect) || capsules_->HitsCapsule(rect) ||
          MiniToolbarRect().Intersects(rect));
}

BEGIN_METADATA(AssistantIsland)
END_METADATA

}  // namespace starry
