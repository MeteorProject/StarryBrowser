// Copyright 2026 The Starry Browser Authors

#include "starry/browser/sessions/session_titles.h"

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace starry {

namespace {

// 同一个标签两次写标题至少隔这么久（Chrome 自己写网页状态最多一秒一次）。
constexpr base::TimeDelta kSaveInterval = base::Seconds(5);

}  // namespace

class SessionTitleTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<SessionTitleTabHelper> {
 public:
  SessionTitleTabHelper(const SessionTitleTabHelper&) = delete;
  SessionTitleTabHelper& operator=(const SessionTitleTabHelper&) = delete;
  ~SessionTitleTabHelper() override = default;

 private:
  friend class content::WebContentsUserData<SessionTitleTabHelper>;

  explicit SessionTitleTabHelper(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<SessionTitleTabHelper>(*contents) {}

  // content::WebContentsObserver:
  void TitleWasSet(content::NavigationEntry* entry) override {
    if (!entry ||
        entry != web_contents()->GetController().GetLastCommittedEntry()) {
      return;
    }
    if (timer_.IsRunning()) {
      pending_ = true;
      return;
    }
    Save();
  }

  void Save() {
    pending_ = false;
    content::NavigationController& controller = web_contents()->GetController();
    content::NavigationEntry* entry = controller.GetLastCommittedEntry();
    if (!entry || entry->IsInitialEntry()) {
      return;
    }
    controller.NotifyEntryChanged(entry);
    timer_.Start(FROM_HERE, kSaveInterval,
                 base::BindOnce(&SessionTitleTabHelper::OnTimer,
                                base::Unretained(this)));
  }

  void OnTimer() {
    if (pending_) {
      Save();
    }
  }

  base::OneShotTimer timer_;
  bool pending_ = false;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(SessionTitleTabHelper);

void SaveTitlesInSession(content::WebContents* contents) {
  SessionTitleTabHelper::CreateForWebContents(contents);
}

}  // namespace starry
