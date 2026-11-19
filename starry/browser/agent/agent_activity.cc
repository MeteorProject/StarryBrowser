// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/agent_activity.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "starry/browser/agent/page_dialogs.h"
#include "starry/browser/l10n/l10n.h"

namespace starry::agent {

namespace {

// 一步做完后说明、高亮框留多久，再过多久光标淡出。
constexpr base::TimeDelta kLinger = base::Milliseconds(1200);
constexpr base::TimeDelta kCursorLinger = base::Milliseconds(2500);

}  // namespace

std::u16string AgentActivity::Line(content::WebContents* contents) const {
  if (action) {
    return *action;
  }
  if (approval) {
    return *approval;
  }
  if (contents && FindPageDialog(contents)) {
    return L("agent.action.dialogWaiting");
  }
  if (file_chooser) {
    return L("agent.action.fileChooserWaiting");
  }
  return L("agent.action.idle");
}

AgentActivities::Entry::Entry() = default;
AgentActivities::Entry::Entry(Entry&&) = default;
AgentActivities::Entry& AgentActivities::Entry::operator=(Entry&&) = default;
AgentActivities::Entry::~Entry() = default;

// static
AgentActivities* AgentActivities::Get() {
  static base::NoDestructor<AgentActivities> activities;
  return activities.get();
}

AgentActivities::AgentActivities()
    : dialogs_(AddPageDialogObserver(base::BindRepeating(
          [](AgentActivities* activities, content::WebContents*) {
            // 提示条上「网页弹出了对话框」那一句跟着变。
            if (!activities->entries_.empty()) {
              activities->Notify();
            }
          },
          base::Unretained(this)))) {}

AgentActivities::~AgentActivities() = default;

void AgentActivities::Begin(const void* owner,
                            tabs::TabInterface* tab,
                            std::u16string client,
                            bool assistant,
                            std::u16string action,
                            base::RepeatingClosure stop,
                            bool pointing) {
  if (!tab) {
    return;
  }
  Entry& entry = entries_[owner];
  if (entry.tab != tab->GetHandle()) {
    // 换到了别的标签：原来那个标签上的提示收起，新的从「做这一步」开始。
    entry.activity = AgentActivity();
    entry.tab = tab->GetHandle();
    entry.started = next_sequence_;
    // 标签关掉、挪到别的窗口：小卡片、渐变环跟着变（标签还在列表里，晚一步再摆）。
    entry.detach = tab->RegisterWillDetach(base::BindRepeating(
        [](AgentActivities* activities, tabs::TabInterface*,
           tabs::TabInterface::DetachReason) { activities->NotifySoon(); },
        base::Unretained(this)));
  }
  entry.activity.client = std::move(client);
  entry.activity.assistant = assistant;
  entry.activity.action = std::move(action);
  entry.activity.approval.reset();
  entry.activity.typing = false;
  if (!pointing) {
    entry.activity.target.reset();
  }
  entry.stop = std::move(stop);
  ++entry.step;
  entry.sequence = next_sequence_++;
  if (entry.idle) {
    entry.idle->Stop();
  }
  Notify();
}

void AgentActivities::AwaitApproval(const void* owner, std::u16string text) {
  Entry* entry = MutableEntry(owner);
  if (!entry) {
    return;
  }
  entry->activity.action.reset();
  entry->activity.approval = std::move(text);
  Notify();
}

void AgentActivities::Finish(const void* owner) {
  Entry* entry = MutableEntry(owner);
  if (!entry) {
    return;
  }
  if (!entry->idle) {
    entry->idle = std::make_unique<base::OneShotTimer>();
  }
  entry->idle->Start(
      FROM_HERE, kLinger,
      base::BindOnce(&AgentActivities::Idle, base::Unretained(this), owner,
                     entry->step));
}

void AgentActivities::SetFileChooser(const void* owner, bool open) {
  Entry* entry = MutableEntry(owner);
  if (!entry || entry->activity.file_chooser == open) {
    return;
  }
  entry->activity.file_chooser = open;
  Notify();
}

void AgentActivities::Idle(const void* owner, int step) {
  Entry* entry = MutableEntry(owner);
  if (!entry || entry->step != step) {
    return;
  }
  entry->activity.action.reset();
  entry->activity.approval.reset();
  entry->activity.target.reset();
  entry->activity.typing = false;
  entry->activity.holding = false;
  // 光标再留一会儿，下一步多半就在附近。
  entry->idle->Start(FROM_HERE, kCursorLinger,
                     base::BindOnce(&AgentActivities::HideCursor,
                                    base::Unretained(this), owner, step));
  Notify();
}

void AgentActivities::HideCursor(const void* owner, int step) {
  Entry* entry = MutableEntry(owner);
  if (!entry || entry->step != step || !entry->activity.cursor) {
    return;
  }
  entry->activity.cursor.reset();
  Notify();
}

void AgentActivities::End(const void* owner) {
  if (entries_.erase(owner)) {
    Notify();
  }
}

void AgentActivities::Point(const void* owner,
                            const gfx::RectF& target,
                            const gfx::PointF& cursor) {
  Entry* entry = MutableEntry(owner);
  if (!entry) {
    return;
  }
  entry->activity.target = target;
  entry->activity.cursor = cursor;
  Notify();
}

void AgentActivities::MoveCursor(const void* owner, const gfx::PointF& cursor) {
  Entry* entry = MutableEntry(owner);
  if (!entry) {
    return;
  }
  entry->activity.cursor = cursor;
  Notify();
}

void AgentActivities::Press(const void* owner) {
  Entry* entry = MutableEntry(owner);
  if (!entry) {
    return;
  }
  ++entry->activity.clicks;
  Notify();
}

void AgentActivities::Scan(const void* owner) {
  Entry* entry = MutableEntry(owner);
  if (!entry) {
    return;
  }
  ++entry->activity.scans;
  Notify();
}

void AgentActivities::SetTyping(const void* owner) {
  Entry* entry = MutableEntry(owner);
  if (!entry || entry->activity.typing) {
    return;
  }
  entry->activity.typing = true;
  Notify();
}

void AgentActivities::BeginDrag(const void* owner,
                                const gfx::RectF& target,
                                const gfx::PointF& cursor) {
  Entry* entry = MutableEntry(owner);
  if (!entry) {
    return;
  }
  entry->activity.holding = true;
  entry->activity.target = target;
  entry->activity.cursor = cursor;
  Notify();
}

void AgentActivities::EndDrag(const void* owner) {
  Entry* entry = MutableEntry(owner);
  if (!entry || !entry->activity.holding) {
    return;
  }
  entry->activity.holding = false;
  Notify();
}

AgentActivities::Entry* AgentActivities::MutableEntry(const void* owner) {
  auto it = entries_.find(owner);
  return it != entries_.end() ? &it->second : nullptr;
}

const AgentActivities::Entry* AgentActivities::EntryFor(
    const tabs::TabInterface* tab) const {
  if (!tab) {
    return nullptr;
  }
  const Entry* found = nullptr;
  for (const auto& [owner, entry] : entries_) {
    if (entry.tab.Get() == tab &&
        (!found || entry.sequence > found->sequence)) {
      found = &entry;
    }
  }
  return found;
}

const AgentActivity* AgentActivities::For(const tabs::TabInterface* tab) const {
  const Entry* entry = EntryFor(tab);
  return entry ? &entry->activity : nullptr;
}

std::vector<std::pair<tabs::TabInterface*, const AgentActivity*>>
AgentActivities::All() const {
  std::vector<const Entry*> shown;
  for (const auto& [owner, entry] : entries_) {
    tabs::TabInterface* tab = entry.tab.Get();
    if (tab && EntryFor(tab) == &entry) {
      shown.push_back(&entry);
    }
  }
  std::ranges::sort(shown, [](const Entry* a, const Entry* b) {
    return a->started < b->started;
  });
  std::vector<std::pair<tabs::TabInterface*, const AgentActivity*>> all;
  for (const Entry* entry : shown) {
    all.emplace_back(entry->tab.Get(), &entry->activity);
  }
  return all;
}

void AgentActivities::Stop(const tabs::TabInterface* tab) {
  const Entry* entry = EntryFor(tab);
  if (entry && entry->stop) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                             entry->stop);
  }
}

base::CallbackListSubscription AgentActivities::AddObserver(
    base::RepeatingClosure callback) {
  return observers_.Add(std::move(callback));
}

void AgentActivities::NotifySoon() {
  if (notify_posted_) {
    return;
  }
  notify_posted_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](AgentActivities* activities) {
                       activities->notify_posted_ = false;
                       activities->Notify();
                     },
                     base::Unretained(this)));
}

void AgentActivities::Notify() {
  observers_.Notify();
}

}  // namespace starry::agent
