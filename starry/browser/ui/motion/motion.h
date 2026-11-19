// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_MOTION_H_
#define STARRY_BROWSER_UI_MOTION_MOTION_H_

#include <vector>

#include "starry/browser/ui/motion/spring.h"

namespace starry {

// 动效曲线：阻尼弹簧、定长三次贝塞尔或关键帧。
class MotionCurve {
 public:
  enum class Kind { kSpring, kTimed, kKeyframes };

  MotionCurve();
  MotionCurve(const MotionCurve&);
  MotionCurve& operator=(const MotionCurve&);
  ~MotionCurve();

  static MotionCurve Of(const Spring& spring);
  // 定长三次贝塞尔曲线。
  static MotionCurve Timed(double seconds,
                           float x1,
                           float y1,
                           float x2,
                           float y2);
  // 常用预设缓动曲线。
  static MotionCurve EaseIn(double seconds);
  static MotionCurve EaseOut(double seconds);
  static MotionCurve EaseInOut(double seconds);
  static MotionCurve Linear(double seconds);
  // 关键帧曲线：values 为绝对值（缩放或不透明度），
  // key_times 是 0…1 的时刻，每段按 easeInEaseOut 插值。
  static MotionCurve Keyframes(double seconds,
                               std::vector<float> values,
                               std::vector<float> key_times);

  Kind kind() const { return kind_; }
  bool is_spring() const { return kind_ == Kind::kSpring; }
  const Spring& spring() const { return spring_; }

  // 一个分量从 from 以初速度 velocity（只有弹簧用）动到 to，t 秒时的值。关键帧不管
  // from、to，直接是关键帧的值。
  double ValueAt(double from, double to, double velocity, double t) const;
  // 动画多长：弹簧是位移小于 epsilon 停下的时间。
  double Duration(double from, double to, double velocity,
                  double epsilon) const;

