// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_PLAYER_VIEW_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_PLAYER_VIEW_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/media/media_parts.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

namespace starry {

class MediaButton;
class MediaSeekBar;
class MediaWaveform;

// 展开的媒体播放器内容视图：供媒体岛悬停展开、侧栏播放坞展开及标签行胶囊弹出框共用。
// 仅绘制内部内容，背景（玻璃或实色）由外层容器提供。
//
// 从上到下：标题行（「正在播放」、个数，右边是换位置、收起的按钮）；封面 64、标题、
// 艺术家、来源；实时波形；进度条和时间；控制（静音，上一首 / 后退 10 秒、播放暂停、
// 下一首 / 前进 10 秒，画中画或者转到标签页）；其余的媒体一行一个，点一行换到上面。
// 卡片的颜色从选中的那个的封面取。
//
// 没有标题行的样子（Options::header = false，侧栏的坞）从波形开始：封面和标题那一行
// 坞自己画（展开时它们从卡片上长大），换位置、收起的按钮也在坞上。
class MediaPlayerView : public views::View, public MediaHub::Observer {
  METADATA_HEADER(MediaPlayerView, views::View)

 public:
  struct Options {
    Options();
    Options(const Options&);
    Options& operator=(const Options&);
    ~Options();

    // 标题行右边换位置的按钮（收进侧栏、收进标签行、浮到网页上）；没有时不显示。
    std::optional<MediaGlyph> placement_glyph;
    std::u16string placement_tooltip;
    base::RepeatingClosure placement;
    // 标题行最右边的收起按钮；没有时不显示。
    base::RepeatingClosure collapse;
    // 内容的高度变了（其余的媒体多了、少了）。
    base::RepeatingClosure size_changed;
    // 选中的那个换了（点了下面的一行）。
    base::RepeatingClosure selection_changed;
    // 上面的标题行和封面那一行，见上面。
    bool header = true;
    // 在这个标签里的媒体不显示（岛：当前标签里看得见的那个）。
    std::optional<base::UnguessableToken> exclude;
  };

  static constexpr int kMaxOthers = 4;

  MediaPlayerView(MediaHub* hub, Options options);
  MediaPlayerView(const MediaPlayerView&) = delete;
  MediaPlayerView& operator=(const MediaPlayerView&) = delete;
  ~MediaPlayerView() override;

  void SetTheme(const Theme& theme);
  // 选中的那个（上面大的部分）；没有时是 MediaHub::Primary。
  void Select(const base::UnguessableToken& id);
  std::optional<base::UnguessableToken> selected() const;
  void SetExclude(std::optional<base::UnguessableToken> exclude);
  // 换位置的按钮换样子（布局变了：收进侧栏还是标签行）。
  void SetPlacement(MediaGlyph glyph, const std::u16string& tooltip);
  // 选中的那个的颜色（外面画底用）。
  const Theme& media_theme() const { return media_theme_; }
  int ContentHeight() const;
  bool empty() const { return entries_.empty(); }
  // 其余的媒体最多列几行（地方不够高时少列几行），不超过 kMaxOthers。
  void SetMaxOthers(int max_others);
  // 不算其余的媒体时的高度。
  int MainHeight() const;
  // 列 others 行其余的媒体时的高度。
  int HeightForOthers(int others) const;
  // 频谱每秒画了多少次（调试步骤 mediainfo 用）。
  double WavePaintRate();

  // MediaHub::Observer:
  void OnMediaChanged() override;

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;

 private:
  class Row;

  void Refresh();
  // 完整的样子上面的标题行、封面和文字。
  void PaintHeader(gfx::Canvas* canvas, const MediaEntry& selected);
  const MediaEntry* Selected() const;
  void UpdateTicker();
  void Seek(double fraction);
  void Perform(media_session::mojom::MediaSessionAction action);

  const raw_ptr<MediaHub> hub_;
  Options options_;
  base::ScopedObservation<MediaHub, MediaHub::Observer> observation_{this};
  Theme window_theme_ = Theme::DarkBase();
  Theme media_theme_ = Theme::DarkBase();
  std::vector<MediaEntry> entries_;
  std::optional<base::UnguessableToken> selected_;
  int last_height_ = 0;
  int max_others_ = kMaxOthers;

  raw_ptr<MediaButton> placement_ = nullptr;
  raw_ptr<MediaButton> collapse_ = nullptr;
  raw_ptr<MediaWaveform> wave_ = nullptr;
  raw_ptr<MediaSeekBar> seek_ = nullptr;
  raw_ptr<MediaButton> mute_ = nullptr;
  raw_ptr<MediaButton> previous_ = nullptr;
  raw_ptr<MediaButton> play_ = nullptr;
  raw_ptr<MediaButton> next_ = nullptr;
  raw_ptr<MediaButton> pip_ = nullptr;
  raw_ptr<MediaButton> show_tab_ = nullptr;
  std::vector<raw_ptr<Row>> rows_;
  // 在放时每 0.25 秒重画进度和时间。
  base::RepeatingTimer ticker_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_PLAYER_VIEW_H_
