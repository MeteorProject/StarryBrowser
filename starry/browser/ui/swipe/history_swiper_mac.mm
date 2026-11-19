// Copyright 2026 The Starry Browser Authors

#import "starry/browser/ui/swipe/history_swiper_mac.h"

#import <AppKit/AppKit.h>

#include <cmath>
#include <memory>

#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/ui/swipe/swipe_navigation.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "ui/events/blink/did_overscroll_params.h"

namespace {

using starry::SwipeNavigation;

// 横向走了多远以后不再把滚动事件给网页（同 Chrome）。
constexpr CGFloat kConsumeEventThreshold = 0.01;
// 上膛以前上下移动多了就不是翻页，同 Chrome 的三条：明显更竖、竖的比横的多、竖的很多。
constexpr CGFloat kCancelEventVerticalThreshold = 0.24;
constexpr CGFloat kCancelEventVerticalLowerThreshold = 0.01;
// 松手前手指停了这么久（秒）就不算甩。
constexpr NSTimeInterval kFlickIdle = 0.06;
// 速度：两次事件隔得比这久（秒）时不和之前的平均。
constexpr NSTimeInterval kVelocityGap = 0.05;
// 妙控鼠标：系统的 gestureAmount 到这么多算够远（同 Chrome）。
constexpr CGFloat kMagicMouseThreshold = 0.3;

}  // namespace

@implementation StarryHistorySwiper {
  // 这个识别器管的网页（同 ChromeRenderWidgetHostViewMacDelegate）。
  int32_t _widgetProcessId;
  int32_t _widgetRoutingId;

  // 以下同 Chrome 的 HistorySwiper：触控板的手指（归一化的坐标，左下角是 (0, 0)）、
  // 网页有没有消费第一下滚动、识别到了哪一步。
  BOOL _receivingTouchEvents;
  NSPoint _touchStartPoint;
  NSPoint _touchCurrentPoint;
  CGFloat _gestureTotalY;
  history_swiper::NavigationDirection _historySwipeDirection;
  BOOL _historySwipeDirectionInverted;
  BOOL _firstScrollUnconsumed;
  BOOL _overscrollTriggeredByRenderer;
  BOOL _waitingForFirstGestureScroll;
  history_swiper::RecognitionState _recognitionState;
  NSSize _mouseScrollDelta;

  // 这一次上过膛：之后上下移动是在选翻几页，不再取消。
  BOOL _everArmed;
  // 朝翻页方向的速度（每秒几个触控板宽）和上一次手指动的位置、时间。
  CGFloat _velocity;
  CGFloat _lastDx;
  NSTimeInterval _lastMoveTime;

  std::unique_ptr<SwipeNavigation> _navigation;
  std::unique_ptr<SwipeNavigation> _mouseNavigation;
}

- (instancetype)initWithDelegate:(id<HistorySwiperDelegate>)delegate
                renderWidgetHost:(content::RenderWidgetHost*)renderWidgetHost {
  self = [super initWithDelegate:delegate];
  if (self) {
    _widgetProcessId = renderWidgetHost->GetProcess()->GetDeprecatedID();
    _widgetRoutingId = renderWidgetHost->GetRoutingID();
  }
  return self;
}

- (content::WebContents*)starryWebContents {
  content::RenderWidgetHost* renderWidgetHost =
      content::RenderWidgetHost::FromID(_widgetProcessId, _widgetRoutingId);
  content::RenderViewHost* renderViewHost =
      renderWidgetHost ? content::RenderViewHost::From(renderWidgetHost)
                       : nullptr;
  return renderViewHost
             ? content::WebContents::FromRenderViewHost(renderViewHost)
             : nullptr;
}

// 触控板、网页的回报：Chrome 的那一套也照常记着（不是 Starry 的标签时滚动事件交给它）。
// 它只在自己的 handleEvent: 里开始翻页，滚动事件不给它时一直停在「等待」，不显示什么。
// 自己的方法名都带 starry 前缀：和父类私有的方法重名会把它们覆盖掉。

