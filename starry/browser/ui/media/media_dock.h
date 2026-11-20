// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_DOCK_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_DOCK_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/prefs/pref_change_registrar.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/frame_motion.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

class BrowserWindowInterface;

namespace ui {
class Compositor;
}

namespace starry {

class MediaButton;
class MediaPlayerView;
class MediaWaveform;

// 侧栏底部的媒体播放坞：媒体岛收起后停靠在工具按钮行上方，不遮挡网页。
//
// 平时是一张和侧栏一样宽的卡片：封面、标题、艺术家和来源、播放暂停，下面一条边播边画的
// 进度；有别的媒体时卡片后面露出一两层（一摞）。鼠标停 0.3 秒（或者点一下）展开，Mac 上
// 触控板轻振一下：卡片以左下角为锚点往上、往右长成完整的播放器（宽 340，可以越过侧栏盖住
// 网页左边一条），封面 36 → 64、标题 13 → 15 原地长大，波形、进度、控制和其余的媒体跟着
// 上边升起来；右上角是浮出、收起。离开 0.3 秒收回。
//
// 视图一直是展开后那么大，左下角对齐卡片（侧栏只给卡片留 kSlotHeight 的位置）。展开、收回
// 时播放器和文字不重新排版、不重画：只改裁剪框和各层的位置、变换、不透明度；底板和封面
// 跟着大小重画（都很小）。收着时面板之外不接鼠标，上面的标签和右边的网页照常能点。
class MediaDock : public views::View,
                  public MediaHub::Observer,
                  public views::ViewTargeterDelegate {
  METADATA_HEADER(MediaDock, views::View)

 public:
  // 卡片 62 高，上面 8 露出后面那一摞。
  static constexpr int kCardHeight = 62;
  static constexpr int kStackHeight = 8;
  static constexpr int kSlotHeight = kCardHeight + kStackHeight;
  // 展开后的宽度（和媒体岛展开后一样）；侧栏更宽时跟着侧栏，窗口太窄时跟着窗口。
  static constexpr int kExpandedWidth = 340;

  explicit MediaDock(BrowserWindowInterface* browser);
  MediaDock(const MediaDock&) = delete;
  MediaDock& operator=(const MediaDock&) = delete;
  ~MediaDock() override;

  void SetTheme(const Theme& theme);
  // 侧栏布局时才有（顶部标签布局在标签行里）。
  void SetSidebarActive(bool active);
  // 该不该占位置。变了时调用 presence_changed（侧栏动画让出、合拢位置）。
  bool ShouldShow() const { return should_show_; }
  void SetPresenceChangedCallback(base::RepeatingClosure callback);
  // FullHeight 变了（其余的媒体多了、少了）：侧栏重新排。
  void SetLayoutChangedCallback(base::RepeatingClosure callback);
  // 视图要多高：展开后的样子。
  int FullHeight() const;
  // 最多能多高（到标签列表的顶上）：放不下时其余的媒体少列几行。
  void SetMaxHeight(int max_height);
  // 收着时多宽（侧栏里的宽度），展开后最多能多宽（不出窗口）。
  void SetWidths(int collapsed_width, int max_width);
  // 视图要多宽：展开后的样子。
  int ExpandedWidth() const;
  // 面板现在露出来的部分（本视图的坐标）。
  gfx::Rect PanelRect() const;
  // 卡片（不含后面那一摞、不算展开）在本视图里的位置：媒体岛飞过来的终点。
  gfx::Rect CardRect() const;
  void SetExpanded(bool expanded);
  bool expanded() const { return expanded_; }
  // 媒体岛飞到了、开始和它交叉淡化：内容淡入（没有飞过来的窗口等一会儿自己淡入）。
  void Reveal();
  // 调试步骤 mediainfo 用：展开的程度、面板现在的位置（本视图的坐标），最近一次展开、
  // 收回的帧数和帧间隔。
  std::string DebugState() const;

  // MediaHub::Observer:
  void OnMediaChanged() override;

  // views::View:
  void Layout(PassKey) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  void RemovedFromWidget() override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  class Piece;
  class Row;

  void Update();
  void OnDockedChanged();
  void OnPlayerSizeChanged();
  // 鼠标停够了：展开（停在播放暂停上时不展开，免得要点的按钮跑掉）。
  void HoverExpand();
  // 鼠标离开够久了：收回（展开的半路上鼠标出去又落进了长大的面板时不收）。
  void LeaveCollapse();
  // 展开、收回的每一帧：面板的大小，各层的位置、变换和不透明度。
  void ApplyFrame();
  // 展开的程度走了一帧。
  void OnExpandFrame();
  // 现在显示的是哪一个：展开着时是播放器里选中的，收着时是主要的那个。
  const MediaEntry* Entry() const;
  int CollapsedWidth() const;
  // 卡片上封面的宽（视频在宽卡片上是宽的）。
  int CardArtWidth() const;
  ui::Compositor* Compositor();

  void PaintSheet(gfx::Canvas* canvas, const gfx::Size& size);
  void PaintShadow(gfx::Canvas* canvas, const gfx::Size& size);
  void PaintStack(gfx::Canvas* canvas, const gfx::Size& size);
  void PaintArt(gfx::Canvas* canvas, const gfx::Size& size);
  void PaintCardTitle(gfx::Canvas* canvas, const gfx::Size& size);
  void PaintFullTitle(gfx::Canvas* canvas, const gfx::Size& size);

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<MediaHub> hub_;
  Theme theme_ = Theme::DarkBase();
  Theme media_theme_ = Theme::DarkBase();
  MediaEntry entry_;
  bool has_entry_ = false;
  size_t count_ = 0;
  // 底上那层封面的颜色是按在放还是暂停画的。
  bool sheet_playing_ = false;
  bool sidebar_active_ = true;
  bool should_show_ = false;
  bool expanded_ = false;
  int max_height_ = 0;
  int full_height_ = 0;
  int collapsed_width_ = 0;
  int max_width_ = 0;
  bool setting_max_height_ = false;
  // 展开的半路上鼠标离开了（收回前再看一眼它是不是落进了长大的面板）。
  bool exited_while_expanding_ = false;
  // 底板、封面现在的圆角（跟着展开的程度变）。
  float sheet_radius_ = 12;
  float art_radius_ = 7;
  base::RepeatingClosure presence_changed_;
  base::RepeatingClosure layout_changed_;

  // 后面那一摞（在面板下面，按收着的宽度画）。
  raw_ptr<Piece> stack_ = nullptr;
  // 面板的投影：按展开后的大小画一次，中途以左下角为锚点缩放到面板的大小。
  raw_ptr<Piece> shadow_ = nullptr;
  // 四个角是圆的裁剪框，一直是面板现在的大小；下面这些都在里面，按面板的左上角摆，跟着
  // 上边升起来。
  raw_ptr<views::View> clip_ = nullptr;
  raw_ptr<Piece> sheet_ = nullptr;
  // 点一下展开、收回的那一块（卡片 / 最上面封面和标题那一行，露出来多少按裁剪框算），
  // 不画东西。
  raw_ptr<Row> row_ = nullptr;
  // 封面：跟着大小重画（36 → 64，视频在宽卡片上从宽的变成方的）。
  raw_ptr<Piece> art_ = nullptr;
  // 卡片上的标题（13）和展开后的标题（15，多一行来源）：一起平移，交叉淡化，展开后的那份
  // 从 13/15 放大到 1。
  raw_ptr<Piece> card_title_ = nullptr;
  raw_ptr<Piece> full_title_ = nullptr;
  // 卡片才有的：播放暂停、边播边画的进度。
  raw_ptr<MediaButton> toggle_ = nullptr;
  raw_ptr<MediaWaveform> wave_ = nullptr;
  // 展开后右上角的浮出、收起。
  raw_ptr<MediaButton> float_ = nullptr;
  raw_ptr<MediaButton> collapse_ = nullptr;
  raw_ptr<MediaPlayerView> player_ = nullptr;

  // 展开的程度：0 是卡片，1 是播放器。
  FrameMotion expand_;
  // 展开着时高度变了：面板的上边先补回变的那么多，再滑到位。
  FrameMotion resize_;
  LayerMotion fade_;
  // 最近一次展开、收回走了多少帧，两帧之间隔了多久（DebugState）。
  int frames_ = 0;
  double frame_gap_sum_ = 0;
  double frame_gap_max_ = 0;
  base::TimeTicks last_frame_;
  base::OneShotTimer hover_timer_;
  base::OneShotTimer leave_timer_;
  // 媒体岛飞进来时晚一点再淡入（等它落位）。
  base::OneShotTimer reveal_timer_;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<MediaHub, MediaHub::Observer> observation_{this};
  base::WeakPtrFactory<MediaDock> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_DOCK_H_
