// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DOWNLOADS_DOWNLOAD_LAUNCH_H_
#define STARRY_BROWSER_UI_DOWNLOADS_DOWNLOAD_LAUNCH_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/point.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

class BrowserWindowInterface;

namespace starry {

class Download;
class DownloadToast;
class DownloadFlight;

// 下载开始的提示，盖在整个窗口上：
// - 下载按钮在屏幕上时，文件图标在鼠标处（多半是刚点的链接）弹出，沿弧线边缩小边飞进按钮；
//   按钮接住时箭头跳一下，外面扩散一圈强调色。
// - 侧栏自动隐藏、收着时，左下角出现一条提示：文件图标（下载中外面一圈进度）、「开始下载」
//   和文件名。点它滑出侧栏、打开下载列表；鼠标停在上面时不收起，3.5 秒后自己收起。
// - 减弱动态效果时不飞，按钮那圈只淡出不扩散。
// 只管这个窗口里开始的下载（网页在这个窗口的标签里）。交给 Starry 自己下载的 GUID 不变，
// 提示条接着显示它的进度；交回 Chrome 重试的不再播。除了提示条，这一层不接收鼠标。
class DownloadLaunchView : public views::View,
                           public views::ViewTargeterDelegate,
                           public DownloadService::Observer {
  METADATA_HEADER(DownloadLaunchView, views::View)

 public:
  explicit DownloadLaunchView(BrowserWindowInterface* browser);
  DownloadLaunchView(const DownloadLaunchView&) = delete;
  DownloadLaunchView& operator=(const DownloadLaunchView&) = delete;
  ~DownloadLaunchView() override;

  void SetTheme(const Theme& theme);
  BrowserWindowInterface* browser() { return browser_; }
  const Theme& theme() const { return theme_; }

  // 下载按钮的中心（本视图的坐标）；按钮没显示时是空的。飞行中每一帧都问一次（侧栏会动）。
  std::optional<gfx::PointF> ButtonCenter() const;
  // 图标飞到了：按钮跳一下、扩散一圈。
  void Land(DownloadFlight* flight);
  void HoldToast(bool hold);
  void DismissToast();

  // 调试用：正在飞的图标数、提示条的文字。
  size_t flights_for_testing() const { return flights_; }
  std::u16string ToastTextForTesting() const;
  // 调试用：当作这个下载刚开始（不用真的下载）。
  void LaunchForTesting(const std::u16string& filename, bool toast);

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // DownloadService::Observer:
  void OnDownloadAdded(Download* download) override;
  void OnDownloadUpdated(Download* download) override;
  void OnDownloadServiceShutdown() override;

 private:
  bool BelongsHere(const Download& download) const;
  void Launch(const std::string& guid,
              const std::u16string& filename,
              const base::FilePath& icon_path);
  void ShowToast(const std::string& guid,
                 const std::u16string& filename,
                 const base::FilePath& icon_path);
  void Pulse(const gfx::PointF& center);

  const raw_ptr<BrowserWindowInterface> browser_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<DownloadService> service_ = nullptr;
  base::ScopedObservation<DownloadService, DownloadService::Observer>
      observation_{this};
  raw_ptr<DownloadToast> toast_ = nullptr;
  base::OneShotTimer toast_timer_;
  bool toast_held_ = false;
  size_t flights_ = 0;

  base::WeakPtrFactory<DownloadLaunchView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DOWNLOADS_DOWNLOAD_LAUNCH_H_