- (BOOL)handleEvent:(NSEvent*)event {
  if (event.type != NSEventTypeScrollWheel) {
    return NO;
  }
  if (!SwipeNavigation::CanHandle([self starryWebContents])) {
    return [super handleEvent:event];
  }
  return [self starryHandleScrollWheel:event];
}

- (void)rendererHandledGestureScrollEvent:(const blink::WebGestureEvent&)event
                                 consumed:(BOOL)consumed {
  [super rendererHandledGestureScrollEvent:event consumed:consumed];
  switch (event.GetType()) {
    case blink::WebInputEvent::Type::kGestureScrollBegin:
      if (event.data.scroll_begin.synthetic ||
          event.data.scroll_begin.inertial_phase ==
              blink::WebGestureEvent::InertialPhaseState::kMomentum) {
        return;
      }
      _waitingForFirstGestureScroll = YES;
      break;
    case blink::WebInputEvent::Type::kGestureScrollUpdate:
      if (_waitingForFirstGestureScroll) {
        _firstScrollUnconsumed = !consumed;
      }
      _waitingForFirstGestureScroll = NO;
      break;
    default:
      break;
  }
}

- (void)onOverscrolled:(const ui::DidOverscrollParams&)params {
  [super onOverscrolled:params];
  _overscrollTriggeredByRenderer =
      params.overscroll_behavior.PropagatesXScroll();
}

- (void)touchesBeganWithEvent:(NSEvent*)event {
  [super touchesBeganWithEvent:event];
  _receivingTouchEvents = YES;
  _touchStartPoint = [self starryAveragePosition:event];
  _touchCurrentPoint = _touchStartPoint;
  _gestureTotalY = 0;
  _firstScrollUnconsumed = NO;
  _overscrollTriggeredByRenderer = NO;
  _waitingForFirstGestureScroll = NO;
  _everArmed = NO;
  _velocity = 0;
  _lastDx = 0;
  _lastMoveTime = event.timestamp;
}

- (void)touchesMovedWithEvent:(NSEvent*)event {
  [super touchesMovedWithEvent:event];
  [self starryProcessTouchEvent:event moved:YES];
}

- (void)touchesCancelledWithEvent:(NSEvent*)event {
  [super touchesCancelledWithEvent:event];
  _receivingTouchEvents = NO;
  if (![self starryProcessTouchEvent:event moved:NO]) {
    return;
  }
  [self starryCancelSwipe];
}

- (void)touchesEndedWithEvent:(NSEvent*)event {
  [super touchesEndedWithEvent:event];
  _receivingTouchEvents = NO;
  if (![self starryProcessTouchEvent:event moved:NO]) {
    return;
  }
  if (_navigation) {
    // 松手前停了一下：不是甩。
    const CGFloat velocity =
        event.timestamp - _lastMoveTime > kFlickIdle ? 0 : _velocity;
    _navigation->Release(velocity);
    _navigation.reset();
    _recognitionState = history_swiper::kCompleted;
  }
}

// ---- 触控板 ----

- (NSPoint)starryAveragePosition:(NSEvent*)event {
  NSPoint position = NSMakePoint(0, 0);
  int count = 0;
  for (NSTouch* touch in [event touchesMatchingPhase:NSTouchPhaseAny
                                              inView:nil]) {
    position.x += touch.normalizedPosition.x;
    position.y += touch.normalizedPosition.y;
    ++count;
  }
  if (count > 1) {
    position.x /= count;
    position.y /= count;
  }
  return position;
}

// 朝翻页的方向走了多远（触控板宽度的几分之几）。
- (CGFloat)starryDirectionalDx {
  CGFloat dx = _touchCurrentPoint.x - _touchStartPoint.x;
  if (_historySwipeDirection == history_swiper::kBackwards) {
    dx = -dx;
  }
  if (_historySwipeDirectionInverted) {
    dx = -dx;
  }
  return dx;
}

// 手指往下走了多远（触控板高度的几分之几；归一化的 y 朝上）。
- (CGFloat)starryDownwardDy {
  return _touchStartPoint.y - _touchCurrentPoint.y;
}

