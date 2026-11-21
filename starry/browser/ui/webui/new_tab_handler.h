// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_NEW_TAB_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_NEW_TAB_HANDLER_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/sessions/core/tab_restore_service_observer.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class BrowserWindowInterface;
class Profile;

namespace starry {

namespace new_tab {
class Shortcuts;
}

// 新标签页（starry://newtab）的 WebUI 消息处理与数据源订阅。
class NewTabHandler : public content::WebUIMessageHandler,
                      public tab_organizer::TabArchive::Observer,
                      public MediaHub::Observer,
                      public sessions::TabRestoreServiceObserver,
                      public ui::SelectFileDialog::Listener {
 public:
  explicit NewTabHandler(Profile* profile);
  NewTabHandler(const NewTabHandler&) = delete;
  NewTabHandler& operator=(const NewTabHandler&) = delete;
  ~NewTabHandler() override;

  static base::FilePath BackgroundPath(Profile* profile);

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // tab_organizer::TabArchive::Observer:
  void OnArchiveChanged() override;
  // MediaHub::Observer:
  void OnMediaChanged() override;
  // sessions::TabRestoreServiceObserver:
  void TabRestoreServiceChanged(sessions::TabRestoreService* service) override;
  void TabRestoreServiceLoaded(sessions::TabRestoreService* service) override;
  void TabRestoreServiceDestroyed(
      sessions::TabRestoreService* service) override;
  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

 private:
  base::DictValue State();
  base::Value MediaState();
  void ScheduleChanged();
  void SendChanged();
  void OnDocked(bool docked);
  BrowserWindowInterface* Browser();

  void HandleGetNewTab(const base::ListValue& args);
  void HandleSearch(const base::ListValue& args);
  void HandleSearchRect(const base::ListValue& args);
  void HandleRestoreClosed(const base::ListValue& args);
  void HandleShortcut(const base::ListValue& args);
  void HandleMedia(const base::ListValue& args);
  void HandleSetNote(const base::ListValue& args);
  void HandleSetWidgets(const base::ListValue& args);
  void HandleSetShortcutRows(const base::ListValue& args);
  void HandleSetBackground(const base::ListValue& args);
  void HandleChooseImage(const base::ListValue& args);
  void HandleOpen(const base::ListValue& args);
  void OnImageCopied(bool ok);

  const raw_ptr<Profile> profile_;
  std::unique_ptr<new_tab::Shortcuts> shortcuts_;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<tab_organizer::TabArchive,
                          tab_organizer::TabArchive::Observer>
      archive_observation_{this};
  base::ScopedObservation<MediaHub, MediaHub::Observer> media_observation_{
      this};
  base::ScopedObservation<sessions::TabRestoreService,
                          sessions::TabRestoreServiceObserver>
      restore_observation_{this};
  base::OneShotTimer changed_timer_;
  scoped_refptr<ui::SelectFileDialog> select_file_dialog_;
  // 封面编码成 data: 地址比较费事：封面没换时用上一次的。
  gfx::ImageSkia artwork_;
  std::string artwork_url_;
  base::WeakPtrFactory<NewTabHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_NEW_TAB_HANDLER_H_
