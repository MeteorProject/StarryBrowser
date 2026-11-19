// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/swipe/swipe_navigation.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/favicon_status.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/swipe/haptics.h"
#include "starry/browser/ui/swipe/swipe_indicator.h"
#include "ui/views/view.h"

namespace starry {

namespace {

// 拖回去不到这么多就退膛（有一点回差，停在临界处不会来回振）。列表展开以后上下移动时
// 左右难免晃，要退得更多才算。
constexpr float kDisarmProgress = 0.82f;
constexpr float kListDisarmProgress = 0.45f;
// 没上膛也翻：至少过了这么多、松手时朝前的速度（每秒几个触控板宽）够快。上了膛但松手时
// 正快速往回甩：不翻。
constexpr float kFlickProgress = 0.45f;
constexpr float kFlickVelocity = 0.55f;
// 往下选翻几页：上膛以后以上下为主地往下动多少展开列表，之后每一格多高（触控板高度的
// 几分之几）；选中的一行要越过两行的分界多少（一格的几分之几）才换，停在分界上不跳。
constexpr float kListOpenDistance = 0.035f;
constexpr float kLevelStep = 0.06f;
constexpr float kLevelHysteresis = 0.18f;
constexpr base::TimeDelta kListHoldDelay = base::Milliseconds(450);
// 动得比这多（触控板宽、高的几分之几）就不算停住，重新等 kListHoldDelay。
constexpr float kHoldSlop = 0.004f;
// 列表最多几行。
constexpr size_t kMaxEntries = 8;

StarryBrowserView* ViewFor(content::WebContents* contents) {
  tabs::TabInterface* tab =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  StarryBrowserView* view =
      browser ? StarryBrowserView::From(browser) : nullptr;
  return view && view->GetWidget() && view->swipe_indicator() ? view : nullptr;
}

}  // namespace

SwipeNavigation::Entry::Entry() = default;
SwipeNavigation::Entry::Entry(const Entry&) = default;
SwipeNavigation::Entry& SwipeNavigation::Entry::operator=(const Entry&) =
    default;
SwipeNavigation::Entry::~Entry() = default;

// static
bool SwipeNavigation::CanHandle(content::WebContents* contents) {
  return ViewFor(contents) != nullptr;
}

// static
std::unique_ptr<SwipeNavigation> SwipeNavigation::Start(
    content::WebContents* contents,
    Direction direction) {
  StarryBrowserView* view = ViewFor(contents);
  if (!view) {
    return nullptr;
  }
  const bool back = direction == Direction::kBack;
  if (back ? !chrome::CanGoBack(contents) : !chrome::CanGoForward(contents)) {
    return nullptr;
  }

  // 这个方向的历史记录，近的在前。
  content::NavigationController& controller = contents->GetController();
  const int step = back ? -1 : 1;
  std::vector<Entry> entries;
  for (int i = controller.GetCurrentEntryIndex() + step;
       i >= 0 && i < controller.GetEntryCount() && entries.size() < kMaxEntries;
       i += step) {
    content::NavigationEntry* navigation = controller.GetEntryAtIndex(i);
    if (!navigation) {
      continue;
    }
    Entry entry;
    entry.unique_id = navigation->GetUniqueID();
    entry.title = navigation->GetTitleForDisplay();
    if (const content::FaviconStatus& favicon = navigation->GetFavicon();
        favicon.valid) {
      entry.favicon = favicon.image;
    }
    entries.push_back(std::move(entry));
  }

  SwipeIndicator* indicator = view->swipe_indicator();
  std::vector<SwipeIndicator::Row> rows;
  for (const Entry& entry : entries) {
    rows.push_back({entry.title, entry.favicon});
  }
  const gfx::Rect page = views::View::ConvertRectToTarget(
      view, indicator, view->PageBoundsFor(contents));
  const int list_rows = indicator->Begin(page, !back, std::move(rows));
  auto navigation = base::WrapUnique(new SwipeNavigation(
      contents, direction, std::move(entries), indicator->GetWeakPtr()));
  navigation->list_rows_ = list_rows;
  return navigation;
}

SwipeNavigation::SwipeNavigation(content::WebContents* contents,
                                 Direction direction,
                                 std::vector<Entry> entries,
                                 base::WeakPtr<SwipeIndicator> indicator)
    : contents_(contents->GetWeakPtr()),
      direction_(direction),
      entries_(std::move(entries)),
      indicator_(std::move(indicator)) {}

SwipeNavigation::~SwipeNavigation() {
  if (!finished_) {
    Finish(/*navigate=*/false);
  }
}

void SwipeNavigation::Update(float dx, float dy) {
  if (finished_) {
    return;
  }
  const float ddx = dx - last_dx_;
  const float ddy = dy - last_dy_;
  last_dx_ = dx;
  last_dy_ = dy;
  progress_ = std::max(0.0f, dx / kCommitDistance);

  if (!armed_ && progress_ >= 1) {
    SetArmed(true);
  } else if (armed_ &&
             progress_ < (list_open_ ? kListDisarmProgress : kDisarmProgress)) {
    SetArmed(false);
  }

  if (armed_ && !list_open_ && list_rows_ >= 2) {
    // 以左右为主的移动把起点带着走：斜着拖的那一点上下漂移不算往下选。
    if (std::abs(ddy) <= std::abs(ddx)) {
      vertical_anchor_ += ddy;
    }
    if (hold_timer_.IsRunning() && std::abs(ddx) + std::abs(ddy) > kHoldSlop) {
      hold_timer_.Reset();
    }
    if (dy - vertical_anchor_ >= kListOpenDistance) {
      OpenList();
    }
  }
  if (list_open_) {
    UpdateLevel(dy);
  }
  ShowIndicator();
}

bool SwipeNavigation::Release(float velocity) {
  if (finished_) {
    return false;
  }
  bool navigate = armed_;
  if (!list_open_) {
    if (!armed_ && progress_ >= kFlickProgress && velocity >= kFlickVelocity) {
      navigate = true;
    } else if (armed_ && velocity <= -kFlickVelocity) {
      navigate = false;
    }
  }
  Finish(navigate);
  return navigate;
}

void SwipeNavigation::Commit() {
  if (!finished_) {
    Finish(/*navigate=*/true);
  }
}

void SwipeNavigation::Cancel() {
  if (!finished_) {
    Finish(/*navigate=*/false);
  }
}

void SwipeNavigation::SetArmed(bool armed) {
  armed_ = armed;
  ever_armed_ = ever_armed_ || armed;
  Feedback(/*level_change=*/false);
  if (!armed) {
    hold_timer_.Stop();
    return;
  }
  if (!list_open_ && list_rows_ >= 2) {
    vertical_anchor_ = last_dy_;
    hold_timer_.Start(
        FROM_HERE, kListHoldDelay,
        base::BindOnce(&SwipeNavigation::OnHold, base::Unretained(this)));
  }
}

void SwipeNavigation::OnHold() {
  if (finished_ || !armed_ || list_open_) {
    return;
  }
  OpenList();
  ShowIndicator();
}

void SwipeNavigation::OpenList() {
  list_open_ = true;
  hold_timer_.Stop();
  level_ = 1;
  level_origin_ = last_dy_;
}

void SwipeNavigation::UpdateLevel(float dy) {
  const int count = std::min(list_rows_, static_cast<int>(entries_.size()));
  if (count < 2) {
    return;
  }
  // 越过两头时把起点挪过去：手指往回动马上就换行，不用先把多走的走回来。
  const float last = static_cast<float>(count - 1);
  float position = (dy - level_origin_) / kLevelStep;
  if (position < 0) {
    level_origin_ = dy;
    position = 0;
  } else if (position > last) {
    level_origin_ = dy - last * kLevelStep;
    position = last;
  }
  const int current = level_ - 1;
  if (std::abs(position - static_cast<float>(current)) <=
      0.5f + kLevelHysteresis) {
    return;
  }
  const int next =
      std::clamp(static_cast<int>(std::lround(position)), 0, count - 1);
  if (next != current) {
    level_ = next + 1;
    Feedback(/*level_change=*/true);
  }
}

void SwipeNavigation::ShowIndicator() {
  if (indicator_) {
    indicator_->Update(progress_, armed_, list_open_, level_ - 1);
  }
}

void SwipeNavigation::Finish(bool navigate) {
  finished_ = true;
  hold_timer_.Stop();
  if (indicator_) {
    indicator_->Finish(navigate);
  }
  if (navigate) {
    Navigate();
  }
}

void SwipeNavigation::Navigate() {
  content::WebContents* contents = contents_.get();
  if (!contents) {
    return;
  }
  // 选了更远的一页：按记录本身找（手势当中网页又导航过，下标会变）。
  if (list_open_ && level_ > 1 &&
      static_cast<size_t>(level_) <= entries_.size()) {
    content::NavigationController& controller = contents->GetController();
    const int unique_id = entries_[level_ - 1].unique_id;
    for (int i = 0; i < controller.GetEntryCount(); ++i) {
      content::NavigationEntry* entry = controller.GetEntryAtIndex(i);
      if (entry && entry->GetUniqueID() == unique_id &&
          i != controller.GetCurrentEntryIndex()) {
        controller.GoToIndex(i);
        return;
      }
    }
  }
  // 一页：同后退、前进按钮（跳过网页没经用户操作塞进来的记录，没有记录时回到打开它的
  // 标签）。
  if (direction_ == Direction::kBack) {
    chrome::GoBack(contents);
  } else {
    chrome::GoForward(contents);
  }
}

void SwipeNavigation::Feedback(bool level_change) {
  ++haptics_;
  PerformHaptic(level_change ? Haptic::kLevelChange : Haptic::kAlignment);
}

std::string SwipeNavigation::DebugDescription() const {
  std::string text = base::StrCat(
      {"direction=", direction_ == Direction::kBack ? "back" : "forward",
       " progress=", base::NumberToString(progress_),
       " armed=", armed_ ? "1" : "0", " list=", list_open_ ? "1" : "0",
       " level=", base::NumberToString(level_), "/",
       base::NumberToString(
           std::min(list_rows_, static_cast<int>(entries_.size()))),
       " haptics=", base::NumberToString(haptics_),
       " finished=", finished_ ? "1" : "0"});
  if (level_ >= 1 && static_cast<size_t>(level_) <= entries_.size()) {
    base::StrAppend(
        &text,
        {" target=[", base::UTF16ToUTF8(entries_[level_ - 1].title), "]"});
  }
  return text;
}

}  // namespace starry
