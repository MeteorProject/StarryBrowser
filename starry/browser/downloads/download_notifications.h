// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_DOWNLOAD_NOTIFICATIONS_H_
#define STARRY_BROWSER_DOWNLOADS_DOWNLOAD_NOTIFICATIONS_H_

#include <functional>
#include <set>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "starry/browser/downloads/download_service.h"

class Profile;

namespace starry {

class Download;

// 下载完成时的系统通知：设置里开着「下载完成时通知」、浏览器在后台（没有一个浏览器窗口在
// 前面）时发；无痕的不发（不建这个对象）。Chrome 下载的、Starry 下载的都算，只算这一次启动
// 里从进行中变成完成的。点通知在访达中显示下好的文件，文件不在了（或者记录已经去掉）就打开
// 下载页。
//
// 走 Chrome 的通知服务（NotificationDisplayService），显示成系统通知：macOS 的通知中心、
// Windows 的通知、Linux 的桌面通知；系统通知用不了时是 Chrome 自己画的。系统通知点了以后
// 要按 NotificationHandler::Type 找处理的对象（不带类型的 TRANSIENT 只能显示成 Chrome 自己
// 画的），Chrome 没有给浏览器自己的通知留类型：借用只在 ChromeOS 上注册的 NEARBY_SHARE，
// 每个用户资料第一次用时注册 Starry 的处理对象。
class DownloadNotifications : public DownloadService::Observer {
 public:
  explicit DownloadNotifications(DownloadService* service);
  DownloadNotifications(const DownloadNotifications&) = delete;
  DownloadNotifications& operator=(const DownloadNotifications&) = delete;
  ~DownloadNotifications() override;

  // 调试步骤用：最近一次「下载完成」时发没发、为什么。
  static std::string LastDecisionForTesting();
  // 调试步骤用：当作用户点了这个下载的通知（和系统通知点了以后走同一条路）。
  static void ClickForTesting(Profile* profile, const std::string& guid);

  // DownloadService::Observer:
  void OnDownloadAdded(Download* download) override;
  void OnDownloadUpdated(Download* download) override;
  void OnDownloadRemoved(const std::string& guid) override;
  void OnDownloadServiceShutdown() override;

 private:
  void Finished(Download* download);

  const raw_ptr<DownloadService> service_;
  // 进行中的（变成完成时才发）。
  std::set<std::string, std::less<>> active_;
  base::ScopedObservation<DownloadService, DownloadService::Observer>
      observation_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_DOWNLOAD_NOTIFICATIONS_H_
