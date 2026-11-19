// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/frame/window_buttons.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include <algorithm>
#include <array>

#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget.h"

// 自定义摆放 macOS 红绿灯：在 AppKit 排完标题栏后再调整位置（避免在 frame 通知中直接
// 修改）；使用图层不透明度淡入淡出（修改 alphaValue 会触发重新排版）；全屏时不干预。
@interface StarryWindowButtons : NSObject
- (instancetype)initWithWindow:(NSWindow*)window;
- (void)setOrigin:(NSPoint)origin
        hasOrigin:(BOOL)hasOrigin
         animated:(BOOL)animated;
@end

namespace {

constexpr std::array<NSWindowButton, 3> kTypes = {
    NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton};
constexpr int kCount = 3;
// 隐藏或移动前 0.1 秒淡出，出现时 0.14 秒淡入。
constexpr NSTimeInterval kFadeOut = 0.1;
constexpr NSTimeInterval kFadeIn = 0.14;
NSString* const kFadeKey = @"starry.fade";
char kAssociatedKey;

}  // namespace

@implementation StarryWindowButtons {
  __weak NSWindow* _window;
  BOOL _hasOrigin;
  NSPoint _origin;
  BOOL _fullScreen;
  std::array<NSPoint, kCount> _system;
  std::array<BOOL, kCount> _hasSystem;
  std::array<NSPoint, kCount> _placed;
  std::array<BOOL, kCount> _hasPlaced;
  BOOL _placeQueued;
  int _placeRetries;
  int _generation;
  BOOL _configured;
  NSMutableArray* _observers;
}

- (instancetype)initWithWindow:(NSWindow*)window {
  if ((self = [super init])) {
    _window = window;
    _observers = [NSMutableArray array];
    _fullScreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    __weak StarryWindowButtons* weakSelf = self;
    for (int i = 0; i < kCount; ++i) {
      NSButton* button = [window standardWindowButton:kTypes[i]];
      if (!button) {
        continue;
      }
      _system[i] = button.frame.origin;
      _hasSystem[i] = YES;
      button.postsFrameChangedNotifications = YES;
      [_observers
          addObject:[center addObserverForName:NSViewFrameDidChangeNotification
                                        object:button
                                         queue:NSOperationQueue.mainQueue
                                    usingBlock:^(NSNotification*) {
                                      [weakSelf buttonMoved:i];
                                    }]];
    }
    [_observers
        addObject:[center addObserverForName:NSWindowDidResizeNotification
                                      object:window
                                       queue:NSOperationQueue.mainQueue
                                  usingBlock:^(NSNotification*) {
                                    [weakSelf placeNow];
                                  }]];
    [_observers
        addObject:[center
                      addObserverForName:NSWindowWillEnterFullScreenNotification
                                  object:window
                                   queue:NSOperationQueue.mainQueue
                              usingBlock:^(NSNotification*) {
                                [weakSelf setFullScreen:YES];
                              }]];
    [_observers
        addObject:[center
                      addObserverForName:NSWindowDidExitFullScreenNotification
                                  object:window
                                   queue:NSOperationQueue.mainQueue
                              usingBlock:^(NSNotification*) {
                                [weakSelf setFullScreen:NO];
                              }]];
  }
  return self;
}

- (void)dealloc {
  for (id observer in _observers) {
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
  }
}

- (NSButton*)button:(int)index {
  return [_window standardWindowButton:kTypes[index]];
}

- (void)setFullScreen:(BOOL)fullScreen {
  _fullScreen = fullScreen;
  [self apply:NO];
}

- (void)setOrigin:(NSPoint)origin
        hasOrigin:(BOOL)hasOrigin
         animated:(BOOL)animated {
  if (_configured && hasOrigin == _hasOrigin &&
      (!hasOrigin || NSEqualPoints(origin, _origin))) {
    return;
  }
  _configured = YES;
  _origin = origin;
  _hasOrigin = hasOrigin;
  [self apply:animated];
}

- (void)buttonMoved:(int)index {
  NSButton* button = [self button:index];
  if (!button || (_hasPlaced[index] &&
                  NSEqualPoints(button.frame.origin, _placed[index]))) {
    return;
  }
  _system[index] = button.frame.origin;
  _hasSystem[index] = YES;
  _hasPlaced[index] = NO;
  [self queuePlace];
}

- (NSPoint)target:(int)index {
  NSButton* button = [self button:index];
  if (!_hasSystem[index]) {
    return button.frame.origin;
  }
  NSWindow* window = _window;
  NSView* superview = button.superview;
  if (!_hasOrigin || _fullScreen || !window || !superview || !_hasSystem[0]) {
    return _system[index];
  }
  const NSSize size = button.frame.size;
  NSRect rect = NSMakeRect(_origin.x + _system[index].x - _system[0].x,
                           window.frame.size.height - _origin.y - size.height,
                           size.width, size.height);
  rect = [superview convertRect:rect fromView:nil];
  rect.origin.y = std::min(std::max(rect.origin.y, CGFloat(0)),
                           superview.bounds.size.height - size.height);
  return rect.origin;
}

- (BOOL)needsMove {
  for (int i = 0; i < kCount; ++i) {
    NSButton* button = [self button:i];
    if (button && !NSEqualPoints(button.frame.origin, [self target:i])) {
      return YES;
    }
  }
  return NO;
}

