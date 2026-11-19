// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/download_notifications.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/notifications/notification_display_service_factory.h"
#include "chrome/browser/notifications/notification_display_service_impl.h"
#include "chrome/browser/notifications/notification_handler.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/common/notifications/notification_operation.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "ui/base/base_window.h"
#include "ui/base/models/image_model.h"
#include "ui/message_center/public/cpp/notification.h"
#include "ui/message_center/public/cpp/notifier_id.h"
#include "url/gurl.h"

namespace starry {

namespace {

// 见头文件：借用只在 ChromeOS 上注册的类型。
constexpr NotificationHandler::Type kNotificationType =
    NotificationHandler::Type::NEARBY_SHARE;
constexpr char kIdPrefix[] = "starry.download.";
constexpr char kNotifierId[] = "starry.downloads";

std::string& LastDecision() {
  static base::NoDestructor<std::string> decision;
  return *decision;
}

// 点了通知：文件还在就在访达中显示，不然打开下载页。
void RevealOrOpenPage(Profile* profile, const std::string& guid) {
  DownloadService* service = DownloadService::GetIfRunning(profile);
  Download* download = service ? service->Find(guid) : nullptr;
  const base::FilePath path =
      download && download->State() == DownloadState::kComplete
          ? download->TargetPath()
          : base::FilePath();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(
          [](const base::FilePath& path) {
            return !path.empty() && base::PathExists(path);
          },
          path),
      base::BindOnce(
          [](base::WeakPtr<Profile> profile, const base::FilePath& path,
             bool exists) {
            if (!profile) {
              return;
            }
            if (exists) {
              platform_util::ShowItemInFolder(profile.get(), path);
              return;
            }
            ShowSingletonTabOverwritingNTP(profile.get(),
                                           GURL(chrome::kChromeUIDownloadsURL),
                                           NavigateParams::IGNORE_AND_NAVIGATE);
          },
          profile->GetWeakPtr(), path));
}

class DownloadNotificationHandler : public NotificationHandler {
 public:
  DownloadNotificationHandler() = default;
  DownloadNotificationHandler(const DownloadNotificationHandler&) = delete;
  DownloadNotificationHandler& operator=(const DownloadNotificationHandler&) =
      delete;
  ~DownloadNotificationHandler() override = default;

  // NotificationHandler:
  void OnClick(Profile* profile,
               const GURL& origin,
               const std::string& notification_id,
               const std::optional<int>& action_index,
               const std::optional<std::u16string>& reply,
               base::OnceClosure completed_closure) override {
    if (NotificationDisplayService* display =
            NotificationDisplayServiceFactory::GetForProfile(profile)) {
      display->Close(kNotificationType, notification_id);
    }
    if (notification_id.starts_with(kIdPrefix)) {
      RevealOrOpenPage(profile,
                       notification_id.substr(std::size(kIdPrefix) - 1));
    }
    std::move(completed_closure).Run();
  }
};

void EnsureHandler(Profile* profile) {
  NotificationDisplayServiceImpl* display =
      NotificationDisplayServiceImpl::GetForProfile(profile);
  if (display && !display->GetNotificationHandler(kNotificationType)) {
    display->AddNotificationHandler(
        kNotificationType, std::make_unique<DownloadNotificationHandler>());
  }
}

// 有一个浏览器窗口在前面（浏览器在前台）。
bool AnyBrowserWindowActive() {
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetWindow() && browser->GetWindow()->IsActive()) {
      return true;
    }
  }
  return false;
}

}  // namespace

DownloadNotifications::DownloadNotifications(DownloadService* service)
    : service_(service) {
  // 上次启动发的通知这次点了，也要有人处理。
  EnsureHandler(service_->profile());
  observation_.Observe(service_);
  for (Download* download : service_->Downloads()) {
    OnDownloadAdded(download);
  }
}

DownloadNotifications::~DownloadNotifications() = default;

// static
std::string DownloadNotifications::LastDecisionForTesting() {
  return LastDecision();
}

// static
void DownloadNotifications::ClickForTesting(Profile* profile,
                                            const std::string& guid) {
  if (NotificationDisplayServiceImpl* display =
          NotificationDisplayServiceImpl::GetForProfile(profile)) {
    display->ProcessNotificationOperation(
        NotificationOperation::kClick, kNotificationType, GURL(),
        base::StrCat({kIdPrefix, guid}), std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, base::DoNothing());
  }
}

void DownloadNotifications::OnDownloadAdded(Download* download) {
  if (IsActive(download->State())) {
    active_.insert(download->Guid());
  }
}

void DownloadNotifications::OnDownloadUpdated(Download* download) {
  if (IsActive(download->State())) {
    active_.insert(download->Guid());
    return;
  }
  auto it = active_.find(download->Guid());
  if (it == active_.end()) {
    return;
  }
  active_.erase(it);
  if (download->State() == DownloadState::kComplete) {
    Finished(download);
  }
}

void DownloadNotifications::OnDownloadRemoved(const std::string& guid) {
  active_.erase(guid);
}

void DownloadNotifications::OnDownloadServiceShutdown() {
  observation_.Reset();
}

void DownloadNotifications::Finished(Download* download) {
  Profile* profile = service_->profile();
  const std::string name = base::UTF16ToUTF8(download->Filename());
  if (!prefs::PrefsFor(profile)->GetBoolean(
          prefs::kDownloadNotifyOnCompletion)) {
    LastDecision() = name + " off";
    return;
  }
  if (AnyBrowserWindowActive()) {
    LastDecision() = name + " foreground";
    return;
  }
  NotificationDisplayService* display =
      NotificationDisplayServiceFactory::GetForProfile(profile);
  if (!display) {
    LastDecision() = name + " unavailable";
    return;
  }
  EnsureHandler(profile);
  message_center::Notification notification(
      message_center::NOTIFICATION_TYPE_SIMPLE,
      base::StrCat({kIdPrefix, download->Guid()}),
      L("extensions.notification.downloadFinished"), download->Filename(),
      ui::ImageModel(), std::u16string(), GURL(),
      message_center::NotifierId(message_center::NotifierType::SYSTEM_COMPONENT,
                                 kNotifierId),
      message_center::RichNotificationData(), nullptr);
  display->Display(kNotificationType, notification, /*metadata=*/nullptr);
  LastDecision() = name + " shown";
  VLOG(1) << "[starry] download finished notification " << name;
}

}  // namespace starry
