// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_SETTINGS_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_SETTINGS_HANDLER_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/shell_integration.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/search_engines/template_url_service.h"
#include "components/search_engines/template_url_service_observer.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class Profile;

namespace starry {

// 设置页（pages/settings/）的数据。设置项都是 Chrome 自己的设置（搜索引擎、启动、Cookie、
// 下载位置……）或者 Starry 的（starry/browser/prefs/）。界面语言和网页时区重启后生效。
// 无痕窗口里打开的也是原来那个用户资料的设置。
//
//   getSettings(callbackId)：回全部设置的当前值，见 settings_handler.cc 的 State()。
//   setSetting(key, value)：改一项（key 同 State() 里的名字）。
//   checkDefaultBrowser()、setDefaultBrowser()：查、设默认浏览器，结果在 settings-changed 里。
//   chooseDownloadFolder()：系统的选文件夹对话框。
//   relaunch()：重新启动（改了界面语言或网页时区之后），恢复窗口和标签页。
//   setOrganizerException(site, add)、clearArchive()：标签管理的不整理的网站、清除归档。
//   setOrganizerRule(site, folder)、forgetOrganizerMemory()：按网站的规则（folder 空的
//   是拿掉）、忘掉学到的整理习惯。
// 「扩展」分类的数据在 ExtensionsHandler（extensions_handler.h）。
// 有变化（这里改的、别处改的）时发 settings-changed（带全部设置）。
class SettingsHandler : public content::WebUIMessageHandler,
                        public TemplateURLServiceObserver,
                        public tab_organizer::TabArchive::Observer,
                        public ui::SelectFileDialog::Listener {
 public:
  explicit SettingsHandler(Profile* profile);
  SettingsHandler(const SettingsHandler&) = delete;
  SettingsHandler& operator=(const SettingsHandler&) = delete;
  ~SettingsHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // TemplateURLServiceObserver:
  void OnTemplateURLServiceChanged() override;

  // tab_organizer::TabArchive::Observer（归档了几个）:
  void OnArchiveChanged() override;

  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

 private:
  base::DictValue State();
  void ScheduleChanged();

  void HandleGetSettings(const base::ListValue& args);
  void HandleSetSetting(const base::ListValue& args);
  void HandleSetFingerprintException(const base::ListValue& args);
  void HandleSetOrganizerException(const base::ListValue& args);
  void HandleSetOrganizerRule(const base::ListValue& args);
  void HandleForgetOrganizerMemory(const base::ListValue& args);
  void HandleClearArchive(const base::ListValue& args);
  void HandleOpenArchive(const base::ListValue& args);
  void HandleCheckDefaultBrowser(const base::ListValue& args);
  void HandleSetDefaultBrowser(const base::ListValue& args);
  void HandleRelaunch(const base::ListValue& args);
  void HandleChooseDownloadFolder(const base::ListValue& args);
  void OnDefaultBrowserState(shell_integration::DefaultWebClientState state);

  const raw_ptr<Profile> profile_;
  PrefChangeRegistrar profile_prefs_;
  PrefChangeRegistrar local_prefs_;
  base::OneShotTimer changed_timer_;
  // 默认浏览器：checking、default、notDefault、unknown。
  std::string default_browser_ = "checking";
  scoped_refptr<shell_integration::DefaultBrowserWorker> default_browser_worker_;
  scoped_refptr<ui::SelectFileDialog> select_folder_dialog_;
  base::ScopedObservation<TemplateURLService, TemplateURLServiceObserver>
      template_url_observation_{this};
  base::ScopedObservation<tab_organizer::TabArchive,
                          tab_organizer::TabArchive::Observer>
      archive_observation_{this};

  base::WeakPtrFactory<SettingsHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_SETTINGS_HANDLER_H_
