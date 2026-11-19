// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_ARCHIVE_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_ARCHIVE_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/tab_organizer/tab_archive.h"

class BrowserWindowInterface;
class Profile;

namespace starry {

// 整页的「已归档」（starry://archive，pages/archive/）的数据：标签管理的归档
// （tab_organizer::TabArchive）。无痕窗口里看的是原来那个用户资料的，恢复到最近用的普通
// 窗口里。
//
//   getArchive(callbackId)
//     回 {tabs: [{id, url, title, host, archived, lastActive, reason, batch,
//         thumbnail}], autoArchive, idleHours, duplicates, retentionDays,
//         incognito}。时间是毫秒；thumbnail 时缩略图在 thumb/<id>.jpg。
//   restoreArchived([id…], select)：恢复到这个窗口的最后；select 时切到最后一个。
//   removeArchived([id…])：从归档里删掉。
//   clearArchive()：全部删掉。
//   openTabSettings()：设置 › 标签管理。
// 归档变了、设置变了时发 archive-changed（同 getArchive 的回复）。
class ArchiveHandler : public content::WebUIMessageHandler,
                       public tab_organizer::TabArchive::Observer {
 public:
  explicit ArchiveHandler(Profile* profile);
  ArchiveHandler(const ArchiveHandler&) = delete;
  ArchiveHandler& operator=(const ArchiveHandler&) = delete;
  ~ArchiveHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // tab_organizer::TabArchive::Observer:
  void OnArchiveChanged() override;

 private:
  base::DictValue Describe();
  void ScheduleChanged();
  void SendChanged();
  // 恢复到哪个窗口：页面所在的窗口；无痕窗口里是最近用的普通窗口。
  BrowserWindowInterface* TargetBrowser();

  void HandleGetArchive(const base::ListValue& args);
  void HandleRestore(const base::ListValue& args);
  void HandleRemove(const base::ListValue& args);
  void HandleClear(const base::ListValue& args);
  void HandleOpenSettings(const base::ListValue& args);

  raw_ptr<Profile> profile_;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<tab_organizer::TabArchive,
                          tab_organizer::TabArchive::Observer>
      observation_{this};
  base::OneShotTimer changed_timer_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_ARCHIVE_HANDLER_H_
