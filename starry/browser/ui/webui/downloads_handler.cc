// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/downloads_handler.h"

#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/icon_manager.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/downloads/download_info.h"
#include "ui/display/screen.h"
#include "url/gurl.h"

namespace starry {

namespace {

constexpr base::TimeDelta kChangedDelay = base::Milliseconds(250);

double TimeValue(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

base::DictValue DescribeItem(const Download& item) {
  base::DictValue dict;
  dict.Set("id", item.Guid());
  dict.Set("filename", item.Filename());
  dict.Set("url", item.Url().spec());
  const GURL referrer = item.ReferrerUrl();
  dict.Set("referrer", referrer.is_valid() ? referrer.spec() : std::string());
  dict.Set("host", DisplayHost(item.Url()));
  dict.Set("state", StateName(item.State()));
  dict.Set("received", static_cast<double>(item.ReceivedBytes()));
  dict.Set("total", static_cast<double>(item.TotalBytes()));
  dict.Set("speed", static_cast<double>(item.Speed()));
  dict.Set("startTime", TimeValue(item.StartTime()));
  dict.Set("endTime", TimeValue(item.EndTime()));
  dict.Set("path", item.TargetPath().AsUTF8Unsafe());
  dict.Set("fileExists", !item.FileMissing());
  dict.Set("dangerous", item.IsDangerous());
  dict.Set("canResume", item.CanResume());
  dict.Set("error", item.ErrorText());
  dict.Set("status", DownloadStatusText(item));
  dict.Set("method", ConnectionSummary(item));
  dict.Set("starry", item.IsStarry());
  dict.Set("rangeSupport", RangeSupportName(item.GetRangeSupport()));
  dict.Set("resumable",
           item.IsStarry()
               ? item.GetRangeSupport() != RangeSupport::kUnsupported
               : (item.chrome_item() &&
                  ChromeDownloadResumable(*item.chrome_item())));
  base::ListValue segments;
  for (const DownloadSegment& segment : item.Segments()) {
    base::ListValue entry;
    entry.Append(static_cast<double>(segment.start));
    entry.Append(segment.end == DownloadSegment::kOpenEnd
                     ? -1.0
                     : static_cast<double>(segment.end));
    entry.Append(static_cast<double>(segment.received));
    segments.Append(std::move(entry));
  }
  dict.Set("segments", std::move(segments));
  dict.Set("connections", item.Connections());
  dict.Set("maxConnections", item.MaxConnections());
  if (const std::optional<int> attempt = item.RetryAttempt()) {
    dict.Set("retryAttempt", *attempt);
  }
  dict.Set("connecting", item.IsConnecting());
  return dict;
}

}  // namespace

DownloadsHandler::DownloadsHandler(Profile* profile)
    : profile_(profile), service_(DownloadService::Get(profile)) {
  if (service_) {
    observation_.Observe(service_);
  }
}

DownloadsHandler::~DownloadsHandler() = default;

void DownloadsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getDownloads",
      base::BindRepeating(&DownloadsHandler::HandleGetDownloads,
                          base::Unretained(this)));
  for (const char* action : {"pause", "resume", "cancel", "retry", "open",
                             "showInFolder", "remove", "keep", "discard"}) {
    web_ui()->RegisterMessageCallback(
        action, base::BindRepeating(&DownloadsHandler::HandleItemAction,
                                    base::Unretained(this), action));
  }
  web_ui()->RegisterMessageCallback(
      "clearInactive",
      base::BindRepeating(&DownloadsHandler::HandleClearInactive,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "clearAll", base::BindRepeating(&DownloadsHandler::HandleClearAll,
                                      base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "openFolder", base::BindRepeating(&DownloadsHandler::HandleOpenFolder,
                                        base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "drag", base::BindRepeating(&DownloadsHandler::HandleDrag,
                                  base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "newDownload", base::BindRepeating(&DownloadsHandler::HandleNewDownload,
                                         base::Unretained(this)));
}

void DownloadsHandler::OnJavascriptAllowed() {}

void DownloadsHandler::OnJavascriptDisallowed() {
  changed_timer_.Stop();
}

void DownloadsHandler::OnDownloadAdded(Download* download) {
  ScheduleChanged();
}

void DownloadsHandler::OnDownloadUpdated(Download* download) {
  ScheduleChanged();
}

void DownloadsHandler::OnDownloadRemoved(const std::string& guid) {
  ScheduleChanged();
}

void DownloadsHandler::OnDownloadServiceShutdown() {
  observation_.Reset();
  service_ = nullptr;
}

base::ListValue DownloadsHandler::ListDownloads() {
  base::ListValue list;
  if (!service_) {
    return list;
  }
  for (const Download* item : service_->Downloads()) {
    list.Append(DescribeItem(*item));
  }
  return list;
}

Download* DownloadsHandler::FindItem(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string() || !service_) {
    return nullptr;
  }
  return service_->Find(args[0].GetString());
}

void DownloadsHandler::ScheduleChanged() {
  if (!IsJavascriptAllowed() || changed_timer_.IsRunning()) {
    return;
  }
  changed_timer_.Start(FROM_HERE, kChangedDelay,
                       base::BindOnce(&DownloadsHandler::SendChanged,
                                      base::Unretained(this)));
}

void DownloadsHandler::SendChanged() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("downloads-changed", ListDownloads());
  }
}

void DownloadsHandler::HandleGetDownloads(const base::ListValue& args) {
  AllowJavascript();
  if (service_) {
    service_->CheckFilesExist();
  }
  base::DictValue reply;
  reply.Set("downloads", ListDownloads());
  reply.Set("incognito", profile_->IsOffTheRecord());
  ResolveJavascriptCallback(args[0], base::Value(std::move(reply)));
}

void DownloadsHandler::HandleItemAction(const std::string& action,
                                        const base::ListValue& args) {
  Download* item = FindItem(args);
  if (!item) {
    return;
  }
  if (action == "pause") {
    item->Pause();
  } else if (action == "resume") {
    item->Resume();
  } else if (action == "cancel") {
    item->Cancel();
  } else if (action == "retry") {
    service_->Retry(item, web_ui()->GetWebContents());
  } else if (action == "open") {
    item->Open();
  } else if (action == "showInFolder") {
    platform_util::ShowItemInFolder(profile_, item->RevealPath());
  } else if (action == "remove" || action == "discard") {
    item->Remove();
  } else if (action == "keep") {
    item->KeepDangerous();
  }
}

void DownloadsHandler::HandleClearInactive(const base::ListValue& args) {
  if (service_) {
    service_->ClearInactive();
  }
}

void DownloadsHandler::HandleClearAll(const base::ListValue& args) {
  // 取消进行中的下载；已下载的文件保留在磁盘上。
  if (service_) {
    service_->ClearAll();
  }
}

void DownloadsHandler::HandleOpenFolder(const base::ListValue& args) {
  DownloadPrefs* prefs = DownloadPrefs::FromBrowserContext(profile_);
  if (!prefs) {
    return;
  }
  platform_util::OpenItem(profile_, prefs->DownloadPath(),
                          platform_util::OPEN_FOLDER,
                          platform_util::OpenOperationCallback());
}

void DownloadsHandler::HandleDrag(const base::ListValue& args) {
  Download* item = FindItem(args);
  content::WebContents* contents = web_ui()->GetWebContents();
  if (!item || !contents) {
    return;
  }
  // 拖出去的是文件本身（同 Chrome 的下载页）；图标用系统的文件图标。
  gfx::NativeView view = contents->GetNativeView();
  const gfx::Image* icon =
      g_browser_process->icon_manager()->LookupIconFromFilepath(
          item->TargetPath(), IconLoader::NORMAL,
          display::Screen::Get()->GetPreferredScaleFactorForView(view).value_or(
              1.0f));
  DragDownload(*item, icon, view);
}

void DownloadsHandler::HandleNewDownload(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string() || !service_) {
    return;
  }
  GURL url(args[0].GetString());
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  service_->StartDownload(url, web_ui()->GetWebContents());
}

}  // namespace starry