 private:
  Kind kind_ = Kind::kTimed;
  Spring spring_;
  double seconds_ = 0;
  // 三次贝塞尔的两个控制点（gfx::CubicBezier 不能赋值，用到时再建）。
  float x1_ = 0, y1_ = 0, x2_ = 1, y2_ = 1;
  std::vector<float> values_;
  std::vector<float> key_times_;
};

// 集中定义的动效参数与曲线，避免在视图中硬编码。
namespace motion {

// 基础交互
inline constexpr Spring kSelection{0.36, 0.8};
// 面板左边紧贴网页边缘，用临界阻尼避免越过终点露出白缝。
inline constexpr Spring kSidebarPeek{0.21, 1};
inline constexpr Spring kFold{0.34, 0.88};
inline constexpr Spring kReveal{0.55, 0.86};
inline constexpr Spring kLift{0.35, 0.78};
inline constexpr Spring kListEdit{0.42, 0.86};
inline constexpr Spring kSearchOpen{0.38, 0.84};
inline constexpr Spring kSearchClose{0.26, 1};
inline constexpr Spring kSearchResize{0.3, 0.9};
inline constexpr double kStaggerStep = 0.028;
inline constexpr int kStaggerLimit = 14;
MotionCurve Popover();        // timingCurve(0.16, 1, 0.3, 1, 0.2)
MotionCurve Sidebar();        // timingCurve(0.2, 0.8, 0.2, 1, 0.32)
MotionCurve SidebarUnpeek();  // timingCurve(0.25, 0.1, 0.25, 1, 0.15)
MotionCurve Hover();          // easeOut 0.18
MotionCurve Tab();            // timingCurve(0.16, 1, 0.3, 1, 0.36)
MotionCurve Accent();         // easeInOut 0.8

// 浏览器
inline constexpr Spring kTabInsert{0.38, 0.82};
// 关闭标签：不回弹，干脆地收起。
inline constexpr Spring kTabClose{0.28, 1};
inline constexpr Spring kCommandBarOpen = kSearchOpen;
inline constexpr Spring kCommandBarClose = kSearchClose;
inline constexpr Spring kFindBar{0.34, 0.86};
inline constexpr Spring kTabPreview{0.26, 0.92};
inline constexpr Spring kTabPreviewMove{0.3, 0.88};
inline constexpr Spring kSplitLayout{0.42, 0.86};
// 分屏的落点预览换边、换格子；把手展开收起；分隔线拖过头时「松开以移出」。
inline constexpr Spring kSplitDropPreview{0.26, 0.84};
inline constexpr Spring kSplitHandle{0.3, 0.8};
inline constexpr Spring kSplitCollapseArm = kLift;
inline constexpr Spring kDialogIn{0.34, 0.84};
// 右键菜单打开：从打开的那个角轻微放大。
inline constexpr Spring kMenuOpen{0.22, 0.84};
// 右键菜单的选中高亮滑到另一行：鼠标扫过时跟得上，不拖尾。
inline constexpr Spring kMenuHighlight{0.16, 0.92};
MotionCurve ChromeLayout();  // 同 Sidebar()
MotionCurve Progress();      // easeOut 0.25
MotionCurve AddressSwap();   // easeOut 0.12
MotionCurve ChromeTint();    // 同 Accent()
MotionCurve DialogBackdrop();  // easeOut 0.18
MotionCurve DialogOut();       // easeOut 0.12
MotionCurve TabPreviewHide();  // easeOut 0.12
// 按下按钮：缩到 0.96，easeOut 0.1。
inline constexpr float kPressScale = 0.96f;
MotionCurve Press();

// 标签控件
// 点标签时图标轻压再略微过冲回弹。
MotionCurve TabClickBounce();
// 新标签插入：从左边 14 pt 滑进来并淡入；关掉：原地缩到 0.96 淡出。
inline constexpr float kTabInsertOffset = 14;
inline constexpr double kTabInsertFade = 0.22;
inline constexpr float kTabCloseScale = 0.96f;
inline constexpr double kTabCloseFade = 0.18;
// 固定标签的图块从 0.6 缩放淡入。
inline constexpr float kPinnedInsertScale = 0.6f;
// 顶部标签行的新标签从左边 0.92 放大淡入。
inline constexpr float kTopTabInsertScale = 0.92f;
// 文件夹开合（侧栏的固定标签）：展开时下面的行让开，带一点回弹，里面的行从上方
// kFolderRevealOffset 处落下来；折叠不回弹，里面的行淡出（秒）。
inline constexpr Spring kFolderOpen{0.36, 0.84};
inline constexpr Spring kFolderClose{0.28, 1};
inline constexpr float kFolderRevealOffset = 8;
inline constexpr double kFolderHideFade = 0.12;
// 展开时里面的行淡入（秒，easeInOut）：开头慢一点，等下面的行先让开。
inline constexpr double kFolderRevealFade = 0.26;
// 拖着的标签原地变淡到这么多。
inline constexpr float kTabLiftedOpacity = 0.4f;
// 标签悬停时底色、关闭按钮的淡入淡出（同 Hover）。
inline constexpr double kHoverDuration = 0.18;
// 拖动标签时插入位置的提示线移动、出现。
inline constexpr double kDropMarkerDuration = 0.14;
// 拖着东西停在折叠的文件夹上这么久（秒），文件夹自己展开。
inline constexpr double kFolderSpringLoad = 0.6;
// 拖着东西停在图块、胶囊上自己落下来的卡片：鼠标离开卡片和图块（胶囊）这么久
// （秒）就收起来，在这之前回来不收。
inline constexpr double kFolderSpringLeave = 0.3;
// 拖着的东西会放进这个文件夹时，它的图标打开并放大到这么多。
inline constexpr float kFolderDropIconScale = 1.08f;
// 松手放进文件夹：跟着鼠标的小条朝文件夹图标缩到这么多、淡出（秒）。
inline constexpr float kFolderDropPillScale = 0.35f;
inline constexpr double kFolderDropPillFade = 0.18;
// 常用图块的文件夹点开：卡片从图块长出来（玻璃的位置、大小、圆角一起变，带
// 一点回弹）；收起时缩回图块，不回弹。
inline constexpr Spring kTileCardOpen{0.42, 0.82};
inline constexpr Spring kTileCardClose{0.3, 1};

// 右键菜单
inline constexpr double kMenuFadeIn = 0.09;
inline constexpr double kMenuFadeOut = 0.12;
inline constexpr double kMenuHighlightFade = 0.08;
inline constexpr double kMenuFlash = 0.12;
inline constexpr double kMenuSubmenuDelay = 0.12;
inline constexpr double kMenuSubmenuGrace = 0.28;
inline constexpr float kMenuOpenScale = 0.95f;
inline constexpr float kSubmenuOpenScale = 0.97f;

// 对话框
inline constexpr float kDialogNudgeScale = 1.018f;
inline constexpr double kDialogNudgeOut = 0.09;
inline constexpr double kDialogNudgeBack = 0.24;

// 小窗口
// 出现：窗口淡入（秒，easeOut），里面的卡片从 kMiniWindowCardScale 放大并淡入（同对话框的
// 卡片）。
inline constexpr double kMiniWindowFadeIn = 0.14;
inline constexpr float kMiniWindowCardScale = 0.97f;
inline constexpr Spring kMiniWindowIn = kDialogIn;
// 关掉：淡出（秒，easeIn），不回弹。
inline constexpr double kMiniWindowFadeOut = 0.12;
// 移到浏览器窗口：网页的截图从小窗口飞到内容卡片，过 kMiniTransferFlight 秒（弹簧基本到位）
// 开始淡出（秒），露出真的网页。
inline constexpr Spring kMiniTransfer{0.42, 0.86};
inline constexpr double kMiniTransferFlight = 0.4;
inline constexpr double kMiniTransferFadeOut = 0.16;

// AI 智能体在网页与标签上的视觉提示
// 光标移到要操作的元素：要让人看清它去了哪里，又不拖沓。
inline constexpr Spring kAgentCursor{0.42, 0.86};
// 光标开始移动后、真正点下之前等的时间（秒），大致是 kAgentCursor 到位的时间。
inline constexpr double kAgentCursorTravel = 0.38;
// 要操作的元素外面的高亮框：出现、换到另一个元素。
inline constexpr Spring kAgentHighlight{0.34, 0.86};
// 点下时光标轻压再弹回，一圈波纹从点击处扩散淡出（秒）。
inline constexpr double kAgentRipple = 0.55;
// 读页面（快照、正文、截图）时一道光带从上往下扫过网页（秒）。
inline constexpr double kAgentScan = 0.9;
// 正在做一步时网页外圈的描边呼吸一趟（秒）；等下一步时停在较淡的一档。
inline constexpr double kAgentGlowPulse = 1.1;
// 底部提示条出现、收起和换文字。
inline constexpr Spring kAgentStatus{0.32, 0.88};
// 拖放：按住元素后光标缓起缓停地滑到目标（秒），和真的拖动事件同步。
inline constexpr double kAgentDragTravel = 0.7;
MotionCurve AgentDrag();  // easeInOut kAgentDragTravel
// 智能体操作着的标签：图标外的渐变环做事时转一圈（秒，匀速）。
inline constexpr double kAgentTabRingSpin = 1.6;
// 在后台操作的标签：内容区右下角的小卡片出现、收起和排列。
inline constexpr Spring kAgentBackground{0.36, 0.86};
// 光标点下：轻压到 0.8 再带轻微回弹恢复。
MotionCurve AgentPress();
// 侧栏、工具栏助手图标上的状态角标出现、消失。
inline constexpr Spring kAssistantBadge = kLift;

// 触控板返回、前进手势指示器（SwipeIndicator）
// 圆泡跟手弹簧。
inline constexpr Spring kSwipeFollow{0.15, 0.86};
// 上膛：圆泡鼓一下再回来；退膛：轻轻缩一下。
MotionCurve SwipeArm();     // 关键帧 0.34 秒：1 → 1.14 → 0.98 → 1
MotionCurve SwipeDisarm();  // 关键帧 0.2 秒：1 → 0.92 → 1
// 上膛时填上主色、箭头反白（秒，easeOut）。
inline constexpr double kSwipeArmFade = 0.14;
// 历史列表展开；选中的一行、圆泡上下滑到另一行。
inline constexpr Spring kSwipeList{0.32, 0.84};
inline constexpr Spring kSwipeSelection{0.2, 0.86};
// 松手翻页：圆泡往前冲、整层淡出（秒，easeOut）；取消：缩回边缘后面（不回弹）、淡出
// （秒，easeIn）。
inline constexpr Spring kSwipeLaunch{0.3, 0.9};
inline constexpr double kSwipeCommitFade = 0.22;
inline constexpr Spring kSwipeRetract{0.26, 1};
inline constexpr double kSwipeCancelFade = 0.24;

// 媒体岛与媒体坞（ui/media/）
// 岛悬停展开：同 AI 助手的岛撑开（带一点回弹）；收回不回弹。
inline constexpr Spring kMediaIslandExpand{0.39, 0.78};
inline constexpr Spring kMediaIslandCollapse{0.3, 1};
// 悬停多久展开、离开多久收回（秒）。
inline constexpr double kMediaIslandHoverDelay = 0.3;
inline constexpr double kMediaIslandLeaveDelay = 0.3;
// 收进侧栏、标签行，浮到网页上：外框沿弧线飞过去变形（同分屏的变形）；最后这么长（秒）
// 和坞交叉淡化。
inline constexpr Spring kMediaDockFlight = kSplitLayout;
inline constexpr double kMediaDockFlightFade = 0.14;
// 坞的位置让出来、合拢；展开着时多一行、少一行。
inline constexpr Spring kMediaDockRow = kListEdit;
// 坞展开：卡片像抽屉一样升上去，到位时轻轻顿一下；收回不回弹。
inline constexpr Spring kMediaDockExpand{0.4, 0.84};
inline constexpr Spring kMediaDockCollapse{0.3, 1};
// 出现、消失（胶囊、坞的内容；秒）。
inline constexpr double kMediaFade = 0.2;
// 视频切出到岛上：同小窗口转场，轻微越过落点再回稳。
inline constexpr Spring kMediaVideoToIsland = kMiniTransfer;
// 返回网页：保持同样的响应时间，临界阻尼，停在播放器的位置、不回弹。
inline constexpr Spring kMediaVideoToPage{kMiniTransfer.response, 1};
// 弹到桌面：小窗从岛上画面的位置飞到它自己的位置。
inline constexpr Spring kMediaPipFlight = kSplitLayout;
// 拖动岛：跟手、略放大；松手吸到最近的角。
inline constexpr double kMediaIslandLiftScale = 1.03;
inline constexpr Spring kMediaIslandSnap = kLift;
// 让位：助手的岛、查找栏、智能体的卡片出现时平移让开，它们走了再回来。
inline constexpr Spring kMediaIslandAvoid = kFindBar;

// 滚动区域拉过头后的回弹（ElasticScrollView）：临界阻尼，无回荡，约半秒复位。
inline constexpr Spring kScrollBounce{0.5, 1};

// 系统设置了「减弱动态效果」：位移类的动画换成淡入淡出。
bool ReduceMotion();

}  // namespace motion

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_MOTION_H_
