// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/task_manager/task_manager_window.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/themes/theme_service_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/common/pref_names.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/starry_hooks.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/task_manager/task_manager_view.h"
#include "ui/base/base_window.h"
#include "ui/base/hit_test.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/color/color_provider_key.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/window/client_view.h"

namespace starry {

namespace {

constexpr gfx::Size kDefaultSize(760, 600);

// 允许在标题行空白处拖动窗口。
class TaskManagerClientView : public views::ClientView {
  METADATA_HEADER(TaskManagerClientView, views::ClientView)

 public:
  TaskManagerClientView(views::Widget* widget, views::View* contents)
      : views::ClientView(widget, contents) {}

  int NonClientHitTest(const gfx::Point& point) override {
    if (views::View* contents = contents_view()) {
      gfx::Point local(point);
      views::View::ConvertPointToTarget(parent(), contents, &local);
      if (IsWindowDraggableAt(contents, local)) {
        return HTCAPTION;
      }
    }
    return views::ClientView::NonClientHitTest(point);
  }
};

BEGIN_METADATA(TaskManagerClientView)
END_METADATA

// 窗口：只开一个，自己拥有 Widget；深浅跟着最近用的用户资料的「外观」设置（同浏览器窗口）。
class TaskManagerWindow : public views::WidgetDelegate,
                          public ThemeServiceObserver,
                          public ProfileObserver {
 public:
  TaskManagerWindow() {
    SetTitle(L("taskManager.title"));
    SetCanResize(true);
    SetCanMaximize(true);
    SetCanMinimize(true);
    SetClientViewFactory(base::BindOnce(
        [](views::Widget* widget,
           views::View* contents) -> std::unique_ptr<views::ClientView> {
          return std::make_unique<TaskManagerClientView>(widget, contents);
        }));
    view_ = SetContentsView(std::make_unique<TaskManagerView>());
    SetInitiallyFocusedView(view_->focus_target());
  }
  TaskManagerWindow(const TaskManagerWindow&) = delete;
  TaskManagerWindow& operator=(const TaskManagerWindow&) = delete;

  ~TaskManagerWindow() override {
    theme_observation_.Reset();
    profile_observation_.Reset();
    view_ = nullptr;
    // 关掉之前记下位置和大小：MakeCloseSynchronous 之后 Widget 自己不记。
    if (widget_ && widget_->IsVisible()) {
      SaveWindowPlacement(widget_->GetRestoredBounds(),
                          widget_->IsMaximized()
                              ? ui::mojom::WindowShowState::kMaximized
                              : ui::mojom::WindowShowState::kNormal);
    }
    widget_.reset();
  }

  void Init() {
    widget_ = std::make_unique<views::Widget>();
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_WINDOW);
    params.delegate = this;
    params.name = "StarryTaskManager";
    params.bounds = InitialBounds();
#if BUILDFLAG(IS_MAC)
    // Widget 拥有它；它的 delegate 是 widget_，活不过 widget_。
    params.native_widget = CreateTaskManagerNativeWidget(widget_.get());
#endif
    widget_->Init(std::move(params));
#if BUILDFLAG(IS_MAC)
    // 标题起点与红绿灯右侧的间距。
    constexpr int kTitleGap = 14;
    const TitlebarMetrics metrics = GetTitlebarMetrics(widget_.get());
    view_->SetTitlebar(metrics.height, metrics.buttons_end + kTitleGap);
#endif
    // 关闭按钮、⌘W、Widget::Close() 都走这里，拆窗口是同步的。
    widget_->MakeCloseSynchronous(base::BindOnce(
        [](views::Widget::ClosedReason reason) { HideTaskManagerWindow(); }));
    ObserveProfile(ProfileManager::GetLastUsedProfileIfLoaded());
    widget_->Show();
  }

  void Activate() {
    if (widget_) {
      widget_->Show();
      widget_->Activate();
    }
  }

