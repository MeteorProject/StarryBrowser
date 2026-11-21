// Copyright 2026 The Starry Browser Authors

#include "starry/browser/content_blocking/blocked_stats.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "starry/browser/prefs/starry_prefs.h"

namespace starry::content_blocking {

namespace {

constexpr base::TimeDelta kFlushDelay = base::Seconds(4);
constexpr int kKeepDays = 14;
const char kUserDataKey[] = "starry_blocked_stats";

std::string DayKey(base::Time time) {
  base::Time::Exploded exploded;
  time.LocalExplode(&exploded);
  return base::StringPrintf("%04d-%02d-%02d", exploded.year, exploded.month,
                            exploded.day_of_month);
}

class Pending : public base::SupportsUserData::Data {
 public:
  explicit Pending(Profile* profile) : profile_(profile) {}

  static Pending* For(Profile* profile) {
    auto* pending = static_cast<Pending*>(profile->GetUserData(kUserDataKey));
    if (!pending) {
      auto owned = std::make_unique<Pending>(profile);
      pending = owned.get();
      profile->SetUserData(kUserDataKey, std::move(owned));
    }
    return pending;
  }

  int count() const { return count_; }

  void Add() {
    ++count_;
    if (!timer_.IsRunning()) {
      timer_.Start(FROM_HERE, kFlushDelay,
                   base::BindOnce(&Pending::Flush, base::Unretained(this)));
    }
  }

  void Flush() {
    if (count_ == 0) {
      return;
    }
    const base::Time now = base::Time::Now();
    ScopedDictPrefUpdate update(profile_->GetPrefs(), prefs::kBlockedStats);
    const std::string today = DayKey(now);
    update->Set(today, update->FindInt(today).value_or(0) + count_);
    count_ = 0;
    std::vector<std::string> keep;
    for (int i = 0; i < kKeepDays; ++i) {
      keep.push_back(DayKey(now - base::Days(i)));
    }
    std::vector<std::string> stale;
    for (const auto [key, value] : *update) {
      if (std::ranges::find(keep, key) == keep.end()) {
        stale.push_back(key);
      }
    }
    for (const std::string& key : stale) {
      update->Remove(key);
    }
  }

 private:
  const raw_ptr<Profile> profile_;
  int count_ = 0;
  base::OneShotTimer timer_;
};

}  // namespace

void RecordBlocked(content::BrowserContext* context) {
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }
  Pending::For(profile)->Add();
}

WeekBlocked BlockedThisWeek(Profile* profile) {
  WeekBlocked week;
  if (!profile) {
    return week;
  }
  profile = profile->GetOriginalProfile();
  const base::DictValue& stats =
      profile->GetPrefs()->GetDict(prefs::kBlockedStats);
  const base::Time now = base::Time::Now();
  for (int i = 0; i < 7; ++i) {
    week.days[6 - i] = stats.FindInt(DayKey(now - base::Days(i))).value_or(0);
  }
  if (auto* pending =
          static_cast<Pending*>(profile->GetUserData(kUserDataKey))) {
    week.days[6] += pending->count();
  }
  for (int day : week.days) {
    week.total += day;
  }
  return week;
}

}  // namespace starry::content_blocking
