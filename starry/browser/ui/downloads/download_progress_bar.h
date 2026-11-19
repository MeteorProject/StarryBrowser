// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DOWNLOADS_DOWNLOAD_PROGRESS_BAR_H_
#define STARRY_BROWSER_UI_DOWNLOADS_DOWNLOAD_PROGRESS_BAR_H_

#include <cstdint>
#include <vector>

#include "base/functional/callback.h"
#include "base/timer/timer.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/rect_f.h"

namespace gfx {
class Canvas;
}

namespace starry {

struct Theme;

// 下载进度条的画法和动画（下载页是网页，画法一致，见 pages/downloads/downloads.js 的
// renderProgress_）：
//
// - 轨道是 onSurface 的 8%（深色 10%）；下载中用强调色，暂停、排队用次要文字色，失败用危险色。
// - Starry 分段下载时按段画出每一段下到了哪里，段的起点是一道 1 pt 的细缝；切出来的新段从零
//   长出来。
// - 进度变化用 motion::Progress() 过渡；下载中有一道光每 1.6 秒扫过已下载的部分；不知道总大小
//   时是来回移动的一小段（2 秒一个来回）。减弱动态效果时都不动。
//
// 不是视图：画在宿主视图的 OnPaint 里，要重画时调用 `repaint`。
class DownloadProgressBar {
 public:
  explicit DownloadProgressBar(base::RepeatingClosure repaint);
  DownloadProgressBar(const DownloadProgressBar&) = delete;
  DownloadProgressBar& operator=(const DownloadProgressBar&) = delete;
  ~DownloadProgressBar();

  // 第一次（`animate` 为 false）直接到位，之后的变化有过渡。
  void Update(const Download& download, bool animate = true);
  // 光和来回的一小段要一直重画：宿主不显示时停掉。
  void SetActive(bool active);
  // `gap` 是段与段之间细缝的颜色（宿主的底色）。
  void Paint(gfx::Canvas* canvas,
             const gfx::RectF& bounds,
             const Theme& theme,
             SkColor gap) const;

  // 测试用：画出来有几段、显示到的比例。
  size_t PieceCountForTesting() const { return to_.size(); }
  double ShownFractionForTesting() const;

 private:
  // 一段，都是相对总长的比例：[start, end) 里已经下到 filled。
  struct Piece {
    int64_t key = 0;
    double start = 0;
    double end = 1;
    double filled = 0;
  };

  std::vector<Piece> Shown() const;
  void UpdateTimer();
  void OnFrame();

  base::RepeatingClosure repaint_;
  DownloadState state_ = DownloadState::kRunning;
  bool indeterminate_ = false;
  bool segmented_ = false;
  bool active_ = true;
  // 过渡：从 from_ 到 to_，blend_ 是 0…1。
  std::vector<Piece> from_;
  std::vector<Piece> to_;
  ValueMotion blend_;
  base::RepeatingTimer frame_timer_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DOWNLOADS_DOWNLOAD_PROGRESS_BAR_H_
