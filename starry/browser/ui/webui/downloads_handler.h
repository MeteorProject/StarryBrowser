// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_DOWNLOADS_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_DOWNLOADS_HANDLER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/downloads/download_service.h"

class Profile;

namespace starry {

class Download;

// 下载页（pages/downloads/）的数据：DownloadService 的列表（Chrome 下载的和 Starry
// 自己下载的）。普通窗口和无痕窗口各自维护独立的下载列表。
//
//   getDownloads(callbackId)：回 {downloads, incognito}，downloads 是全部下载，新的在前，每个是
//     {id, filename, url, referrer, host, state, received, total, speed, startTime,
//      endTime, path, fileExists, dangerous, canResume, error, status,
//      starry, rangeSupport, segments, connections, maxConnections, retryAttempt,
//      connecting, method}；
//     state 是 queued、running、paused、complete、cancelled、interrupted；status 是一行状态
//     （同弹出框）；Starry 自己下载的 starry 为 true，segments 是 [[start, end, received]]（end
//     是 -1 时不知道结尾），method 是下载方式的说明。
//   pause / resume / cancel / retry / open / showInFolder / remove(id)
//   keep / discard(id)：可能有危险的下载保留或丢弃。
//   clearInactive()、clearAll()（进行中的取消掉）、openFolder()、newDownload(url)
//   drag(id)：把下载好的文件拖出去（拖到访达、桌面、别的 App）。
// 下载有变化时发 downloads-changed（带全部下载），最多每 250 毫秒一次。
class DownloadsHandler : public content::WebUIMessageHandler,
                         public DownloadService::Observer {
 public:
  explicit DownloadsHandler(Profile* profile);
  DownloadsHandler(const DownloadsHandler&) = delete;
  DownloadsHandler& operator=(const DownloadsHandler&) = delete;
  ~DownloadsHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // DownloadService::Observer:
  void OnDownloadAdded(Download* download) override;
  void OnDownloadUpdated(Download* download) override;
  void OnDownloadRemoved(const std::string& guid) override;
  void OnDownloadServiceShutdown() override;

 private:
  base::ListValue ListDownloads();
  Download* FindItem(const base::ListValue& args);
  void ScheduleChanged();
  void SendChanged();

  void HandleGetDownloads(const base::ListValue& args);
  void HandleItemAction(const std::string& action, const base::ListValue& args);
  void HandleClearInactive(const base::ListValue& args);
  void HandleClearAll(const base::ListValue& args);
  void HandleOpenFolder(const base::ListValue& args);
  void HandleNewDownload(const base::ListValue& args);
  void HandleDrag(const base::ListValue& args);

  const raw_ptr<Profile> profile_;
  raw_ptr<DownloadService> service_ = nullptr;
  base::ScopedObservation<DownloadService, DownloadService::Observer>
      observation_{this};
  base::OneShotTimer changed_timer_;

  base::WeakPtrFactory<DownloadsHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_DOWNLOADS_HANDLER_H_
