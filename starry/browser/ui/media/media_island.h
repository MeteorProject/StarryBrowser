// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_ISLAND_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_ISLAND_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/prefs/pref_change_registrar.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/media/media_pip.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

namespace views {
class EventMonitor;
}

namespace starry {

class GlassView;
class MediaPlayerView;
class StarryBrowserView;

// 媒体岛：浮在内容卡片一角的媒体控件，有媒体在播放且不在当前标签可见时出现。
// 默认位于右下角，可拖到四个角，松手吸附到最近的角（kMediaCorner，跨窗口同步）；
// 助手岛、查找栏、智能体卡片遮挡该角时自动平移让开。
//
// 音乐：一颗胶囊，封面（外面一圈进度）、标题和滚动的响度、播放暂停；旁边几颗「卫星」是
// 其余的媒体，点一下换过来。胶囊是实色的（从封面取色），不用实时玻璃：常驻在网页上的
// 背景滤镜会让 macOS 整帧改走 Chromium
// 自己合成。鼠标停 0.3 秒
// 撑开成播放器（MediaPlayerView，实时玻璃，临时的），离开 0.3 秒收回。
//
// 视频（窗内画中画，MediaPip::Host）：视频在放的标签切到后台时，画面从网页里原来的位置
// 飞到角上继续放；切回那个标签时飞回去，落地后才关掉画中画。悬停时画面上出控件（回到
// 标签页、弹到桌面、关闭、播放暂停、进度）。拖出窗口也是弹到桌面：指针出了窗口再走
// kPopOutSlack 才算，画面暗下来、写「松手弹到桌面」，拖回窗口里取消。角上已经有一段自动进来、
// 又暂停着的视频时，刚切走的在放的视频换掉它（在放的、手动放上来的不换）。卫星竖着排在旁边，点
// 一颗在视频朝里的一边展开播放器（放的是点的那个），画面留在原处：画中画开着就一定看得
// 见。关闭以后（暂停了）它不再以胶囊冒出来，再放起来、切到它的标签时才算数。
//
// 收起（kMediaDocked）：岛沿弧线飞进侧栏底部的坞（顶部标签布局时是标签行里的胶囊）并
// 变形成它；浮出时从那里飞回来。收起只管音乐：窗内的画中画照样浮在角上（没有卫星，
// 其余的在坞里），视频回到网页、关掉以后网页上什么都不盖。
//
// 桌面的画中画小窗盖到这个窗口时也当作挡着的东西让开（StarryBrowserView::MediaObstacles）。
//
// 铺满窗口（StarryBrowserView
// 的子视图），只有胶囊、视频、卫星和展开的卡片接鼠标。网页上 的鼠标移动 Views
// 收不到，悬停靠窗口的事件监视器。
class MediaIsland : public views::View,
                    public views::ViewTargeterDelegate,
                    public MediaHub::Observer,
                    public TabStripModelObserver,
                    public MediaPip::Host {
  METADATA_HEADER(MediaIsland, views::View)

 public:
  explicit MediaIsland(StarryBrowserView* browser_view);
  MediaIsland(const MediaIsland&) = delete;
  MediaIsland& operator=(const MediaIsland&) = delete;
  ~MediaIsland() override;

  void SetTheme(const Theme& theme);
  // 内容卡片（本视图的坐标），布局之后给。
  void SetCardBounds(const gfx::Rect& card);
  // 窗口里的鼠标移动（Widget
  // 的坐标）。真的移动由窗口的事件监视器转过来，调试步骤 mediahover 也走这里。
  void OnWindowMouseMove(const gfx::Point& root_point);
  void SetExpanded(bool expanded);
  bool expanded() const { return expanded_; }
  bool showing() const { return shown_; }
  // 胶囊、视频、展开的卡片现在的位置（本视图的坐标）；没显示时为空。
  gfx::Rect SurfaceRect() const;
  // 拖动胶囊、视频（屏幕坐标；调试步骤 mediadrag 也走这里）。
  void BeginDrag(const gfx::Point& screen_point);
  void DragTo(const gfx::Point& screen_point);
  void EndDrag(const gfx::Point& screen_point);
  // 盖在角上的东西（助手的岛、查找栏、智能体的卡片）变了：要不要让开。
  void UpdateAvoidance();
  // 窗内放着的画中画；没有时为空。
  MediaPip* video() const { return pip_; }
  // 调试：一行说明现在的状态。
  std::string DebugState() const;

  // MediaHub::Observer:
  void OnMediaChanged() override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

  // MediaPip::Host:
  bool CanHostVideo(content::WebContents* contents) const override;
  void AttachVideo(MediaPip* pip) override;
  void DetachVideo(MediaPip* pip) override;
  void VideoChanged(MediaPip* pip) override;
  void VideoClosedByUser(content::WebContents* contents) override;
  void DesktopVideoMoved() override;

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  class Capsule;
  class Satellite;
  class Tint;
  class VideoCard;
  class EventWatcher;
  friend class EventWatcher;

  // 角：右边、下边各一位。
  static constexpr int kRight = 1;
  static constexpr int kBottom = 2;

  // 岛该不该显示、显示哪一个（不算收起、飞行）。
  void Update();
  const MediaEntry* Shown() const;
  std::optional<base::UnguessableToken> ActiveTabMedia() const;
  // 窗内画中画那个标签的媒体 id。
  std::optional<base::UnguessableToken> VideoMediaId() const;
  // 在桌面小窗里放着的媒体 id：已经看得见，岛上不再放它。
  std::optional<base::UnguessableToken> DesktopVideoMediaId() const;
  // 主体是视频（有窗内的画中画）。
  bool ShowsVideo() const;
  // 点了一颗卫星：换到岛上；视频在角上时在它旁边展开播放器。
  void SelectMedia(const base::UnguessableToken& id);
  // 用户关掉的那个又放起来了（先暂停、再播放）、没了：不再躲着。
  void UpdateDismissed();
  bool Docked() const;
  void OnDockedChanged();
  void OnMediaSettingsChanged();
  void OnCornerChanged();
  void OnWindowMousePress(const gfx::Point& root_point);
  void SetShown(bool shown, bool animate = true);
  void UpdateMonitor();
  // 主体在角上的位置（算上让位，不算拖动）。
  gfx::Rect Anchored(const gfx::Size& size) const;
  // 拖动、松手吸附时离角上的位置差多少。
  gfx::Vector2d MoveOffset() const;
  // 胶囊、视频、展开的卡片的位置。
  gfx::Rect CompactRect() const;
  gfx::Rect ExpandedRect() const;
  // 展开前的样子：胶囊；视频在角上时是贴着视频的一条（播放器从那里长出来）。
  gfx::Rect CollapsedRect() const;
  // 展开时鼠标在这里面不算离开：播放器，视频在角上时再加上视频和卫星。
  gfx::Rect ExpandedArea() const;
  gfx::Size VideoSize() const;
  gfx::Rect VideoRect() const;
  gfx::Rect MainRect() const;
  // 主体加上旁边的卫星占的地方（让位按这个算）。
  gfx::Rect FootprintAt(const gfx::Rect& main) const;
  // 飞行的另一头：侧栏的坞、标签行的胶囊（本视图的坐标）。
  gfx::Rect DockRect() const;
  // 视频在网页里原来的位置（本视图的坐标）；不知道时为空。
  gfx::Rect VideoSourceRect() const;
  // 视频的标签切到后台了：让它进画中画、放到岛上。
  void MaybeInlineVideo(content::WebContents* contents);
  // 视频的标签切回来了：画面飞回网页，落地后关掉画中画。
  void ReturnVideo();
  void Relayout();
  void LayoutSatellites(const gfx::Rect& main, bool video, float opacity);
  // 玻璃、底色和播放器：从 from 展开到 to，e 是 expand_ 的值。
  void LayoutPlayer(const gfx::Rect& from, const gfx::Rect& to, double e);
  // 主体要换了（视频上岛、离开）：还展开着的播放器在现在的位置收回去（在换之前调）。
  void CollapseInPlace();
  // 拖着视频时指针到了哪（屏幕坐标）：松手会不会弹到桌面，画面上的提示。
  void UpdatePopOutHint(const gfx::Point& screen_point);
  void OnFlightStep();
  void OnVideoFlightStep();
  void RemoveCard(VideoCard* card);
  void UpdateVideoStacking();

  const raw_ptr<StarryBrowserView> browser_view_;
  const raw_ptr<MediaHub> hub_;
  Theme theme_ = Theme::DarkBase();
  gfx::Rect card_;

  raw_ptr<GlassView> glass_ = nullptr;
  raw_ptr<Tint> tint_ = nullptr;
  raw_ptr<Capsule> capsule_ = nullptr;
  raw_ptr<MediaPlayerView> player_ = nullptr;
  std::vector<raw_ptr<Satellite>> satellites_;

  // 岛上显示的是哪一个：点卫星、在展开的卡片里点一行时换；没选过时跟着焦点会话（有窗内
  // 画中画时是视频）。
  std::optional<base::UnguessableToken> selected_;
  // 用户点关闭关掉画中画的那个媒体：再放起来以前不当岛上的主体。dismissed_paused_：
  // 关掉以后已经看到过它暂停（之后的「在放」才是再放起来）。
  std::optional<base::UnguessableToken> dismissed_;
  bool dismissed_paused_ = false;
  bool shown_ = false;
  bool expanded_ = false;
  bool docked_ = false;
  // 展开的程度：0 是胶囊，1 是播放器（回弹时会超过 1）。
  ValueMotion expand_;
  // 收起、浮出的飞行：0 在胶囊的位置，1 在坞的位置。
  ValueMotion flight_;
  // 飞行的终点：飞进坞时每一帧重新取（坞也在动）；浮出时是开始那一刻坞的位置。
  gfx::Rect flight_dock_;
  bool flying_ = false;
  bool flying_to_dock_ = false;
  bool flight_fading_ = false;
  LayerMotion fade_;
  LayerMotion capsule_fade_;

  // 窗内的画中画：放上来时拿到，拿走时清掉。
  raw_ptr<MediaPip> pip_ = nullptr;
  raw_ptr<VideoCard> video_card_ = nullptr;
  // 视频的飞行：0 在网页里原来的位置，1 在角上。
  ValueMotion video_flight_;
  gfx::Rect video_source_;
  bool returning_ = false;

  // 在哪个角（kRight | kBottom）。
  int corner_ = kRight | kBottom;
  bool dragging_ = false;
  gfx::Point drag_start_;
  gfx::Vector2d drag_base_;
  gfx::Vector2d drag_offset_;
  // 松手吸附：从 snap_offset_（离新的角差多少）乘 1 动到乘 0。
  ValueMotion snap_;
  gfx::Vector2d snap_offset_;
  // 拖动时略放大：0…1。
  ValueMotion lift_;
  // 让位：离角往里挪多少（像素）。
  ValueMotion avoid_;
  int avoid_target_ = 0;
  // 胶囊（连同卫星）显示的程度：视频上岛时在 capsule_exit_ 淡出，视频离开后在角上淡入。
  ValueMotion capsule_in_;
  gfx::Rect capsule_exit_;
  // 主体换了时还展开着的播放器：从哪里、到哪里收回（原来的位置，不跳到新的）。
  std::optional<std::pair<gfx::Rect, gfx::Rect>> player_exit_;
  // 拖着视频到了窗口外面：松手弹到桌面。
  bool pop_out_armed_ = false;

  base::OneShotTimer hover_timer_;
  base::OneShotTimer leave_timer_;
  base::OneShotTimer video_leave_timer_;
  std::unique_ptr<EventWatcher> watcher_;
  std::unique_ptr<views::EventMonitor> monitor_;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<MediaHub, MediaHub::Observer> observation_{this};
  base::WeakPtrFactory<MediaIsland> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_ISLAND_H_
