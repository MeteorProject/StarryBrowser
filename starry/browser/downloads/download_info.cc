// Copyright 2026 The Starry Browser Authors

#include "starry/browser/downloads/download_info.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/current_thread.h"
#include "chrome/browser/download/drag_download_item.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "net/http/http_status_code.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/downloads/http_downloader.h"
#include "starry/browser/l10n/l10n.h"
#include "ui/base/clipboard/file_info.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/mojom/drag_drop_types.mojom-shared.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/button_drag_utils.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

std::u16string Separator() {
  return L("downloads.status.separator");
}

std::u16string Amount(const Download& download) {
  if (download.TotalBytes() > 0) {
    return base::StrCat({FormatBytes(download.ReceivedBytes()), u" / ",
                         FormatBytes(download.TotalBytes())});
  }
  return FormatBytes(download.ReceivedBytes());
}

std::u16string HttpStatusText(int status) {
  switch (status) {
    case 401:
    case 407:
      return L("downloads.error.http.auth", {{"status", status}});
    case 403:
      return L("downloads.error.http.forbidden");
    case 404:
    case 410:
      return L("downloads.error.http.notFound", {{"status", status}});
    case 429:
      return L("downloads.error.http.tooManyRequests");
    default:
      if (status >= 500 && status < 600) {
        return L("downloads.error.http.server", {{"status", status}});
      }
      return L("downloads.error.http.other",
               {{"status", status},
                {"reason", net::GetHttpReasonPhrase(status, "")}});
  }
}

}  // namespace

bool ShouldShowDownload(const download::DownloadItem& item) {
  return !item.IsTransient() && !item.IsTemporary();
}

std::string DisplayHost(const GURL& url) {
  std::string host(url.host());
  if (host.starts_with("www.")) {
    host = host.substr(4);
  }
  return host;
}

std::u16string DownloadErrorText(const DownloadError& error) {
  switch (error.kind) {
    case DownloadError::Kind::kNone:
      return std::u16string();
    case DownloadError::Kind::kCreateFailed:
      return L("downloads.error.createFailed", {{"reason", error.detail}});
    case DownloadError::Kind::kWriteFailed:
      return L("downloads.error.writeFailed", {{"reason", error.detail}});
    case DownloadError::Kind::kDiskFull:
      return L("downloads.error.diskFull");
    case DownloadError::Kind::kDiskSpace:
      return L("downloads.error.diskSpaceDetail",
               {{"needed", FormatBytes(error.needed)},
                {"available", FormatBytes(error.available)}});
    case DownloadError::Kind::kInvalidResponse:
      return L("downloads.error.invalidResponse");
    case DownloadError::Kind::kConnectionClosed:
      return L("downloads.error.connectionClosed");
    case DownloadError::Kind::kHttpStatus:
      return HttpStatusText(error.status);
    case DownloadError::Kind::kFileChanged:
      return L("downloads.error.fileChanged");
    case DownloadError::Kind::kSaveFailed:
      return error.detail.empty()
                 ? L("downloads.error.cannotSave")
                 : L("downloads.error.saveFailed", {{"reason", error.detail}});
    case DownloadError::Kind::kNoDestination:
      return L("downloads.error.noDestination");
    case DownloadError::Kind::kInterrupted:
      return L("downloads.error.interrupted");
  }
}