- (void)placeNow {
  if (!_hasOrigin || _fullScreen || !_window) {
    return;
  }
  if ([self needsMove]) {
    [self place];
  }
}

- (void)queuePlace {
  if (!_hasOrigin || _fullScreen || _placeQueued) {
    return;
  }
  _placeQueued = YES;
  __weak StarryWindowButtons* weakSelf = self;
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
    StarryWindowButtons* strongSelf = weakSelf;
    if (!strongSelf) {
      return;
    }
    strongSelf->_placeQueued = NO;
    [strongSelf placeNow];
  });
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

- (void)place {
  NSView* superview = nil;
  for (int i = 0; i < kCount; ++i) {
    NSButton* button = [self button:i];
    if (!button) {
      continue;
    }
    superview = button.superview;
    const NSPoint origin = [self target:i];
    _placed[i] = origin;
    _hasPlaced[i] = YES;
    if (!NSEqualPoints(button.frame.origin, origin)) {
      [button setFrameOrigin:origin];
    }
  }
  // AppKit 有时马上又把它们排回去：再试几次。
  BOOL moved = NO;
  for (int i = 0; i < kCount; ++i) {
    NSButton* button = [self button:i];
    if (button && !NSEqualPoints(button.frame.origin, _placed[i])) {
      moved = YES;
      _hasPlaced[i] = NO;
    }
  }
  if (moved && _placeRetries < 3) {
    ++_placeRetries;
    [self queuePlace];
  } else {
    _placeRetries = 0;
  }
  [superview updateTrackingAreas];
  [_window.contentView.superview updateTrackingAreas];
}

- (BOOL)onScreen {
  for (int i = 0; i < kCount; ++i) {
    NSButton* button = [self button:i];
    if (button && !button.hidden &&
        (!button.layer || button.layer.opacity > 0)) {
      return YES;
    }
  }
  return NO;
}

- (void)apply:(BOOL)animated {
  if (!_window) {
    return;
  }
  const int generation = ++_generation;
  const BOOL visible = _hasOrigin || _fullScreen;
  const BOOL needsMove = [self needsMove];
  if (needsMove && [self onScreen] && animated) {
    __weak StarryWindowButtons* weakSelf = self;
    [self fadeTo:0
          duration:kFadeOut
        completion:^{
          StarryWindowButtons* strongSelf = weakSelf;
          if (!strongSelf || strongSelf->_generation != generation) {
            return;
          }
          [strongSelf place];
          [strongSelf show:visible animated:YES generation:generation];
        }];
    return;
  }
  if (needsMove) {
    [self place];
  }
  [self show:visible animated:animated generation:generation];
}

- (void)show:(BOOL)visible animated:(BOOL)animated generation:(int)generation {
  if (visible) {
    for (int i = 0; i < kCount; ++i) {
      NSButton* button = [self button:i];
      if (button && button.hidden) {
        button.layer.opacity = 0;
        button.hidden = NO;
      }
    }
    [self placeNow];
    [self fadeTo:1 duration:animated ? kFadeIn : 0 completion:nil];
    return;
  }
  // 要真的藏起来：透明的按钮仍然能被点到。
  __weak StarryWindowButtons* weakSelf = self;
  [self fadeTo:0
        duration:animated ? kFadeOut : 0
      completion:^{
        StarryWindowButtons* strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_generation != generation) {
          return;
        }
        for (int i = 0; i < kCount; ++i) {
          [strongSelf button:i].hidden = YES;
        }
        [strongSelf placeNow];
      }];
}

- (void)fadeTo:(float)opacity
      duration:(NSTimeInterval)duration
    completion:(void (^)(void))completion {
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  if (completion) {
    [CATransaction setCompletionBlock:completion];
  }
  for (int i = 0; i < kCount; ++i) {
    NSButton* button = [self button:i];
    if (!button) {
      continue;
    }
    if (!button.layer) {
      button.wantsLayer = YES;
    }
    CALayer* layer = button.layer;
    const float from = layer.presentationLayer ? layer.presentationLayer.opacity
                                               : layer.opacity;
    layer.opacity = opacity;
    if (duration > 0) {
      CABasicAnimation* fade =
          [CABasicAnimation animationWithKeyPath:@"opacity"];
      fade.fromValue = @(from);
      fade.toValue = @(opacity);
      fade.duration = duration;
      fade.timingFunction = [CAMediaTimingFunction
          functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
      [layer addAnimation:fade forKey:kFadeKey];
    } else {
      [layer removeAnimationForKey:kFadeKey];
    }
  }
  [CATransaction commit];
}

@end

namespace starry {

void SetWindowButtonsPlacement(views::Widget* widget,
                               std::optional<gfx::Point> origin,
                               bool animated) {
  if (!widget) {
    return;
  }
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  if (!window || ![window standardWindowButton:NSWindowCloseButton]) {
    return;
  }
  StarryWindowButtons* placer =
      objc_getAssociatedObject(window, &kAssociatedKey);
  if (!placer) {
    placer = [[StarryWindowButtons alloc] initWithWindow:window];
    objc_setAssociatedObject(window, &kAssociatedKey, placer,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  [placer setOrigin:origin ? NSMakePoint(origin->x(), origin->y()) : NSZeroPoint
          hasOrigin:origin.has_value()
           animated:animated];
}

}  // namespace starry