// 返回 NO 时不再往下处理（同 Chrome 的 processTouchEventForHistorySwiping:）。
- (BOOL)starryProcessTouchEvent:(NSEvent*)event moved:(BOOL)moved {
  switch (_recognitionState) {
    case history_swiper::kCancelled:
    case history_swiper::kCompleted:
      return NO;
    case history_swiper::kPending:
    case history_swiper::kPotential:
    case history_swiper::kTracking:
      break;
  }

  const NSPoint position = [self starryAveragePosition:event];
  _gestureTotalY += std::abs(position.y - _touchCurrentPoint.y);
  _touchCurrentPoint = position;

  if (!_everArmed && [self starryShouldCancelHorizontalSwipe]) {
    [self starryCancelSwipe];
    return NO;
  }
  if (_recognitionState == history_swiper::kPending) {
    return NO;
  }

  const CGFloat dx = [self starryDirectionalDx];
  if (_recognitionState == history_swiper::kPotential &&
      std::abs(_touchCurrentPoint.x - _touchStartPoint.x) >
          kConsumeEventThreshold) {
    _recognitionState = history_swiper::kTracking;
    if (_historySwipeDirection == history_swiper::kBackwards) {
      [self.delegate backwardsSwipeNavigationLikely];
    }
  }
  if (moved) {
    const NSTimeInterval dt = event.timestamp - _lastMoveTime;
    if (dt > 0.001) {
      const CGFloat instant = (dx - _lastDx) / dt;
      _velocity = dt > kVelocityGap ? instant : 0.6 * instant + 0.4 * _velocity;
      _lastDx = dx;
      _lastMoveTime = event.timestamp;
    }
  }
  if (_navigation) {
    _navigation->Update(dx, [self starryDownwardDy]);
    if (_navigation->armed()) {
      _everArmed = YES;
    }
  }
  return YES;
}

// 上膛以前：同 Chrome，用户其实是想上下滚动。
- (BOOL)starryShouldCancelHorizontalSwipe {
  const CGFloat yDelta = _gestureTotalY;
  const CGFloat xDelta = std::abs(_touchCurrentPoint.x - _touchStartPoint.x);
  if (yDelta > 2 * xDelta) {
    return YES;
  }
  if (yDelta * 1.3 > xDelta && yDelta > kCancelEventVerticalLowerThreshold) {
    return YES;
  }
  return yDelta > kCancelEventVerticalThreshold;
}

- (void)starryCancelSwipe {
  if (_navigation) {
    _navigation->Cancel();
    _navigation.reset();
  }
  _recognitionState = history_swiper::kCancelled;
}

// ---- 滚动事件（同 Chrome 的 handleScrollWheelEvent:，开始翻页时建 SwipeNavigation） ----

- (BOOL)starryHandleScrollWheel:(NSEvent*)event {
  if (event.phase == NSEventPhaseBegan) {
    _recognitionState = history_swiper::kPending;
    _mouseScrollDelta = NSZeroSize;
    _everArmed = NO;
    // 上一次没收尾（没收到手指离开）。
    _navigation.reset();
  }

  if (event.phase != NSEventPhaseChanged &&
      event.momentumPhase != NSEventPhaseChanged) {
    return NO;
  }
  if (_recognitionState != history_swiper::kPending) {
    return [self starryShouldConsumeWheelEvent:event];
  }
  // 惯性滚动不开始翻页。
  if (event.momentumPhase != NSEventPhaseNone) {
    return NO;
  }
  if (!NSEvent.swipeTrackingFromScrollEventsEnabled) {
    return NO;
  }
  if (![self.delegate shouldAllowHistorySwiping]) {
    return NO;
  }
  // 网页消费了第一下、没有横向滚到头、overscroll-behavior 不让：是网页自己的滚动。
  if (!_firstScrollUnconsumed || !_overscrollTriggeredByRenderer) {
    return NO;
  }
  // 有手势阶段但没有手指：妙控鼠标。
  if (!_receivingTouchEvents) {
    return [self starryHandleMagicMouseWheel:event];
  }

  const CGFloat xDelta = _touchCurrentPoint.x - _touchStartPoint.x;
  if (std::abs(xDelta) < 0.001) {
    return NO;
  }
  BOOL isRightScroll = xDelta > 0;
  if (event.directionInvertedFromDevice) {
    isRightScroll = !isRightScroll;
  }
  const history_swiper::NavigationDirection direction =
      isRightScroll ? history_swiper::kForwards : history_swiper::kBackwards;
  if (![self.delegate canNavigateInDirection:direction onWindow:event.window]) {
    return NO;
  }
  _navigation = SwipeNavigation::Start(
      [self starryWebContents], direction == history_swiper::kForwards
                                    ? SwipeNavigation::Direction::kForward
                                    : SwipeNavigation::Direction::kBack);
  if (!_navigation) {
    return NO;
  }
  _historySwipeDirection = direction;
  _historySwipeDirectionInverted = event.directionInvertedFromDevice;
  _recognitionState = history_swiper::kPotential;
  _velocity = 0;
  _lastDx = [self starryDirectionalDx];
  _lastMoveTime = event.timestamp;
  _navigation->Update(_lastDx, [self starryDownwardDy]);
  return [self starryShouldConsumeWheelEvent:event];
}

