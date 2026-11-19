// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_TOAST_H_
#define STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_TOAST_H_

#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

class BrowserWindowInterface;

namespace starry {

class ArchiveNotice;
class ArchiveToast;

// 自动整理后，内容卡片左下角的提示条：归档盒、说明、「撤销」和「查看」。
// 「查看」在垂直标签布局下打开侧栏归档页，在顶部标签布局下打开整页。悬停时暂停倒数。
//
// 媒体岛把它当作挡着的东西让开（StarryBrowserView::MediaObstacles）：岛在左下角时往上挪，
// 不会叠在一起。铺满内容卡片，只有提示条接鼠标。
class ArchiveToastHost : public views::View,
                         public views::ViewTargeterDelegate {
  METADATA_HEADER(ArchiveToastHost, views::View)

 public:
  explicit ArchiveToastHost(BrowserWindowInterface* browser);
  ArchiveToastHost(const ArchiveToastHost&) = delete;
  ArchiveToastHost& operator=(const ArchiveToastHost&) = delete;
  ~ArchiveToastHost() override;

  void SetTheme(const Theme& theme);
  const Theme& theme() const { return theme_; }
  BrowserWindowInterface* browser() const { return browser_; }
  ArchiveNotice* Notice() const;
  // 提示条在哪（本视图的坐标）；没显示（或者正在收起）时是空的。
  gfx::Rect ToastRect() const;
  // 「查看」：侧栏的归档页或者整页。
  void ShowArchive();
  // 调试用：提示条上写的字（没显示是空的）。
  std::u16string TextForTesting() const;

  // views::View:
  void AddedToWidget() override;
  void RemovedFromWidget() override;
  void Layout(PassKey) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

 private:
  void Update();
  void Show(ArchiveNotice* notice);
  void Hide();
  void PlaceToast();
  void NotifyObstacles();

  const raw_ptr<BrowserWindowInterface> browser_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<ArchiveToast> toast_ = nullptr;
  base::CallbackListSubscription notice_subscription_;
  base::WeakPtrFactory<ArchiveToastHost> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_TOAST_H_