std::u16string ChromeErrorText(const download::DownloadItem& item) {
  if (item.GetState() != download::DownloadItem::INTERRUPTED) {
    return std::u16string();
  }
  switch (item.GetLastReason()) {
    case download::DOWNLOAD_INTERRUPT_REASON_NONE:
      return std::u16string();
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE:
      return L("downloads.error.diskFull");
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_ACCESS_DENIED:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_NAME_TOO_LONG:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_TOO_LARGE:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_TRANSIENT_ERROR:
      return L("downloads.error.cannotSave");
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_VIRUS_INFECTED:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_SECURITY_CHECK_FAILED:
      return L("downloads.error.blocked");
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_HASH_MISMATCH:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_SAME_AS_SOURCE:
      return L("downloads.error.fileChanged");
    case download::DOWNLOAD_INTERRUPT_REASON_NETWORK_FAILED:
    case download::DOWNLOAD_INTERRUPT_REASON_NETWORK_TIMEOUT:
    case download::DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED:
    case download::DOWNLOAD_INTERRUPT_REASON_NETWORK_SERVER_DOWN:
      return L("downloads.error.connectionClosed");
    case download::DOWNLOAD_INTERRUPT_REASON_NETWORK_INVALID_REQUEST:
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_NO_RANGE:
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_CONTENT_LENGTH_MISMATCH:
      return L("downloads.error.invalidResponse");
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT:
      return L("downloads.error.notFound");
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED:
      return L("downloads.error.unauthorized");
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN:
      return L("downloads.error.http.forbidden");
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED:
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_UNREACHABLE:
    case download::DOWNLOAD_INTERRUPT_REASON_SERVER_CERT_PROBLEM:
      return L("downloads.error.server");
    case download::DOWNLOAD_INTERRUPT_REASON_USER_SHUTDOWN:
      return L("downloads.error.interruptedOnQuit");
    case download::DOWNLOAD_INTERRUPT_REASON_CRASH:
      return L("downloads.error.interrupted");
    default:
      return L("downloads.error.failed");
  }
}

bool ChromeDownloadResumable(const download::DownloadItem& item) {
  return !item.GetETag().empty() || !item.GetLastModifiedTime().empty();
}

std::u16string FormatBytes(int64_t bytes) {
  const double value = std::max<int64_t>(0, bytes);
  if (value < 1000) {
    return L("downloads.size.bytes",
             {{"count", static_cast<int64_t>(std::round(value))}});
  }
  static constexpr auto kUnits =
      std::to_array<const char*>({"KB", "MB", "GB", "TB"});
  double scaled = value / 1000;
  size_t unit = 0;
  while (scaled >= 1000 && unit < kUnits.size() - 1) {
    scaled /= 1000;
    ++unit;
  }
  const int digits = unit == 0 ? 0 : unit == 1 ? 1 : 2;
  return base::UTF8ToUTF16(
      base::StringPrintf("%.*f %s", digits, scaled, kUnits[unit]));
}

std::u16string FormatSpeed(int64_t bytes_per_second) {
  return base::StrCat({FormatBytes(bytes_per_second), u"/s"});
}

std::u16string FormatDuration(double seconds) {
  const int64_t total = static_cast<int64_t>(std::ceil(seconds));
  if (total < 60) {
    return L("downloads.duration.seconds",
             {{"count", std::max<int64_t>(total, 1)}});
  }
  if (total < 3600) {
    return L("downloads.duration.minutes", {{"count", (total + 30) / 60}});
  }
  const int64_t hours = total / 3600;
  const int64_t minutes = (total % 3600) / 60;
  if (minutes == 0) {
    return L("downloads.duration.hours", {{"count", hours}});
  }
  return L("downloads.duration.hoursMinutes",
           {{"hours", hours}, {"minutes", minutes}});
}