  views::Widget* widget() { return widget_.get(); }
  TaskManagerView* view() { return view_; }

  // views::WidgetDelegate:
  std::string GetWindowName() const override {
    return ::prefs::kTaskManagerWindowPlacement;
  }

  // ThemeServiceObserver:
  void OnThemeChanged() override { ApplyColorMode(); }

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override {
    theme_observation_.Reset();
    profile_observation_.Reset();
    theme_service_ = nullptr;
    ApplyColorMode();
  }

 private:
  // 第一次打开：在最近用的浏览器窗口所在的屏幕中间（之后用记下的位置和大小）。
  static gfx::Rect InitialBounds() {
    display::Screen* screen = display::Screen::Get();
    display::Display display = screen->GetPrimaryDisplay();
    if (BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
      display = screen->GetDisplayNearestWindow(
          browser->GetWindow()->GetNativeWindow());
    }
    const gfx::Rect area = display.work_area();
    gfx::Rect bounds(kDefaultSize);
    bounds.set_x(area.x() + std::max(0, (area.width() - bounds.width()) / 2));
    bounds.set_y(area.y() + std::max(0, (area.height() - bounds.height()) / 2));
    return bounds;
  }

  void ObserveProfile(Profile* profile) {
    theme_observation_.Reset();
    profile_observation_.Reset();
    theme_service_ = nullptr;
    if (profile) {
      profile = profile->GetOriginalProfile();
      theme_service_ = ThemeServiceFactory::GetForProfile(profile);
      profile_observation_.Observe(profile);
      if (theme_service_) {
        theme_observation_.Observe(theme_service_);
      }
    }
    ApplyColorMode();
  }

  // 「外观」选了浅色、深色时固定那一种，跟随系统时不覆盖。
  void ApplyColorMode() {
    if (!widget_) {
      return;
    }
    std::optional<ui::ColorProviderKey::ColorMode> mode;
    if (theme_service_) {
      switch (theme_service_->GetBrowserColorScheme()) {
        case ThemeService::BrowserColorScheme::kLight:
          mode = ui::ColorProviderKey::ColorMode::kLight;
          break;
        case ThemeService::BrowserColorScheme::kDark:
          mode = ui::ColorProviderKey::ColorMode::kDark;
          break;
        case ThemeService::BrowserColorScheme::kSystem:
          break;
      }
    }
    widget_->SetColorModeOverride(mode);
  }

  std::unique_ptr<views::Widget> widget_;
  raw_ptr<TaskManagerView> view_ = nullptr;
  raw_ptr<ThemeService> theme_service_ = nullptr;
  base::ScopedObservation<ThemeService, ThemeServiceObserver>
      theme_observation_{this};
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
};

std::unique_ptr<TaskManagerWindow>& Instance() {
  static base::NoDestructor<std::unique_ptr<TaskManagerWindow>> instance;
  return *instance;
}

}  // namespace

void ShowTaskManagerWindow() {
  std::unique_ptr<TaskManagerWindow>& window = Instance();
  if (window) {
    window->Activate();
    return;
  }
  window = std::make_unique<TaskManagerWindow>();
  window->Init();
}

void HideTaskManagerWindow() {
  // 先从全局拿走再拆：拆的时候有人再问「开着吗」应该是没有。
  std::unique_ptr<TaskManagerWindow> window = std::move(Instance());
  window.reset();
}

void CloseTaskManagerWindowNow() {
  HideTaskManagerWindow();
}

TaskManagerView* GetTaskManagerViewForTesting() {
  return Instance() ? Instance()->view() : nullptr;
}

views::Widget* GetTaskManagerWidgetForTesting() {
  return Instance() ? Instance()->widget() : nullptr;
}

bool ShowTaskManager(BrowserWindowInterface* browser) {
  ShowTaskManagerWindow();
  return true;
}

void HideTaskManager() {
  HideTaskManagerWindow();
}

}  // namespace starry
