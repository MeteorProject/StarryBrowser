// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/mini_window/mini_window_activation.h"

#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "base/apple/bridging.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "components/remote_cocoa/app_shim/browser_native_widget_window_mac.h"
#include "components/remote_cocoa/common/native_widget_ns_window.mojom.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "ui/base/base_window.h"
#include "ui/base/cocoa/window_size_constants.h"
#include "ui/gfx/mac/scoped_cocoa_disable_screen_updates.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

// 维护 macOS 小窗口的层级与焦点移交，避免外部链接激活应用时主窗口闪到前面。
namespace starry::mini_window {

namespace {

// 一个普通窗口在屏幕上的位置：它前面（离它最近的在前）是哪些窗口。
struct Position {
  __weak NSWindow* window = nil;
  std::vector<NSInteger> above;
};

struct VisibleWindow {
  NSInteger number = 0;
  pid_t owner = 0;
};

// LaunchServices 的激活通知先于 URL Apple 事件；短暂合并这两个事件的屏幕更新，
// 避免先显示主窗口再还原。普通激活最多等 100ms，明确的用户操作立即结束。
constexpr base::TimeDelta kActivationGrace = base::Milliseconds(100);
uint64_t g_activation_generation = 0;
std::unique_ptr<gfx::ScopedCocoaDisableScreenUpdates>& ScreenUpdates() {
  static base::NoDestructor<
      std::unique_ptr<gfx::ScopedCocoaDisableScreenUpdates>>
      updates;
  return *updates;
}

struct ActivationState {
  std::vector<Position> positions;
  NSRunningApplication* last_external_app = nil;
  NSRunningApplication* return_to_app = nil;
};

ActivationState& Activation() {
  static base::NoDestructor<ActivationState> state;
  return *state;
}

// 正在显示一个小窗口：激活完成（或者小窗口出来）时把记下的层级还原。
bool g_presenting = false;
bool g_tracking = false;

bool IsNormalBrowserWindow(NSWindow* window) {
  BrowserView* view = window ? BrowserView::GetBrowserViewForNativeWindow(
                                   gfx::NativeWindow(window))
                             : nullptr;
  return view && view->GetIsNormalType();
}

// 屏幕上的普通层级的窗口，从前往后（所有 App 的）。只读编号、层级和进程号。
std::vector<VisibleWindow> VisibleWindows(CGWindowID below = kCGNullWindowID) {
  std::vector<VisibleWindow> result;
  const auto scope = below == kCGNullWindowID
                         ? kCGWindowListOptionOnScreenOnly
                         : kCGWindowListOptionOnScreenBelowWindow;
  base::apple::ScopedCFTypeRef<CFArrayRef> list(CGWindowListCopyWindowInfo(
      scope | kCGWindowListExcludeDesktopElements, below));
  NSArray* windows = base::apple::CFToNSPtrCast(list.get());
  for (NSDictionary* item in windows) {
    NSNumber* layer = item[base::apple::CFToNSPtrCast(kCGWindowLayer)];
    NSNumber* number = item[base::apple::CFToNSPtrCast(kCGWindowNumber)];
    NSNumber* owner = item[base::apple::CFToNSPtrCast(kCGWindowOwnerPID)];
    if (!layer || !number || !owner ||
        layer.integerValue != NSNormalWindowLevel) {
      continue;
    }
    result.push_back({number.integerValue, owner.intValue});
  }
  return result;
}

// 普通浏览器窗口（不含小窗口、最小化的、浮在上面的）各自前面有哪些窗口。
std::vector<Position> WindowPositions() {
  const std::vector<VisibleWindow> order = VisibleWindows();
  std::map<NSInteger, NSWindow*> browsers;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
        !browser->GetWindow()) {
      continue;
    }
    NSWindow* window =
        browser->GetWindow()->GetNativeWindow().GetNativeNSWindow();
    if (!window || window.level != NSNormalWindowLevel || window.miniaturized) {
      continue;
    }
    browsers[window.windowNumber] = window;
  }
  std::vector<Position> positions;
  for (size_t i = 0; i < order.size(); ++i) {
    auto it = browsers.find(order[i].number);
    if (it == browsers.end()) {
      continue;
    }
    Position position;
    position.window = it->second;
    for (size_t j = i; j > 0; --j) {
      position.above.push_back(order[j - 1].number);
    }
    positions.push_back(std::move(position));
  }
  return positions;
}