std::u16string DownloadStatusText(const Download& download) {
  if (download.IsDangerous()) {
    return L("downloads.status.dangerous");
  }
  const int64_t received = download.ReceivedBytes();
  const int64_t total = download.TotalBytes();
  switch (download.State()) {
    case DownloadState::kQueued:
      return received > 0 ? L("downloads.status.queuedAmount",
                              {{"amount", Amount(download)}})
                          : L("downloads.status.queued");
    case DownloadState::kRunning: {
      if (const std::optional<int> attempt = download.RetryAttempt()) {
        return L("downloads.status.retrying",
                 {{"attempt", *attempt}, {"max", kMaxRetryAttempts}});
      }
      const int64_t speed = download.Speed();
      if (download.IsConnecting() || (received == 0 && speed == 0)) {
        return L("downloads.status.connecting");
      }
      std::u16string text = Amount(download);
      if (speed > 0) {
        base::StrAppend(&text, {Separator(), FormatSpeed(speed)});
        if (total > 0) {
          base::StrAppend(
              &text, {Separator(),
                      L("downloads.status.remaining",
                        {{"time",
                          FormatDuration(static_cast<double>(total - received) /
                                         speed)}})});
        }
      }
      return text;
    }
    case DownloadState::kPaused:
      if (download.CanResume()) {
        return received > 0 ? L("downloads.status.pausedAmount",
                                {{"amount", Amount(download)}})
                            : L("downloads.status.paused");
      }
      return received > 0 ? L("downloads.status.pausedAmountRestart",
                              {{"amount", Amount(download)}})
                          : L("downloads.status.pausedRestart");
    case DownloadState::kComplete: {
      if (download.FileMissing()) {
        return L("downloads.status.fileMissing");
      }
      std::u16string text = FormatBytes(total > 0 ? total : received);
      const std::string host = DisplayHost(download.Url());
      if (!host.empty()) {
        base::StrAppend(&text, {Separator(), base::UTF8ToUTF16(host)});
      }
      return text;
    }
    case DownloadState::kCancelled:
      return L("downloads.status.cancelled");
    case DownloadState::kInterrupted: {
      const std::u16string error = download.ErrorText();
      return error.empty() ? L("downloads.error.failed") : error;
    }
  }
}

std::optional<double> DownloadProgress(const Download& download) {
  if (download.TotalBytes() <= 0) {
    return std::nullopt;
  }
  return std::clamp(static_cast<double>(download.ReceivedBytes()) /
                        static_cast<double>(download.TotalBytes()),
                    0.0, 1.0);
}

std::u16string ConnectionSummary(const Download& download) {
  if (const download::DownloadItem* item = download.chrome_item()) {
    return ChromeDownloadResumable(*item)
               ? L("downloads.connection.chromeResumable")
               : L("downloads.connection.chromeNotResumable");
  }
  const size_t segments = download.Segments().size();
  switch (download.GetRangeSupport()) {
    case RangeSupport::kSupported:
      if (download.State() == DownloadState::kRunning) {
        return L("downloads.connection.segmentedActive",
                 {{"connections", download.Connections()},
                  {"max", download.MaxConnections()},
                  {"count", segments}});
      }
      return L("downloads.connection.segmented", {{"count", segments}});
    case RangeSupport::kUnsupported:
      return L("downloads.connection.unsupported");
    case RangeSupport::kUnknown:
      return L("downloads.connection.checking");
  }
}

void DragDownload(Download& download,
                  const gfx::Image* icon,
                  gfx::NativeView view) {
  if (download.State() != DownloadState::kComplete || download.FileMissing() ||
      !view) {
    return;
  }
  // 拖动在系统的拖放循环里同步进行，期间允许嵌套的任务（拖进网页时要用）。
  base::CurrentThread::ScopedAllowApplicationTasksInNativeNestedLoop allow;
  if (download::DownloadItem* item = download.chrome_item()) {
    DragDownloadItem(item, icon, view);
    return;
  }
  views::Widget* widget = views::Widget::GetWidgetForNativeView(view);
  if (!widget) {
    return;
  }
  auto data = std::make_unique<ui::OSExchangeData>();
  button_drag_utils::SetDragImage(
      GURL(), download.Filename(),
      icon && !icon->IsEmpty() ? icon->AsImageSkia() : gfx::ImageSkia(),
      nullptr, data.get());
  const base::FilePath path = download.TargetPath();
  data->SetFilenames({ui::FileInfo(path, path.BaseName())});
  widget->RunDragDropLoop(
      nullptr, std::move(data), gfx::Point(),
      ui::DragDropTypes::DRAG_COPY | ui::DragDropTypes::DRAG_LINK,
      ui::mojom::DragEventSource::kMouse);
}

}  // namespace starry