- (BOOL)starryShouldConsumeWheelEvent:(NSEvent*)event {
  switch (_recognitionState) {
    case history_swiper::kPending:
    case history_swiper::kCancelled:
      return NO;
    case history_swiper::kTracking:
    case history_swiper::kCompleted:
      return YES;
    case history_swiper::kPotential:
      // 还说不准：有上下的成分就照样给网页。
      return event.scrollingDeltaY == 0;
  }
}

// ---- 妙控鼠标（没有手指的触摸事件，只能用系统的 trackSwipeEvent） ----

- (BOOL)starryHandleMagicMouseWheel:(NSEvent*)event {
  if (event.phase == NSEventPhaseNone) {
    return NO;
  }
  _mouseScrollDelta.width += event.scrollingDeltaX;
  _mouseScrollDelta.height += event.scrollingDeltaY;
  if (std::abs(_mouseScrollDelta.width) <= std::abs(_mouseScrollDelta.height)) {
    return NO;
  }
  const BOOL isRightScroll = event.scrollingDeltaX < 0;
  const history_swiper::NavigationDirection direction =
      isRightScroll ? history_swiper::kForwards : history_swiper::kBackwards;
  if (![self.delegate canNavigateInDirection:direction onWindow:event.window]) {
    return NO;
  }
  const SwipeNavigation::Direction swipe =
      isRightScroll ? SwipeNavigation::Direction::kForward
                    : SwipeNavigation::Direction::kBack;
  __weak StarryHistorySwiper* weakSelf = self;
  // 锁定方向：先往一边再快速甩回来时不会变成反方向（同 Chrome）。gestureAmount 已经按
  // 系统的滚动方向设置转好了。
  [event trackSwipeEventWithOptions:NSEventSwipeTrackingLockDirection
           dampenAmountThresholdMin:-1
                                max:1
                       usingHandler:^(CGFloat gestureAmount, NSEventPhase phase,
                                      BOOL isComplete, BOOL* stop) {
                         StarryHistorySwiper* strongSelf = weakSelf;
                         if (!strongSelf) {
                           *stop = YES;
                           return;
                         }
                         [strongSelf starryTrackMagicMouse:swipe
                                                    amount:gestureAmount
                                                     phase:phase
                                                  complete:isComplete];
                       }];
  return YES;
}

- (void)starryTrackMagicMouse:(SwipeNavigation::Direction)direction
                       amount:(CGFloat)amount
                        phase:(NSEventPhase)phase
                     complete:(BOOL)complete {
  if (phase == NSEventPhaseBegan) {
    _mouseNavigation =
        SwipeNavigation::Start([self starryWebContents], direction);
    return;
  }
  if (!_mouseNavigation) {
    return;
  }
  // 系统松手以后还会把 gestureAmount 动到终点，这时已经交给指示器自己的动画了。
  _mouseNavigation->Update(std::abs(amount) / kMagicMouseThreshold *
                               SwipeNavigation::kCommitDistance,
                           0);
  if (phase == NSEventPhaseEnded) {
    _mouseNavigation->Commit();
    _mouseNavigation.reset();
  } else if (phase == NSEventPhaseCancelled || complete) {
    _mouseNavigation.reset();
  }
}

@end