// 每个窗口放回它原来前面那个（还在屏幕上的）窗口的下面。
void Restore(const std::vector<Position>& positions) {
  std::set<NSInteger> visible;
  for (const VisibleWindow& window : VisibleWindows()) {
    visible.insert(window.number);
  }
  for (const Position& position : positions) {
    NSWindow* window = position.window;
    if (!window || !window.visible || window.miniaturized) {
      continue;
    }
    for (NSInteger anchor : position.above) {
      if (visible.contains(anchor)) {
        [window orderWindow:NSWindowBelow relativeTo:anchor];
        break;
      }
    }
  }
}

void CaptureWindowOrder() {
  if (g_presenting) {
    return;
  }
  NSRunningApplication* front =
      NSWorkspace.sharedWorkspace.frontmostApplication;
  Activation().return_to_app = front && front.processIdentifier != getpid()
                                   ? front
                                   : Activation().last_external_app;
  Activation().positions = WindowPositions();
  if (Activation().positions.empty()) {
    return;
  }
  if (!ScreenUpdates()) {
    ScreenUpdates() = std::make_unique<gfx::ScopedCocoaDisableScreenUpdates>();
  }
  const uint64_t generation = ++g_activation_generation;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](uint64_t generation) {
            if (generation == g_activation_generation) {
              // URL 可能还在路上。到时放行屏幕更新，保留激活前的层级。
              ScreenUpdates().reset();
            }
          },
          generation),
      kActivationGrace);
}

void EndPresentation() {
  ++g_activation_generation;
  ScreenUpdates().reset();
  Activation().positions.clear();
  g_presenting = false;
}

void RestoreIfPresenting() {
  if (g_presenting) {
    Restore(Activation().positions);
    EndPresentation();
  }
}

}  // namespace

void StartTrackingActivation() {
  if (g_tracking) {
    return;
  }
  g_tracking = true;
  // 浮动面板、只有菜单栏或桌面的 App 可能没有普通层级的窗口。
  // WillBecomeActive 到来时，Workspace 的前台进程可能已经是 Starry。
  NSWorkspace* workspace = NSWorkspace.sharedWorkspace;
  NSRunningApplication* front = workspace.frontmostApplication;
  if (front.processIdentifier != getpid()) {
    Activation().last_external_app = front;
  }
  [workspace.notificationCenter
      addObserverForName:NSWorkspaceDidActivateApplicationNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification* notification) {
                NSRunningApplication* app =
                    notification.userInfo[NSWorkspaceApplicationKey];
                if (app.processIdentifier != getpid()) {
                  Activation().last_external_app = app;
                }
              }];
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  [center addObserverForName:NSApplicationWillBecomeActiveNotification
                      object:nil
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    CaptureWindowOrder();
                  }];
  [center addObserverForName:NSApplicationDidBecomeActiveNotification
                      object:nil
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    RestoreIfPresenting();
                  }];
  [center addObserverForName:NSApplicationDidResignActiveNotification
                      object:nil
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    DiscardWindowOrder();
                  }];
  // 用户在浏览器里操作过了：激活前的层级不再适用。
  constexpr NSEventMask kUserInput =
      NSEventMaskKeyDown | NSEventMaskLeftMouseDown |
      NSEventMaskRightMouseDown | NSEventMaskScrollWheel;
  [NSEvent
      addLocalMonitorForEventsMatchingMask:kUserInput
                                   handler:^NSEvent*(NSEvent* event) {
                                     EndPresentation();
                                     if (Activation().return_to_app &&
                                         IsNormalBrowserWindow(
                                             event.window ?: NSApp.keyWindow)) {
                                       Activation().return_to_app = nil;
                                     }
                                     return event;
                                   }];
}

