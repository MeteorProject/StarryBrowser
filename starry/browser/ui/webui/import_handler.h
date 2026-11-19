// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_IMPORT_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_IMPORT_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/importer/import_service.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class Profile;

namespace starry {

// 设置 › 通用 › 导入浏览器数据（pages/settings/settings.js 的 importData 分类）的数据。
// 导入本身在用户资料上的 ImportService（import_service.h），设置页关了也会做完。
//
//   getImportState(callbackId)：回来源和最近一次导入的进度、结果（见 import_handler.cc
//     的 State()）；第一次时去找来源。
//   detectImportSources()：重新找（比如刚给了「完全磁盘访问权限」）。
//   startImport(sourceId, items)：items 是 {bookmarks, history, passwords, searchEngines,
//     autofill} 的开关。
//   importBookmarksFile()：选一个书签文件（HTML）导入。
//   clearImport()：收起结果。
//   openFullDiskAccess()：macOS 上打开系统设置的「完全磁盘访问权限」。
// 有变化时发 import-changed（带 State()）。
class ImportHandler : public content::WebUIMessageHandler,
                      public ImportService::Observer,
                      public ui::SelectFileDialog::Listener {
 public:
  explicit ImportHandler(Profile* profile);
  ImportHandler(const ImportHandler&) = delete;
  ImportHandler& operator=(const ImportHandler&) = delete;
  ~ImportHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // ImportService::Observer:
  void OnImportChanged() override;

  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

 private:
  base::DictValue State();

  void HandleGetImportState(const base::ListValue& args);
  void HandleDetectImportSources(const base::ListValue& args);
  void HandleStartImport(const base::ListValue& args);
  void HandleImportBookmarksFile(const base::ListValue& args);
  void HandleClearImport(const base::ListValue& args);
  void HandleOpenFullDiskAccess(const base::ListValue& args);

  const raw_ptr<Profile> profile_;
  scoped_refptr<ui::SelectFileDialog> select_file_dialog_;
  base::ScopedObservation<ImportService, ImportService::Observer> observation_{
      this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_IMPORT_HANDLER_H_
