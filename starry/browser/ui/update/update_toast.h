// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_UPDATE_UPDATE_TOAST_H_
#define STARRY_BROWSER_UI_UPDATE_UPDATE_TOAST_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/updater/updater_service.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

class BrowserWindowInterface;

namespace starry {

class UpdateToast;

// 更新就绪或发现新版本时，在内容卡片左下角显示的液态玻璃提示条：
// - 自动下载就绪后展示「新版本 X.Y.Z 已就绪 · 重启并更新 · 稍后」
//   （Linux 上为「安装更新」）；
// - 仅通知模式下展示「发现新版本 X.Y.Z · 查看更新 · 稍后」；
// - 点击「稍后」收起提示条（设置按钮上的小圆点仍保留）；
// - 媒体岛将其计入避让区域（StarryBrowserView::MediaObstacles），不会重叠。
class UpdateToastHost : public views::View,
                        public views::ViewTargeterDelegate,
                        public updater::UpdaterService::Observer {
  METADATA_HEADER(UpdateToastHost, views::View)

 public:
  explicit UpdateToastHost(BrowserWindowInterface* browser);
  UpdateToastHost(const UpdateToastHost&) = delete;
  UpdateToastHost& operator=(const UpdateToastHost&) = delete;
  ~UpdateToastHost() override;

  void SetTheme(const Theme& theme);
  const Theme& theme() const { return theme_; }
  BrowserWindowInterface* browser() const { return browser_; }
  gfx::Rect ToastRect() const;

  void PrimaryAction();
  void DismissAction();

  // views::View:
  void AddedToWidget() override;
  void RemovedFromWidget() override;
  void Layout(PassKey) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // updater::UpdaterService::Observer:
  void OnUpdateStateChanged() override;

 private:
  void Update();
  void Show(const updater::UpdaterService::StateSnapshot& snapshot);
  void Hide();
  void PlaceToast();
  void NotifyObstacles();

  const raw_ptr<BrowserWindowInterface> browser_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<UpdateToast> toast_ = nullptr;
  base::ScopedObservation<updater::UpdaterService,
                          updater::UpdaterService::Observer>
      observation_{this};
  base::WeakPtrFactory<UpdateToastHost> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_UPDATE_UPDATE_TOAST_H_
