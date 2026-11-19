// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_IMPORTER_CHROMIUM_BROWSERS_H_
#define STARRY_BROWSER_IMPORTER_CHROMIUM_BROWSERS_H_

#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "starry/browser/importer/import_source.h"

namespace starry {

// 一个 Chromium 系浏览器的描述：用户数据目录位置和密码密钥信息。
struct ChromiumBrowser {
  const char* id;
  // 品牌名，不翻译。
  const char* name;
  // 数据目录（Local State 所在），相对各平台放应用数据的目录，用 / 分开；空的是这个
  // 平台上没有：macOS 是 ~/Library/Application Support，Windows 是 %LOCALAPPDATA%
  // （windows_roaming 时是 %APPDATA%），Linux 是 ~/.config（$XDG_CONFIG_HOME）。
  const char* mac;
  const char* windows;
  bool windows_roaming;
  const char* linux;
  // macOS 的钥匙串里放密码密钥的项目（它的 os_crypt 用的「Chrome Safe Storage」/
  // 「Chrome」这种）。
  const char* keychain_service;
  const char* keychain_account;
  // 密码能不能导入：Yandex 用自己的加密，不是 Chromium 的 os_crypt。
  bool passwords;
};

base::span<const ChromiumBrowser> ChromiumBrowsers();

std::optional<base::FilePath> ImportHomeForTesting();

// 找本机这几个浏览器的用户资料（读 Local State 的 profile.info_cache），只留下有东西可
// 导入的。会读文件：在能阻塞的线程上调用。
std::vector<ImportSource> DetectChromiumSources();

// 用户资料里各样数据的文件。
inline constexpr char kBookmarksFile[] = "Bookmarks";
// 登录了 Google 账号、书签存在账号里时的第二份（Chrome 2024 年起）。
inline constexpr char kAccountBookmarksFile[] = "AccountBookmarks";
inline constexpr char kHistoryFile[] = "History";
inline constexpr char kLoginDataFile[] = "Login Data";
inline constexpr char kAccountLoginDataFile[] = "Login Data For Account";
// 搜索引擎（keywords 表）和表单的自动填充（autofill 表）。
inline constexpr char kWebDataFile[] = "Web Data";
inline constexpr char kLocalStateFile[] = "Local State";

}  // namespace starry

#endif  // STARRY_BROWSER_IMPORTER_CHROMIUM_BROWSERS_H_
