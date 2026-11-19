// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_DOWNLOADS_CHROME_DOWNLOAD_BRIDGE_H_
#define STARRY_BROWSER_DOWNLOADS_CHROME_DOWNLOAD_BRIDGE_H_

class Profile;

namespace download {
class DownloadItem;
}

namespace starry {

class StarryDownloadItem;

// Starry 自己下载的接进 Chrome 里只认 download::DownloadItem 的地方（Starry 的一项在那里是
// StarryDownloadItem）。上游补丁调的函数（starry_hooks.h）也在这里实现。
//
// - 扩展的 chrome.downloads：按编号找、搜索时加上 Starry 的，暂停、继续、取消、去掉记录、
//   删文件、打开、在访达中显示都转给 StarryDownload；onCreated、onChanged、onErased 交给这个
//   用户资料的 ExtensionDownloadsEventRouter 发。Chrome 的一项交给 Starry 时扩展看到的还是
//   同一个下载（同一个编号，不发取消、去掉记录的事件）。交回 Chrome 时是去掉了一项、新开始
//   一项。启动时读回来的记录发 onCreated（同 Chrome 读回的下载记录）。
// - DownloadStatusUpdater：程序坞图标上的下载个数和总进度（Windows 是任务栏），macOS 上
//   访达里临时文件上的进度（可以在那里取消）、下完时程序坞里的下载文件夹跳一下。
// - 关最后一个无痕窗口、退出时数要确认的下载：加上 Starry 会丢掉的（无痕的没下完的）。

// Chrome 的一项交给了 Starry（`replacement` 已经用了它的编号）：扩展不再看到 Chrome 那一项
// （之后的取消、去掉记录不发事件），它在扩展事件里的状态（上次报过的各个字段）搬到 Starry
// 的那一项上，之后的 onChanged 照常只报变了的字段。
void ReplaceForExtensions(download::DownloadItem* chrome_item,
                          StarryDownloadItem* replacement);

// Starry 的一项有了编号：开始算进程序坞图标。`announce`：给扩展发 onCreated（从 Chrome
// 交过来的不发，扩展已经看到过）。
void ItemStarted(Profile* profile, StarryDownloadItem* item, bool announce);
// 状态、进度变了。`extensions`：给扩展发 onChanged（用户资料关闭时不发）。
void ItemUpdated(Profile* profile, StarryDownloadItem* item, bool extensions);
// 从列表里去掉了：给扩展发 onErased。
void ItemRemoved(Profile* profile, StarryDownloadItem* item);

}  // namespace starry

#endif  // STARRY_BROWSER_DOWNLOADS_CHROME_DOWNLOAD_BRIDGE_H_
