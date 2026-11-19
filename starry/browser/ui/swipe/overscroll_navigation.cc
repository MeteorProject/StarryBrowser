// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/swipe/overscroll_navigation.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "starry/browser/ui/swipe/overscroll_delegate.h"
#include "starry/browser/ui/swipe/swipe_navigation.h"

namespace starry {

namespace {

// OverscrollController 给的是 DIP（触控板是滚动的距离，触摸屏是手指走的距离），
// SwipeNavigation 用的是触控板宽、高的几分之几：拖 160 DIP 上膛（kCommitDistance 是
// 0.1；Chrome 自己要拖到屏幕宽的 30%，1080p 上五百多），快速一甩要每秒 880 DIP；上膛以后
// 往下 28 DIP 展开列表、之后每 48 DIP 多翻一页（控制器先吞掉开始的 60 DIP）。还没在
// Windows、Linux 的真机上调过。
constexpr float kDipsPerWidth = 1600;
constexpr float kDipsPerHeight = 800;

class Swipe : public OverscrollSwipe {
 public:
  explicit Swipe(std::unique_ptr<SwipeNavigation> navigation)
      : navigation_(std::move(navigation)) {}
  Swipe(const Swipe&) = delete;
  Swipe& operator=(const Swipe&) = delete;
  ~Swipe() override = default;

  // OverscrollSwipe:
  void Update(float dx, float dy) override {
    navigation_->Update(dx / kDipsPerWidth, dy / kDipsPerHeight);
  }
  void Release(float velocity) override {
    navigation_->Release(velocity / kDipsPerWidth);
  }
  bool KeepsDirection() const override { return navigation_->ever_armed(); }
  std::string DebugDescription() const override {
    return navigation_->DebugDescription();
  }

 private:
  std::unique_ptr<SwipeNavigation> navigation_;
};

std::unique_ptr<OverscrollSwipe> StartSwipe(content::WebContents* contents,
                                            bool forward) {
  std::unique_ptr<SwipeNavigation> navigation = SwipeNavigation::Start(
      contents, forward ? SwipeNavigation::Direction::kForward
                        : SwipeNavigation::Direction::kBack);
  if (!navigation) {
    return nullptr;
  }
  return std::make_unique<Swipe>(std::move(navigation));
}

}  // namespace

void RegisterOverscrollNavigation() {
  SetOverscrollSwipeFactory(base::BindRepeating(&StartSwipe));
}

}  // namespace starry
