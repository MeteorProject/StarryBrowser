// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_IMPORTER_CHROMIUM_READER_H_
#define STARRY_BROWSER_IMPORTER_CHROMIUM_READER_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "components/autofill/core/browser/webdata/autocomplete/autocomplete_entry.h"
#include "components/history/core/browser/url_row.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/search_engines/template_url_data.h"
#include "components/user_data_importer/common/imported_bookmark_entry.h"
#include "components/user_data_importer/common/importer_data_types.h"

namespace starry {

struct ImportSource;

// 读之前在界面线程上定下来的。
struct ChromiumReadOptions {
  ChromiumReadOptions();
  ChromiumReadOptions(const ChromiumReadOptions&);
  ~ChromiumReadOptions();

  // 要读哪几样：user_data_importer::ImportItem 的位。
  uint16_t items = 0;
  // 书签栏、移动设备书签导入后的文件夹名（界面语言的）。「其他书签」里的直接放进导入的
  // 文件夹。
  std::u16string bookmark_bar_name;
  std::u16string mobile_bookmarks_name;
  // 每次开始读一样时（在读的线程上）。
  base::RepeatingCallback<void(user_data_importer::ImportItem)> on_item;
};

// 从一个 Chromium 系浏览器的用户资料中读取的数据（书签、历史记录、密码、搜索引擎、
// 表单自动填充）。
struct ChromiumImportData {
  ChromiumImportData();
  ChromiumImportData(ChromiumImportData&&);
  ChromiumImportData& operator=(ChromiumImportData&&);
  ~ChromiumImportData();

  // 按原来的顺序：书签栏的在前（in_toolbar），然后其他书签、移动设备书签。
  std::vector<user_data_importer::ImportedBookmarkEntry> bookmarks;
  // 每一次访问一行（新的在前，最多 kMaxHistoryVisits 次）：同一个网址可以有好几行，
  // 历史记录里就有这几次访问。
  history::URLRows history;
  std::vector<password_manager::PasswordForm> passwords;
  // 解不开的密码有几个（v20、v11、密钥不对）。
  size_t undecryptable_passwords = 0;
  // 拿不到密码的密钥：macOS 的钥匙串没有允许（或者没有这一项），一个也解不开。
  bool password_key_unavailable = false;
  // 用户自己加的、或者用过的搜索引擎（不含浏览器内置的）。
  std::vector<TemplateURLData> search_engines;
  std::vector<autofill::AutocompleteEntry> autofill;
  // 要读但没读出来的几样（文件打不开、被正开着的浏览器锁住了、格式认不出）。
  uint16_t failed = 0;
};

// 最多导入多少次访问（Chrome 留 90 天的，一般几万次）。
inline constexpr size_t kMaxHistoryVisits = 300000;
// 最多导入多少条表单自动填充的记录。
inline constexpr size_t kMaxAutofillEntries = 50000;

// 读取一个用户资料。SQLite 数据库先复制到临时目录再打开（源浏览器可能正在运行）。
// 会执行文件 I/O，读取密码时在 macOS 上可能请求钥匙串授权，应在可阻塞线程调用。
ChromiumImportData ReadChromiumProfile(const ImportSource& source,
                                       const ChromiumReadOptions& options);

}  // namespace starry

#endif  // STARRY_BROWSER_IMPORTER_CHROMIUM_READER_H_
