// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_PRIVATE_WINDOWS_H_
#define STARRY_BROWSER_AGENT_PRIVATE_WINDOWS_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "base/no_destructor.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"

class BrowserWindowInterface;
class GlobalBrowserCollection;
class Profile;

// 智能体的临时无痕窗口：
// browser_tab_new 带 private 时，每个智能体（一套 ToolRunner）第一次用时建一个独立的
// 无痕 Profile（不落盘，和用户的无痕窗口、其他智能体都不共用）和它的一个普通窗口。窗口建好
// 不显示，标签在里面照常运行；智能体 browser_tab_select 带 show 时才拿出来。
//
// 什么时候关：智能体断开（ToolRunner 没了）、30 分钟没有调用工具（用户正看着时不关）、
// 设置里不再允许、用户关掉窗口、智能体关掉最后一个标签。这个 Profile 的窗口都关了以后
// Profile 销毁，Cookie、缓存、网站数据随之丢弃；下次再开是新的一个。
//
// 整个浏览器进程一个，只在 UI 线程上用。owner 是那套工具，只当作键，不解引用。
namespace starry::agent {

class PrivateWindows : public BrowserCollectionObserver,
                       public ProfileObserver {
 public:
  static PrivateWindows* Get();

  PrivateWindows(const PrivateWindows&) = delete;
  PrivateWindows& operator=(const PrivateWindows&) = delete;

  // owner 的无痕窗口，还没有就建一个（不显示）。profile 是智能体所在的用户资料，name 是
  // 窗口工具栏上写的智能体的名字。建不了（正在退出……）时是 nullptr。
  BrowserWindowInterface* Open(const void* owner,
                               Profile* profile,
                               const std::u16string& name);
  // owner 的无痕 Profile：没开过、已经关掉时是 nullptr。
  Profile* ProfileOf(const void* owner) const;
  // owner 又调用了工具：重新计时，名字也更新（客户端报的名字后来才有）。
  void Touch(const void* owner, const std::u16string& name);
  // 关掉 owner 的无痕窗口（智能体断开）。
  void Close(const void* owner);
  // 设置改了：不再允许无痕标签的用户资料下的全部关掉。
  void SettingsChanged();

  // 窗口工具栏上的标记：是智能体的无痕窗口时是智能体的名字。
  std::optional<std::u16string> OwnerName(Profile* profile) const;

  // 测试用：多久没有调用工具就关（默认 30 分钟）。
  void SetIdleTimeForTesting(base::TimeDelta idle);

 private:
  struct Entry;
  friend class base::NoDestructor<PrivateWindows>;

  PrivateWindows();
  ~PrivateWindows() override;

  Entry* EntryFor(Profile* profile) const;
  void StartIdle(const void* owner);
  void IdleFired(const void* owner);
  // 关掉这个 Profile 的所有窗口（不问 beforeunload），都关了以后销毁它。
  void Dispose(Profile* profile);
  void DestroyLater(Profile* profile);
  void Forget(Profile* profile);

  // BrowserCollectionObserver:
  void OnBrowserClosed(BrowserWindowInterface* browser) override;
  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

  std::map<const void*, std::unique_ptr<Entry>> entries_;
  // 已经不归谁了、等窗口关完再销毁的。
  std::set<Profile*> disposing_;
  base::TimeDelta idle_;
  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      browsers_{this};
  base::ScopedMultiSourceObservation<Profile, ProfileObserver> profiles_{this};
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_PRIVATE_WINDOWS_H_
