// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/private_windows.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime_desktop.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "ui/base/base_window.h"

namespace starry::agent {

namespace {

// 临时无痕窗口无工具调用后的自动关闭超时。
constexpr base::TimeDelta kIdleClose = base::Minutes(30);

bool ShuttingDown() {
  return browser_shutdown::IsTryingToQuit() || !g_browser_process ||
         g_browser_process->IsShuttingDown();
}

// 这个 Profile 的窗口（包括网页弹出的小窗口）。
std::vector<BrowserWindowInterface*> WindowsOf(Profile* profile) {
  std::vector<BrowserWindowInterface*> windows;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetProfile() == profile) {
      windows.push_back(browser);
    }
  }
  return windows;
}

void DestroyProfile(base::WeakPtr<Profile> profile) {
  if (profile && !ShuttingDown()) {
    ProfileDestroyer::DestroyOTRProfileWhenAppropriate(profile.get());
  }
}

}  // namespace

struct PrivateWindows::Entry {
  raw_ptr<Profile> original;
  raw_ptr<Profile> profile;
  std::u16string name;
  base::OneShotTimer idle;
};

// static
PrivateWindows* PrivateWindows::Get() {
  static base::NoDestructor<PrivateWindows> windows;
  return windows.get();
}

PrivateWindows::PrivateWindows() : idle_(kIdleClose) {}
PrivateWindows::~PrivateWindows() = default;

BrowserWindowInterface* PrivateWindows::Open(const void* owner,
                                             Profile* profile,
                                             const std::u16string& name) {
  if (ShuttingDown()) {
    return nullptr;
  }
  auto it = entries_.find(owner);
  if (it == entries_.end()) {
    Profile* original = profile->GetOriginalProfile();
    auto entry = std::make_unique<Entry>();
    entry->original = original;
    // 每个智能体自己一个：和用户的无痕窗口（主无痕 Profile）、别的智能体都不共用。
    // 用 DevTools 的 ID（同 Target.createBrowserContext）：Chrome 只让这种非主无痕
    // Profile 开浏览器窗口（Profile::OTRProfileID::AllowsBrowserWindows），不用打补丁。
    // 它的设置全在内存里，窗口里改的不写回用户资料。
    entry->profile = original->GetOffTheRecordProfile(
        Profile::OTRProfileID::CreateUniqueForDevTools(),
        /*create_if_needed=*/true);
    profiles_.AddObservation(entry->profile);
    if (!browsers_.IsObserving()) {
      browsers_.Observe(GlobalBrowserCollection::GetInstance());
    }
    it = entries_.emplace(owner, std::move(entry)).first;
    StartIdle(owner);
  }
  // 先记下名字：窗口建出来时工具栏的标记要用。
  Entry& entry = *it->second;
  entry.name = name;
  for (BrowserWindowInterface* browser : WindowsOf(entry.profile)) {
    if (browser->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
      return browser;
    }
  }
  if (GetBrowserWindowCreationStatusForProfile(*entry.profile) !=
      BrowserWindowInterface::CreationStatus::kOk) {
    return nullptr;
  }
  BrowserWindowCreateParams params(*entry.profile,
                                   /*from_user_gesture=*/false);
  params.omit_from_session_restore = true;
  // 不显示：第一个标签用 NEW_BACKGROUND_TAB 打开时 Navigate 也不会拿出来
  // （目标就是这个窗口，见 browser_navigator.cc 的「If a new window has been
  // created」）。
  return CreateBrowserWindow(std::move(params));
}

Profile* PrivateWindows::ProfileOf(const void* owner) const {
  auto it = entries_.find(owner);
  return it != entries_.end() ? it->second->profile.get() : nullptr;
}

void PrivateWindows::Touch(const void* owner, const std::u16string& name) {
  auto it = entries_.find(owner);
  if (it == entries_.end()) {
    return;
  }
  it->second->name = name;
  StartIdle(owner);
}

void PrivateWindows::Close(const void* owner) {
  auto it = entries_.find(owner);
  if (it == entries_.end()) {
    return;
  }
  Profile* profile = it->second->profile;
  entries_.erase(it);
  Dispose(profile);
}

