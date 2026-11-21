// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/swipe/overscroll_delegate.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cc/input/overscroll_behavior.h"
#include "content/browser/renderer_host/overscroll_controller.h"
#include "content/browser/renderer_host/overscroll_controller_delegate.h"
#include "content/public/browser/preloading.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "ui/base/window_open_disposition.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/events/types/scroll_types.h"

namespace starry {

namespace {

using content::OverscrollMode;

// 松手前手指停了这么久就不算甩；两次更新隔得比这久时速度不和之前的平均。
constexpr base::TimeDelta kFlickIdle = base::Milliseconds(60);
constexpr base::TimeDelta kVelocityGap = base::Milliseconds(50);

OverscrollSwipeFactory& Factory() {
  static base::NoDestructor<OverscrollSwipeFactory> factory;
  return *factory;
}

const char* ModeName(OverscrollMode mode) {
  switch (mode) {
    case content::OVERSCROLL_NONE:
      return "none";
    case content::OVERSCROLL_WEST:
      return "west";
    case content::OVERSCROLL_EAST:
      return "east";
  }
  return "?";
}

class StarryOverscrollDelegate
    : public content::OverscrollControllerDelegate,
      public content::WebContentsUserData<StarryOverscrollDelegate> {
 public:
  StarryOverscrollDelegate(const StarryOverscrollDelegate&) = delete;
  StarryOverscrollDelegate& operator=(const StarryOverscrollDelegate&) = delete;
  ~StarryOverscrollDelegate() override = default;

  void SetFallback(content::OverscrollControllerDelegate* fallback) {
    if (fallback) {
      fallback_ = fallback->GetWeakPtr();
    }
  }

  std::string DebugDescription() const {
    if (swipe_) {
      return swipe_->DebugDescription();
    }
    return absorbing_ ? "released" : "none";
  }

  // content::OverscrollControllerDelegate:
  gfx::Size GetDisplaySize() const override {
    if (fallback_) {
      return fallback_->GetDisplaySize();
    }
    display::Screen* screen = display::Screen::Get();
    return screen ? screen->GetDisplayNearestView(contents_->GetNativeView())
                        .size()
                  : gfx::Size();
  }

  bool OnOverscrollUpdate(float delta_x) override {
    return OnOverscrollUpdateWithVerticalDelta(delta_x, 0.f);
  }

  bool OnOverscrollUpdateWithVerticalDelta(float delta_x,
                                           float delta_y) override {
    if (absorbing_) {
      return true;
    }
    if (!swipe_) {
      return fallback_ && fallback_->OnOverscrollUpdate(delta_x);
    }
    const float dx = mode_ == content::OVERSCROLL_EAST ? delta_x : -delta_x;
    TrackVelocity(dx);
    swipe_->Update(dx, delta_y);
    return true;
  }

  void OnOverscrollComplete(OverscrollMode overscroll_mode) override {
    if (absorbing_) {
      absorbing_ = false;
      return;
    }
    if (!swipe_) {
      if (fallback_) {
        fallback_->OnOverscrollComplete(overscroll_mode);
      }
      return;
    }
    // 控制器自己判定完成了（没先看到手指离开）：按松手算。
    Release(ReleaseVelocity());
    absorbing_ = false;
  }

  void OnOverscrollModeChange(OverscrollMode old_mode,
                              OverscrollMode new_mode,
                              content::OverscrollSource source,
                              cc::OverscrollBehavior behavior) override {
    // 控制器换方向总是先回到 NONE：这次翻页到此为止。Chrome 的那一套没见过它开始，不告诉
    // 它结束。没看到手指离开就重置了（按了键、新的滚动）：取消。
    if (swipe_ || absorbing_) {
      swipe_.reset();
      absorbing_ = false;
      if (new_mode == content::OVERSCROLL_NONE) {
        return;
      }
    }
    if ((new_mode == content::OVERSCROLL_EAST ||
         new_mode == content::OVERSCROLL_WEST) &&
        behavior.PropagatesXScroll() && Factory()) {
      const bool forward =
          new_mode == (base::i18n::IsRTL() ? content::OVERSCROLL_EAST
                                           : content::OVERSCROLL_WEST);
      swipe_ = Factory().Run(&GetWebContents(), forward);
      if (swipe_) {
        mode_ = new_mode;
        velocity_ = 0;
        last_dx_ = 0;
        last_update_ = base::TimeTicks::Now();
        if (!forward) {
          GetWebContents().BackNavigationLikely(
              content::preloading_predictor::kBackGestureNavigation,
              WindowOpenDisposition::kCurrentTab);
        }
        return;
      }
    }
    if (fallback_) {
      fallback_->OnOverscrollModeChange(old_mode, new_mode, source, behavior);
    }
  }

  std::optional<float> GetMaxOverscrollDelta() const override {
    if (swipe_ || absorbing_) {
      return std::nullopt;
    }
    return fallback_ ? fallback_->GetMaxOverscrollDelta() : std::nullopt;
  }

  bool KeepsOverscrollMode() const override {
    return swipe_ && swipe_->KeepsDirection();
  }

  void OnOverscrollEvent(const blink::WebInputEvent& event) override {
    if (!swipe_) {
      return;
    }
    switch (event.GetType()) {
      case blink::WebInputEvent::Type::kGestureScrollEnd:
        // 触控板的手指离开、触摸屏松手（惯性阶段的结束也算：前面没看到别的）。
        Release(ReleaseVelocity());
        break;
      case blink::WebInputEvent::Type::kGestureFlingStart: {
        const float velocity = static_cast<const blink::WebGestureEvent&>(event)
                                   .data.fling_start.velocity_x;
        Release(mode_ == content::OVERSCROLL_EAST ? velocity : -velocity);
        break;
      }
      case blink::WebInputEvent::Type::kGestureScrollUpdate:
        // 惯性开始：手指已经离开了。
        if (static_cast<const blink::WebGestureEvent&>(event)
                .data.scroll_update.inertial_phase ==
            blink::WebGestureEvent::InertialPhaseState::kMomentum) {
          Release(ReleaseVelocity());
        }
        break;
      default:
        break;
    }
  }

 private:
  friend WebContentsUserData;

  explicit StarryOverscrollDelegate(content::WebContents* contents)
      : content::WebContentsUserData<StarryOverscrollDelegate>(*contents),
        contents_(contents) {}

  void TrackVelocity(float dx) {
    const base::TimeTicks now = base::TimeTicks::Now();
    const base::TimeDelta gap = now - last_update_;
    if (gap < base::Milliseconds(1)) {
      return;
    }
    const float instant = (dx - last_dx_) / gap.InSecondsF();
    velocity_ =
        gap > kVelocityGap ? instant : 0.6f * instant + 0.4f * velocity_;
    last_dx_ = dx;
    last_update_ = now;
  }

  float ReleaseVelocity() const {
    return base::TimeTicks::Now() - last_update_ > kFlickIdle ? 0 : velocity_;
  }

  // 手指离开：交给 SwipeNavigation 决定翻不翻，晚一个任务（见头文件）。这次滚动剩下的
  // 回调（惯性、控制器自己的完成和重置）都吞掉。
  void Release(float velocity) {
    std::unique_ptr<OverscrollSwipe> swipe = std::move(swipe_);
    absorbing_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce([](std::unique_ptr<OverscrollSwipe> swipe,
                          float velocity) { swipe->Release(velocity); },
                       std::move(swipe), velocity));
  }

