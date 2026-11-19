// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/archive/archive_notice.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "starry/browser/l10n/l10n.h"

namespace starry {

namespace {

// 被看到以后多久收回去；鼠标离开以后再等多久。
constexpr base::TimeDelta kShown = base::Seconds(8);
constexpr base::TimeDelta kAfterHover = base::Seconds(3);
// 整理过多久还没被看到就不提示了（过了一夜回来还算「刚」）。
constexpr base::TimeDelta kStale = base::Hours(12);
constexpr base::TimeDelta kCheckInterval = base::Milliseconds(500);

std::map<BrowserWindowInterface*, ArchiveNotice*>& Notices() {
  static base::NoDestructor<std::map<BrowserWindowInterface*, ArchiveNotice*>>
      notices;
  return *notices;
}

}  // namespace

// static
ArchiveNotice* ArchiveNotice::From(BrowserWindowInterface* browser) {
  auto it = Notices().find(browser);
  return it == Notices().end() ? nullptr : it->second;
}

ArchiveNotice::ArchiveNotice(BrowserWindowInterface* browser)
    : browser_(browser) {
  Notices()[browser] = this;
  Profile* profile = browser->GetProfile();
  if (!profile->IsOffTheRecord()) {
    observation_.Observe(tab_organizer::TabOrganizer::ForProfile(profile));
    archive_observation_.Observe(
        tab_organizer::TabArchive::ForProfile(profile));
  }
}

ArchiveNotice::~ArchiveNotice() {
  Notices().erase(browser_);
}

void ArchiveNotice::Hold(bool hold) {
  if (held_ == hold || !active()) {
    held_ = hold;
    return;
  }
  held_ = hold;
  if (hold) {
    dismiss_timer_.Stop();
  } else if (shown_) {
    dismiss_timer_.Start(
        FROM_HERE, kAfterHover,
        base::BindOnce(&ArchiveNotice::Dismiss, base::Unretained(this)));
  }
}

void ArchiveNotice::Undo() {
  if (!active()) {
    return;
  }
  const std::string batch = batch_id_;
  // 先收起（撤销的时候标签回来，列表会动）。
  Clear();
  tab_organizer::TabOrganizer::ForProfile(browser_->GetProfile())->Undo(batch);
}

void ArchiveNotice::Dismiss() {
  if (active()) {
    Clear();
  }
}

base::CallbackListSubscription ArchiveNotice::AddChangedCallback(
    base::RepeatingClosure callback) {
  return changed_.Add(std::move(callback));
}

void ArchiveNotice::ShowForTesting(const std::string& batch_id, int count) {
  Start(Kind::kArchive, batch_id, {}, count, base::Time::Now());
}

std::u16string ArchiveNotice::ToastText() const {
  return kind_ == Kind::kOrganize
             ? L("organize.toast.text", {{"count", count_}})
             : L("archive.toast.text", {{"count", count_}});
}

std::string ArchiveNotice::DescribeForTesting() const {
  return base::StrCat(
      {"notice active=", active() ? "1" : "0",
       " kind=", kind_ == Kind::kOrganize ? "organize" : "archive",
       " batch=", batch_id_, " count=", base::NumberToString(count_),
       " place=", shown_ ? "toast" : "none", " held=", held_ ? "1" : "0",
       " counting=", dismiss_timer_.IsRunning() ? "1" : "0"});
}

void ArchiveNotice::OnBatchDone(const tab_organizer::Batch& batch) {
  // 一键整理：放进了这个窗口里的标签就提示放进了几个文件夹。
  if (batch.source == tab_organizer::Source::kOrganize) {
    const bool here = std::ranges::any_of(
        batch.moves, [this](const tab_organizer::Batch::Move& move) {
          return move.window == browser_->GetSessionID();
        });
    if (here) {
      Start(Kind::kOrganize, batch.id, {},
            static_cast<int>(batch.folders.size()), batch.time);
    }
    return;
  }
  // 自动整理的才提示：助手的在对话里已经有结果。
  if (batch.source != tab_organizer::Source::kAuto) {
    return;
  }
  std::vector<std::string> ids;
  for (const tab_organizer::Batch::Item& item : batch.items) {
    if (item.window == browser_->GetSessionID()) {
      ids.push_back(item.archive_id);
    }
  }
  if (!ids.empty()) {
    const int count = static_cast<int>(ids.size());
    Start(Kind::kArchive, batch.id, std::move(ids), count, batch.time);
  }
}

void ArchiveNotice::OnArchiveChanged() {
  if (!active() || ids_.empty()) {
    return;
  }
  // 这一批里恢复了、删了的不算：都没了就不提示了。
  tab_organizer::TabArchive* archive =
      tab_organizer::TabArchive::ForProfile(browser_->GetProfile());
  const int remaining = static_cast<int>(std::ranges::count_if(
      ids_, [archive](const std::string& id) { return archive->Find(id); }));
  if (remaining == 0) {
    Clear();
  } else if (remaining != count_) {
    count_ = remaining;
    Notify();
  }
}

void ArchiveNotice::OnBatchUndone(const tab_organizer::Batch& batch) {
  if (batch.id == batch_id_) {
    Clear();
  }
}

void ArchiveNotice::Start(Kind kind,
                          const std::string& batch_id,
                          std::vector<std::string> ids,
                          int count,
                          base::Time time) {
  kind_ = kind;
  batch_id_ = batch_id;
  ids_ = std::move(ids);
  count_ = count;
  time_ = time;
  shown_ = false;
  held_ = false;
  dismiss_timer_.Stop();
  // 等到窗口在前台时再显示提示条。
  check_timer_.Start(
      FROM_HERE, kCheckInterval,
      base::BindRepeating(&ArchiveNotice::Check, base::Unretained(this)));
  Notify();
}

void ArchiveNotice::Check() {
  if (!active() || shown_) {
    check_timer_.Stop();
    return;
  }
  if (base::Time::Now() - time_ > kStale) {
    Clear();
    return;
  }
  if (!browser_->IsActive()) {
    return;
  }
  shown_ = true;
  check_timer_.Stop();
  if (!held_) {
    dismiss_timer_.Start(
        FROM_HERE, kShown,
        base::BindOnce(&ArchiveNotice::Dismiss, base::Unretained(this)));
  }
  Notify();
}

void ArchiveNotice::Clear() {
  batch_id_.clear();
  ids_.clear();
  count_ = 0;
  shown_ = false;
  held_ = false;
  check_timer_.Stop();
  dismiss_timer_.Stop();
  Notify();
}

void ArchiveNotice::Notify() {
  changed_.Notify();
}

}  // namespace starry
