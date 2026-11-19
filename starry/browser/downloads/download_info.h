// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_DOWNLOAD_INFO_H_
#define STARRY_BROWSER_DOWNLOADS_DOWNLOAD_INFO_H_

#include <cstdint>
#include <optional>
#include <string>

#include "ui/gfx/native_ui_types.h"
#include "url/gurl.h"

namespace download {
class DownloadItem;
}

namespace gfx {
class Image;
}

namespace starry {

class Download;
struct DownloadError;

// 下载项的文字（大小、速度、剩余时间、状态、失败原因），下载页和弹出框共用。

// 列不列出来：Chrome 内部的临时下载（例如拖出去的文件）不列。
bool ShouldShowDownload(const download::DownloadItem& item);

// 去掉 www. 的主机名。
std::string DisplayHost(const GURL& url);

// 文件大小（按 1000 进位）：「512 字节」「1.2 MB」。
std::u16string FormatBytes(int64_t bytes);
std::u16string FormatSpeed(int64_t bytes_per_second);
// 「3 秒」「2 分钟」「1 小时 5 分」。
std::u16string FormatDuration(double seconds);

// 一行状态：进行中是「12 MB / 30 MB · 2.1 MB/s · 剩余 9 秒」，排队、重试、连接中各有说法，
// 结束的是「30 MB · example.com」或者失败的原因。
std::u16string DownloadStatusText(const Download& download);

// 0…1；不知道总大小时是空的。
std::optional<double> DownloadProgress(const Download& download);

// 下载方式（下载页的详情）：「支持分段下载 · 4/8 个连接 · 8 段」「服务器不支持分段，
// 单线程下载……」「由 Chrome 下载 · 支持断点续传」。
std::u16string ConnectionSummary(const Download& download);
// Chrome 下载的能不能断点续传（服务器给了 ETag 或 Last-Modified）。
bool ChromeDownloadResumable(const download::DownloadItem& item);

// 失败的原因。
std::u16string DownloadErrorText(const DownloadError& error);
std::u16string ChromeErrorText(const download::DownloadItem& item);

// 把下载好的文件拖出去（拖到访达、桌面、别的 App）。在系统的拖放循环里同步进行。
void DragDownload(Download& download,
                  const gfx::Image* icon,
                  gfx::NativeView view);

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_DOWNLOAD_INFO_H_