  // 同 GetWebContents()，只是 const 的方法里也要取它的视图。
  const raw_ptr<content::WebContents> contents_;
  base::WeakPtr<content::OverscrollControllerDelegate> fallback_;
  std::unique_ptr<OverscrollSwipe> swipe_;
  // 松了手、这次滚动还没结束。
  bool absorbing_ = false;
  OverscrollMode mode_ = content::OVERSCROLL_NONE;
  // 朝翻页方向的速度（DIP/秒）和上一次更新。
  float velocity_ = 0;
  float last_dx_ = 0;
  base::TimeTicks last_update_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(StarryOverscrollDelegate);

// ---- 调试 ----

struct Simulation {
  base::WeakPtr<content::WebContents> contents;
  std::unique_ptr<content::OverscrollController> controller;
  blink::WebGestureDevice device = blink::WebGestureDevice::kTouchpad;
};

Simulation& GetSimulation() {
  static base::NoDestructor<Simulation> simulation;
  return *simulation;
}

// 同 RenderWidgetHostViewAura：控制器不吞的事件「交给网页」，网页没消费。
void Send(content::OverscrollController& controller,
          const blink::WebInputEvent& event) {
  if (!controller.WillHandleEvent(event)) {
    controller.ReceivedEventACK(event, /*processed=*/false);
  }
}

blink::WebGestureEvent Gesture(blink::WebInputEvent::Type type,
                               blink::WebGestureDevice device) {
  return blink::WebGestureEvent(type, blink::WebInputEvent::kNoModifiers,
                                base::TimeTicks::Now(), device);
}

}  // namespace

void SetOverscrollSwipeFactory(OverscrollSwipeFactory factory) {
  Factory() = std::move(factory);
}

base::WeakPtr<content::OverscrollControllerDelegate> OverscrollDelegateFor(
    content::WebContents* contents,
    content::OverscrollControllerDelegate* fallback) {
  StarryOverscrollDelegate::CreateForWebContents(contents);
  StarryOverscrollDelegate* delegate =
      StarryOverscrollDelegate::FromWebContents(contents);
  delegate->SetFallback(fallback);
  return delegate->GetWeakPtr();
}

std::string SimulateOverscrollForTesting(content::WebContents* contents,
                                         const std::string& command) {
  Simulation& simulation = GetSimulation();
  const std::vector<std::string> parts = base::SplitString(
      command, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  const std::string name = parts.empty() ? std::string() : parts[0];
  double first = 0;
  double second = 0;
  if (parts.size() > 1) {
    base::StringToDouble(parts[1], &first);
  }
  if (parts.size() > 2) {
    base::StringToDouble(parts[2], &second);
  }

  if (name == "begin") {
    simulation.contents = contents->GetWeakPtr();
    simulation.controller = std::make_unique<content::OverscrollController>();
    simulation.controller->set_delegate(
        OverscrollDelegateFor(contents, nullptr));
    simulation.device = parts.size() > 1 && parts[1] == "touchscreen"
                            ? blink::WebGestureDevice::kTouchscreen
                            : blink::WebGestureDevice::kTouchpad;
  }
  if (!simulation.controller || simulation.contents.get() != contents) {
    return "no simulation (begin first)";
  }
  content::OverscrollController& controller = *simulation.controller;
  const blink::WebGestureDevice device = simulation.device;
  const bool touchpad = device == blink::WebGestureDevice::kTouchpad;
  using Type = blink::WebInputEvent::Type;
  using Phase = blink::WebGestureEvent::InertialPhaseState;

  if (name == "begin") {
    blink::WebGestureEvent event = Gesture(Type::kGestureScrollBegin, device);
    event.data.scroll_begin.delta_hint_units =
        ui::ScrollGranularity::kScrollByPrecisePixel;
    Send(controller, event);
  } else if (name == "scroll" || name == "momentum") {
    blink::WebGestureEvent event = Gesture(Type::kGestureScrollUpdate, device);
    event.data.scroll_update.delta_x = static_cast<float>(first);
    event.data.scroll_update.delta_y = static_cast<float>(second);
    event.data.scroll_update.delta_units =
        ui::ScrollGranularity::kScrollByPrecisePixel;
    event.data.scroll_update.inertial_phase =
        name == "momentum" ? Phase::kMomentum : Phase::kNonMomentum;
    Send(controller, event);
  } else if (name == "end" || name == "momentumend") {
    blink::WebGestureEvent event = Gesture(Type::kGestureScrollEnd, device);
    event.data.scroll_end.delta_units =
        ui::ScrollGranularity::kScrollByPrecisePixel;
    event.data.scroll_end.inertial_phase = name == "momentumend" || !touchpad
                                               ? Phase::kMomentum
                                               : Phase::kNonMomentum;
    Send(controller, event);
  } else if (name == "fling") {
    blink::WebGestureEvent event = Gesture(Type::kGestureFlingStart, device);
    event.data.fling_start.velocity_x = static_cast<float>(first);
    Send(controller, event);
  } else if (name == "reset") {
    blink::WebKeyboardEvent event(Type::kRawKeyDown,
                                  blink::WebInputEvent::kNoModifiers,
                                  base::TimeTicks::Now());
    Send(controller, event);
  }

  StarryOverscrollDelegate* delegate =
      StarryOverscrollDelegate::FromWebContents(contents);
  return base::StrCat(
      {"mode=", ModeName(controller.overscroll_mode()), " swipe=",
       delegate ? delegate->DebugDescription() : std::string("none")});
}

}  // namespace starry