void PrivateWindows::SettingsChanged() {
  std::vector<const void*> closing;
  for (const auto& [owner, entry] : entries_) {
    if (!prefs::PrefsFor(entry->original)
             ->GetBoolean(prefs::kAgentAllowPrivateTabs)) {
      closing.push_back(owner);
    }
  }
  for (const void* owner : closing) {
    Close(owner);
  }
}

std::optional<std::u16string> PrivateWindows::OwnerName(
    Profile* profile) const {
  if (Entry* entry = EntryFor(profile)) {
    return entry->name;
  }
  return std::nullopt;
}

void PrivateWindows::SetIdleTimeForTesting(base::TimeDelta idle) {
  idle_ = idle;
  for (const auto& [owner, entry] : entries_) {
    StartIdle(owner);
  }
}

PrivateWindows::Entry* PrivateWindows::EntryFor(Profile* profile) const {
  for (const auto& [owner, entry] : entries_) {
    if (entry->profile == profile) {
      return entry.get();
    }
  }
  return nullptr;
}

void PrivateWindows::StartIdle(const void* owner) {
  auto it = entries_.find(owner);
  if (it == entries_.end()) {
    return;
  }
  // owner 只当作键：计时器跟着 Entry，Entry 没了它也停了。
  it->second->idle.Start(FROM_HERE, idle_,
                         base::BindOnce(&PrivateWindows::IdleFired,
                                        base::Unretained(this), owner));
}

void PrivateWindows::IdleFired(const void* owner) {
  Profile* profile = ProfileOf(owner);
  if (!profile) {
    return;
  }
  // 用户正看着（包括最小化了）：留着，过一会儿再看。
  for (BrowserWindowInterface* browser : WindowsOf(profile)) {
    ui::BaseWindow* window = browser->GetWindow();
    if (window && (window->IsVisible() || window->IsMinimized())) {
      StartIdle(owner);
      return;
    }
  }
  Close(owner);
}

void PrivateWindows::Dispose(Profile* profile) {
  if (ShuttingDown()) {
    // 退出时 Chrome 自己关窗口、销毁 Profile。
    Forget(profile);
    return;
  }
  if (WindowsOf(profile).empty()) {
    DestroyLater(profile);
    return;
  }
  disposing_.insert(profile);
  // 不问 beforeunload：智能体的窗口没人在看，挂着的对话框当作取消。窗口都关了以后
  // OnBrowserClosed 销毁 Profile。
  chrome::CloseAllBrowsersWithIncognitoProfile(profile,
                                               /*skip_beforeunload=*/true);
}

void PrivateWindows::DestroyLater(Profile* profile) {
  base::WeakPtr<Profile> weak = profile->GetWeakPtr();
  Forget(profile);
  // 窗口关闭的过程中 Profile 可能还在用：晚一步再销毁（同 DevTools 的
  // DevToolsBrowserContextManager::OnBrowserClosed）。
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&DestroyProfile, std::move(weak)));
}

void PrivateWindows::Forget(Profile* profile) {
  std::erase_if(entries_, [profile](const auto& entry) {
    return entry.second->profile == profile;
  });
  disposing_.erase(profile);
  if (profiles_.IsObservingSource(profile)) {
    profiles_.RemoveObservation(profile);
  }
  if (entries_.empty() && disposing_.empty()) {
    browsers_.Reset();
  }
}

void PrivateWindows::OnBrowserClosed(BrowserWindowInterface* browser) {
  Profile* profile = browser->GetProfile();
  if (!EntryFor(profile) && !disposing_.contains(profile)) {
    return;
  }
  // 还有这个 Profile 的窗口（网页弹出的小窗口……）：留着。
  for (BrowserWindowInterface* other : WindowsOf(profile)) {
    if (other != browser) {
      return;
    }
  }
  // 用户关掉了窗口、智能体关掉了最后一个标签、上面 Dispose 关完了：网站数据一起丢掉，
  // 下次开的是新的。
  if (ShuttingDown()) {
    Forget(profile);
    return;
  }
  DestroyLater(profile);
}

void PrivateWindows::OnProfileWillBeDestroyed(Profile* profile) {
  Forget(profile);
}

}  // namespace starry::agent