void ConfigureWindow(views::Widget* widget) {
  StartTrackingActivation();
  if (!NSApp.active) {
    CaptureWindowOrder();
  }
  g_presenting = true;
}

void WindowShown(views::Widget* widget) {
  // 应用已经激活了（链接先激活应用再交过来）：现在还原；还没有时等激活完成。
  if (NSApp.active) {
    RestoreIfPresenting();
  }
}

void WindowClosing(views::Widget* widget, bool moving_to_browser) {
  if (moving_to_browser) {
    DiscardWindowOrder();
    return;
  }
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  if (!window || !window.keyWindow || !NSApp.active) {
    return;
  }
  NSRunningApplication* target = Activation().return_to_app;
  if (target.terminated || target.hidden) {
    target = nil;
  }
  // 在 orderOut: 之前交出应用激活权；否则 AppKit 会先把主窗口选回来。
  for (const VisibleWindow& each : VisibleWindows(window.windowNumber)) {
    if (each.owner == getpid()) {
      NSWindow* own = [NSApp windowWithWindowNumber:each.number];
      if (!own.canBecomeKeyWindow || own.parentWindow == window) {
        continue;
      }
      // 下面还有一个小窗口（例如 window.open 的来源），留在 Starry。
      if (!IsNormalBrowserWindow(own)) {
        return;
      }
    } else if (target && each.owner != target.processIdentifier) {
      // 其他进程的辅助窗口可能夹在两个小窗口之间。
      continue;
    } else if (!target) {
      target = [NSRunningApplication
          runningApplicationWithProcessIdentifier:each.owner];
    }
    break;
  }
  if (!target || target.terminated) {
    return;
  }
  [NSApp deactivate];
  if (@available(macOS 14, *)) {
    [NSApp yieldActivationToApplication:target];
    [target activateFromApplication:NSRunningApplication.currentApplication
                            options:0];
  } else {
    [target activateWithOptions:0];
  }
}

void DiscardWindowOrder() {
  EndPresentation();
  Activation().return_to_app = nil;
}

}  // namespace starry::mini_window

// BrowserView 在 OnWidgetClosing 之前就会 Hide()；必须在原生 orderOut: 之前
// 交出焦点，且只有真正获准关闭（beforeunload 已通过）才会走到这里。
@interface StarryMiniNativeWindow : BrowserNativeWidgetWindow
@end

@implementation StarryMiniNativeWindow
- (void)orderOut:(id)sender {
  views::Widget* widget =
      views::Widget::GetWidgetForNativeWindow(gfx::NativeWindow(self));
  auto* view = views::AsViewClass<starry::StarryBrowserView>(
      BrowserView::GetBrowserViewForNativeWindow(gfx::NativeWindow(self)));
  if (widget && view && view->is_mini_window()) {
    starry::mini_window::WindowClosing(widget, view->moving_to_browser());
  }
  [super orderOut:sender];
}
@end

namespace starry::mini_window {
NativeWidgetMacNSWindow* CreateNativeWindow(
    BrowserView* browser_view,
    const remote_cocoa::mojom::CreateWindowParams* params) {
  auto* view = views::AsViewClass<StarryBrowserView>(browser_view);
  if (!view || !view->is_mini_window() ||
      params->window_class != remote_cocoa::mojom::WindowClass::kBrowser) {
    return nil;
  }
  auto* window = [[StarryMiniNativeWindow alloc]
      initWithContentRect:ui::kWindowSizeDeterminedLater
                styleMask:params->style_mask
                  backing:NSBackingStoreBuffered
                    defer:NO];
  window.releasedWhenClosed = NO;
  window.titlebarAppearsTransparent = params->titlebar_appears_transparent;
  if (params->window_title_hidden) {
    window.titleVisibility = NSWindowTitleHidden;
  }
  if (params->animation_enabled) {
    window.animationBehavior = NSWindowAnimationBehaviorDocumentWindow;
  }
  return window;
}
}  // namespace starry::mini_window
