// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_EXTENSIONS_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_EXTENSIONS_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/ui/extensions/extension_enable_flow_delegate.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_prefs_observer.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/webstore_install_result.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class ExtensionEnableFlow;
class Profile;

namespace extensions {
class CrxInstallError;
class Extension;
}  // namespace extensions

namespace starry {

// 设置页「扩展」分类的数据和操作。chrome://extensions 也会重定向到这里（见
// starry_content_browser_client.cc），替代 Chrome 内置扩展页。
//
// 页面发来的消息：
//   getExtensions(callback)                    → 扩展列表（见 Describe）
//   extensionAction(id, action[, value])       → enable、disable、uninstall、
//       options、reload、restart、approve、inspect、pin、incognito、fileAccess、
//       userScripts、siteAccess
//   setShortcut(callback, id, command, keys)   → 设快捷键（keys 为空时清除），回
//       新的显示文字或错误
//   suspendShortcuts(bool)                     → 录快捷键时先停掉扩展的快捷键
//   addExtension(kind[, value])                → unpacked、crx、store（应用店的
//       链接或扩展 ID）
//   checkUpdates(callback)                     → 检查更新，回 {updated}
// 发给页面的事件：extensions-changed（列表）、extensions-notice（{text, error}）。
class ExtensionsHandler : public content::WebUIMessageHandler,
                          public extensions::ExtensionRegistryObserver,
                          public extensions::ExtensionPrefsObserver,
                          public ToolbarActionsModel::Observer,
                          public ui::SelectFileDialog::Listener,
                          public ExtensionEnableFlowDelegate {
 public:
  explicit ExtensionsHandler(Profile* profile);
  ExtensionsHandler(const ExtensionsHandler&) = delete;
  ExtensionsHandler& operator=(const ExtensionsHandler&) = delete;
  ~ExtensionsHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // extensions::ExtensionRegistryObserver:
  void OnExtensionLoaded(content::BrowserContext* browser_context,
                         const extensions::Extension* extension) override;
  void OnExtensionUnloaded(content::BrowserContext* browser_context,
                           const extensions::Extension* extension,
                           extensions::UnloadedExtensionReason reason) override;
  void OnExtensionInstalled(content::BrowserContext* browser_context,
                            const extensions::Extension* extension,
                            bool is_update) override;
  void OnExtensionUninstalled(content::BrowserContext* browser_context,
                              const extensions::Extension* extension,
                              extensions::UninstallReason reason) override;

  // extensions::ExtensionPrefsObserver:
  void OnExtensionDisableReasonsChanged(
      const extensions::ExtensionId& extension_id,
      extensions::DisableReasonSet disabled_reasons) override;
  void OnExtensionPrefsUpdated(
      const extensions::ExtensionId& extension_id) override;
  void OnExtensionRuntimePermissionsChanged(
      const extensions::ExtensionId& extension_id) override;

  // ToolbarActionsModel::Observer:
  void OnToolbarActionAdded(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarActionRemoved(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarActionUpdated(const ToolbarActionsModel::ActionId& id) override;
  void OnToolbarModelInitialized() override;
  void OnToolbarPinnedActionsChanged() override;

  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

  // ExtensionEnableFlowDelegate:
  void ExtensionEnableFlowFinished() override;
  void ExtensionEnableFlowAborted(bool user_initiated) override;

 private:
  enum class FileKind { kNone, kUnpacked, kCrx };

  void HandleGetExtensions(const base::ListValue& args);
  void HandleExtensionAction(const base::ListValue& args);
  void HandleSetShortcut(const base::ListValue& args);
  void HandleSuspendShortcuts(const base::ListValue& args);
  void HandleAddExtension(const base::ListValue& args);
  void HandleCheckUpdates(const base::ListValue& args);

  base::ListValue List();
  base::DictValue Describe(const extensions::Extension& extension);
  void ScheduleChanged();
  void Notice(const std::u16string& text, bool error);
  void ChooseFile(FileKind kind);
  void LoadUnpacked(const base::FilePath& path);
  void InstallCrx(const base::FilePath& path);
  void InstallFromStore(const std::string& input);
  void OnUnpackedLoaded(const extensions::Extension* extension,
                        const base::FilePath& path,
                        const std::u16string& error);
  void OnCrxInstalled(
      const std::u16string& file_name,
      const std::optional<extensions::CrxInstallError>& error);
  void OnStoreInstalled(bool success,
                        const std::string& error,
                        extensions::webstore_install::Result result);
  void OnUpdatesChecked(std::string callback_id,
                        std::map<std::string, std::string> versions_before);

  const raw_ptr<Profile> profile_;
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
  base::ScopedObservation<extensions::ExtensionPrefs,
                          extensions::ExtensionPrefsObserver>
      prefs_observation_{this};
  base::ScopedObservation<ToolbarActionsModel, ToolbarActionsModel::Observer>
      toolbar_observation_{this};
  // 快捷键存在 Chrome 的配置里（extensions.commands）。
  PrefChangeRegistrar pref_registrar_;
  scoped_refptr<ui::SelectFileDialog> select_file_dialog_;
  FileKind file_kind_ = FileKind::kNone;
  std::unique_ptr<ExtensionEnableFlow> enable_flow_;
  bool shortcuts_suspended_ = false;
  base::OneShotTimer changed_timer_;
  base::WeakPtrFactory<ExtensionsHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_EXTENSIONS_HANDLER_H_
