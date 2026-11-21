// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_UPDATE_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_UPDATE_HANDLER_H_

#include "base/scoped_observation.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/updater/updater_service.h"

namespace starry {

// 设置 › 通用 的「软件更新」分组数据与操作（settings.js 的 general_）。
// 消息：
//   getUpdateState                      → 当前更新状态
//   checkForUpdates                     手动检查更新
//   downloadUpdate                      下载检测到的新版本
//   cancelUpdateDownload                取消正在进行的下载
//   applyUpdate                         重启并更新（Linux 上打开安装包）
//   revealUpdatePackage                 在文件管理器中显示已下载的安装包
//   setAutoUpdateMode [auto|notify|off] 设置自动更新方式
//   setUpdateMirror [url]               → {error}（空串为成功）
// 状态变化时向页面发送 update-changed。
class UpdateHandler : public content::WebUIMessageHandler,
                      public updater::UpdaterService::Observer {
 public:
  UpdateHandler();
  UpdateHandler(const UpdateHandler&) = delete;
  UpdateHandler& operator=(const UpdateHandler&) = delete;
  ~UpdateHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // updater::UpdaterService::Observer:
  void OnUpdateStateChanged() override;

 private:
  base::DictValue State() const;
  void HandleGetState(const base::ListValue& args);
  void HandleCheck(const base::ListValue& args);
  void HandleDownload(const base::ListValue& args);
  void HandleCancelDownload(const base::ListValue& args);
  void HandleApply(const base::ListValue& args);
  void HandleReveal(const base::ListValue& args);
  void HandleSetMode(const base::ListValue& args);
  void HandleSetMirror(const base::ListValue& args);

  base::ScopedObservation<updater::UpdaterService,
                          updater::UpdaterService::Observer>
      observation_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_UPDATE_HANDLER_H_
