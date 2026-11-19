// Copyright 2026 The Starry Browser Authors

#include "starry/browser/importer/chromium_reader.h"

#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/password_manager/core/browser/password_string.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "starry/browser/importer/chromium_browsers.h"
#include "starry/browser/importer/chromium_passwords.h"
#include "starry/browser/importer/import_source.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace starry {

namespace {

using user_data_importer::ImportedBookmarkEntry;

// sql::Database 要一个登记过的标签（sql/histograms.xml，编译时核对），只用来分指标。
// 用 Chrome 的 Firefox 导入器那个，免得改 histograms.xml 重编所有用到 sql/database.h 的
// 文件。
constexpr sql::Database::Tag kDatabaseTag{"FirefoxImporter"};

// 书签文件一般几百 KB。
constexpr size_t kMaxBookmarksFileSize = 64 * 1024 * 1024;

// Chromium 的时间：1601 年起的微秒（书签的 date_added 是这个数的字符串）。
base::Time FromChromiumTime(int64_t microseconds) {
  return base::Time::FromDeltaSinceWindowsEpoch(
      base::Microseconds(microseconds));
}

// ---- 书签 ----

void AppendBookmarks(const base::DictValue& folder,
                     const std::vector<std::u16string>& path,
                     bool in_toolbar,
                     base::Time now,
                     std::vector<ImportedBookmarkEntry>& out) {
  const base::ListValue* children = folder.FindList("children");
  if (!children) {
    return;
  }
  for (const base::Value& value : *children) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& node = value.GetDict();
    const std::string* type = node.FindString("type");
    const std::string* name = node.FindString("name");
    const std::u16string title = name ? base::UTF8ToUTF16(*name) : u"";
    base::Time created = now;
    int64_t date_added = 0;
    if (const std::string* date = node.FindString("date_added");
        date && base::StringToInt64(*date, &date_added) && date_added > 0) {
      created = FromChromiumTime(date_added);
    }
    if (type && *type == "folder") {
      const base::ListValue* grandchildren = node.FindList("children");
      if (grandchildren && !grandchildren->empty()) {
        std::vector<std::u16string> child_path = path;
        child_path.push_back(title);
        AppendBookmarks(node, child_path, in_toolbar, now, out);
        continue;
      }
      // 空的文件夹也带过来（有东西的文件夹由里面的书签的 path 建出来）。
      ImportedBookmarkEntry entry;
      entry.is_folder = true;
      entry.in_toolbar = in_toolbar;
      entry.path = path;
      entry.title = title;
      entry.creation_time = created;
      out.push_back(std::move(entry));
    } else if (type && *type == "url") {
      const std::string* url = node.FindString("url");
      GURL gurl(url ? *url : std::string());
      if (!gurl.is_valid()) {
        continue;
      }
      ImportedBookmarkEntry entry;
      entry.is_folder = false;
      entry.in_toolbar = in_toolbar;
      entry.url = std::move(gurl);
      entry.path = path;
      entry.title = title;
      entry.creation_time = created;
      out.push_back(std::move(entry));
    }
  }
}

// Bookmarks（或 AccountBookmarks）的 JSON：roots 下面是 bookmark_bar、other、synced
// （移动设备书签）三个根文件夹。
bool ReadBookmarksFile(const base::FilePath& file,
                       const ChromiumReadOptions& options,
                       std::vector<ImportedBookmarkEntry>& out) {
  std::string text;
  if (!base::ReadFileToStringWithMaxSize(file, &text, kMaxBookmarksFileSize)) {
    return false;
  }
  const std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(text, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const base::DictValue* roots = root ? root->FindDict("roots") : nullptr;
  if (!roots) {
    return false;
  }
  const base::Time now = base::Time::Now();
  // 书签栏的：直接放进 Starry 的书签栏（它空着时）或者导入的文件夹里的「书签栏」。
  if (const base::DictValue* bar = roots->FindDict("bookmark_bar")) {
    AppendBookmarks(*bar, {options.bookmark_bar_name}, /*in_toolbar=*/true, now,
                    out);
  }
  if (const base::DictValue* other = roots->FindDict("other")) {
    AppendBookmarks(*other, {}, /*in_toolbar=*/false, now, out);
  }
  if (const base::DictValue* synced = roots->FindDict("synced")) {
    AppendBookmarks(*synced, {options.mobile_bookmarks_name},
                    /*in_toolbar=*/false, now, out);
  }
  return true;
}

// ---- 数据库 ----

// 把数据库（连同没写完的 -journal、-wal）复制到临时目录再打开：浏览器开着时文件可能
// 正在写，直接打开还可能被它锁着。Windows 上浏览器开着时常常连复制都不行。
class DatabaseCopy {
 public:
  bool Open(const base::FilePath& file) {
    if (!base::PathExists(file) || !temp_dir_.CreateUniqueTempDir()) {
      return false;
    }
    const base::FilePath copy = temp_dir_.GetPath().Append(file.BaseName());
    if (!base::CopyFile(file, copy)) {
      return false;
    }
    for (const base::FilePath::CharType* suffix :
         {FILE_PATH_LITERAL("-journal"), FILE_PATH_LITERAL("-wal")}) {
      const base::FilePath sidecar(file.value() + suffix);
      if (base::PathExists(sidecar)) {
        base::CopyFile(sidecar, base::FilePath(copy.value() + suffix));
      }
    }
    return database_.Open(copy);
  }

  sql::Database& database() { return database_; }

 private:
  base::ScopedTempDir temp_dir_;
  // 先关数据库再删临时目录（成员倒着析构）。
  sql::Database database_{sql::DatabaseOptions().set_exclusive_locking(false),
                          kDatabaseTag};
};

bool IsHistoryScheme(const GURL& url) {
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile() ||
         url.SchemeIs(url::kFtpScheme);
}

// 每次访问一行，只取跳转链末尾，排除子框架和关键字生成的记录。
bool ReadHistory(const base::FilePath& file, history::URLRows& out) {
  DatabaseCopy copy;
  if (!copy.Open(file)) {
    return false;
  }
  sql::Database& db = copy.database();
  if (!db.DoesTableExist("urls") || !db.DoesTableExist("visits")) {
    return false;
  }
  sql::Statement statement(db.GetUniqueStatement(
      "SELECT u.url, u.title, v.visit_time, u.typed_count, u.visit_count "
      "FROM urls u JOIN visits v ON u.id = v.url "
      "WHERE u.hidden = 0 AND (v.transition & ?) != 0 "
      "AND (v.transition & ?) NOT IN (?, ?, ?) "
      "ORDER BY v.visit_time DESC LIMIT ?"));
  statement.BindInt64(0, ui::PAGE_TRANSITION_CHAIN_END);
  statement.BindInt64(1, ui::PAGE_TRANSITION_CORE_MASK);
  statement.BindInt64(2, ui::PAGE_TRANSITION_AUTO_SUBFRAME);
  statement.BindInt64(3, ui::PAGE_TRANSITION_MANUAL_SUBFRAME);
  statement.BindInt64(4, ui::PAGE_TRANSITION_KEYWORD_GENERATED);
  statement.BindInt64(5, static_cast<int64_t>(kMaxHistoryVisits));
  while (statement.Step()) {
    GURL url(statement.ColumnStringView(0));
    const int64_t visit_time = statement.ColumnInt64(2);
    if (!url.is_valid() || !IsHistoryScheme(url) || visit_time <= 0) {
      continue;
    }
    history::URLRow row(url);
    row.set_title(statement.ColumnString16(1));
    row.set_last_visit(FromChromiumTime(visit_time));
    row.set_typed_count(statement.ColumnInt(3));
    row.set_visit_count(statement.ColumnInt(4));
    out.push_back(std::move(row));
  }
  return statement.Succeeded();
}

// 同一个登录（密码库的主键）在 Login Data 和 Login Data For Account 里都有时只要一个。
using LoginKey = std::tuple<std::string,
                            std::string,
                            std::u16string,
                            std::u16string,
                            std::u16string>;

bool ReadLogins(const base::FilePath& file,
                const ChromiumPasswordDecryptor& decryptor,
                std::set<LoginKey>& seen,
                ChromiumImportData& data) {
  DatabaseCopy copy;
  if (!copy.Open(file)) {
    return false;
  }
  sql::Database& db = copy.database();
  if (!db.DoesTableExist("logins")) {
    return false;
  }
  // 联合登录（用 Google 账号登录的那种）没有密码，不导入。
  const bool has_federation = db.DoesColumnExist("logins", "federation_url");
  sql::Statement statement(
      has_federation
          ? db.GetUniqueStatement(
                "SELECT origin_url, action_url, username_element, "
                "username_value, password_element, password_value, "
                "signon_realm, blacklisted_by_user, scheme, federation_url "
                "FROM logins")
          : db.GetUniqueStatement(
                "SELECT origin_url, action_url, username_element, "
                "username_value, password_element, password_value, "
                "signon_realm, blacklisted_by_user, scheme FROM logins"));
  while (statement.Step()) {
    if (has_federation && !statement.ColumnStringView(9).empty()) {
      continue;
    }
    // 只有网页表单（0）和 HTTP 基本认证（1），同 Chrome 的导入器。
    const int scheme = statement.ColumnInt(8);
    if (scheme != 0 && scheme != 1) {
      continue;
    }
    password_manager::PasswordForm form;
    form.scheme = scheme == 1 ? password_manager::PasswordForm::Scheme::kBasic
                              : password_manager::PasswordForm::Scheme::kHtml;
    form.url = GURL(statement.ColumnStringView(0));
    form.action = GURL(statement.ColumnStringView(1));
    form.username_element = statement.ColumnString16(2);
    form.username_value = statement.ColumnString16(3);
    form.password_element = statement.ColumnString16(4);
    form.signon_realm = statement.ColumnString(6);
    form.blocked_by_user = statement.ColumnBool(7);
    if (!form.url.is_valid() || form.signon_realm.empty()) {
      continue;
    }
    if (!form.blocked_by_user) {
      std::optional<std::u16string> password =
          decryptor.Decrypt(statement.ColumnBlob(5));
      if (!password) {
        ++data.undecryptable_passwords;
        continue;
      }
      if (password->empty()) {
        continue;
      }
      form.password_value =
          password_manager::PasswordString(std::move(*password));
    }
    if (!seen.emplace(form.signon_realm, form.url.spec(), form.username_element,
                      form.username_value, form.password_element)
             .second) {
      continue;
    }
    data.passwords.push_back(std::move(form));
  }
  return statement.Succeeded();
}

// keywords 表里用户自己加的、或者用过（激活了）的搜索引擎；浏览器内置的、管理员设的、
// 地址栏里 @书签这种内置的快捷方式不要。
bool ReadSearchEngines(sql::Database& db, std::vector<TemplateURLData>& out) {
  if (!db.DoesTableExist("keywords")) {
    return false;
  }
  std::string query =
      "SELECT short_name, keyword, url FROM keywords WHERE prepopulate_id = 0";
  if (db.DoesColumnExist("keywords", "created_by_policy")) {
    query += " AND created_by_policy = 0";
  }
  if (db.DoesColumnExist("keywords", "starter_pack_id")) {
    query += " AND starter_pack_id = 0";
  }
  // is_active：0 没定、1 激活了、2 停用了（网站自动加的默认是没定，不导入）。
  query += db.DoesColumnExist("keywords", "is_active")
               ? " AND (safe_for_autoreplace = 0 OR is_active = 1)"
               : " AND safe_for_autoreplace = 0";
  sql::Statement statement(db.GetUniqueStatement(query));
  while (statement.Step()) {
    TemplateURLData engine;
    const std::u16string keyword = statement.ColumnString16(1);
    const std::string url = statement.ColumnString(2);
    if (keyword.empty() || url.empty()) {
      continue;
    }
    const std::u16string name = statement.ColumnString16(0);
    engine.SetShortName(name.empty() ? keyword : name);
    engine.SetKeyword(keyword);
    engine.SetURL(url);
    out.push_back(std::move(engine));
  }
  return statement.Succeeded();
}

// autofill 表：表单里填过的字（时间是 1970 年起的秒）。
bool ReadAutofill(sql::Database& db,
                  std::vector<autofill::AutocompleteEntry>& out) {
  if (!db.DoesTableExist("autofill")) {
    return false;
  }
  sql::Statement statement(db.GetUniqueStatement(
      "SELECT name, value, date_created, date_last_used FROM autofill "
      "ORDER BY date_last_used DESC LIMIT ?"));
  statement.BindInt64(0, static_cast<int64_t>(kMaxAutofillEntries));
  while (statement.Step()) {
    const std::u16string name = statement.ColumnString16(0);
    const std::u16string value = statement.ColumnString16(1);
    if (name.empty() || value.empty()) {
      continue;
    }
    out.emplace_back(autofill::AutocompleteKey(name, value),
                     base::Time::FromTimeT(statement.ColumnInt64(2)),
                     base::Time::FromTimeT(statement.ColumnInt64(3)));
  }
  return statement.Succeeded();
}

}  // namespace

ChromiumReadOptions::ChromiumReadOptions() = default;
ChromiumReadOptions::ChromiumReadOptions(const ChromiumReadOptions&) = default;
ChromiumReadOptions::~ChromiumReadOptions() = default;

ChromiumImportData::ChromiumImportData() = default;
ChromiumImportData::ChromiumImportData(ChromiumImportData&&) = default;
ChromiumImportData& ChromiumImportData::operator=(ChromiumImportData&&) =
    default;
ChromiumImportData::~ChromiumImportData() = default;

ChromiumImportData ReadChromiumProfile(const ImportSource& source,
                                       const ChromiumReadOptions& options) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  ChromiumImportData data;
  const base::FilePath& dir = source.profile_dir;
  const auto starting = [&options](user_data_importer::ImportItem item) {
    if (options.on_item) {
      options.on_item.Run(item);
    }
  };
  const auto wanted = [&options](user_data_importer::ImportItem item) {
    return (options.items & item) != 0;
  };

  if (wanted(user_data_importer::FAVORITES)) {
    starting(user_data_importer::FAVORITES);
    bool read = false;
    for (const char* name : {kBookmarksFile, kAccountBookmarksFile}) {
      const base::FilePath file = dir.AppendASCII(name);
      if (base::PathExists(file)) {
        read |= ReadBookmarksFile(file, options, data.bookmarks);
      }
    }
    if (!read) {
      data.failed |= user_data_importer::FAVORITES;
    }
  }

  if (wanted(user_data_importer::HISTORY)) {
    starting(user_data_importer::HISTORY);
    if (!ReadHistory(dir.AppendASCII(kHistoryFile), data.history)) {
      data.failed |= user_data_importer::HISTORY;
    }
  }

  if (wanted(user_data_importer::PASSWORDS) && source.chromium) {
    starting(user_data_importer::PASSWORDS);
    // macOS 上这里会问钥匙串。
    const ChromiumPasswordDecryptor decryptor =
        ChromiumPasswordDecryptor::Create(*source.chromium,
                                          source.user_data_dir);
#if !BUILDFLAG(IS_WIN)
    data.password_key_unavailable =
        decryptor.status() != ChromiumPasswordDecryptor::Status::kReady;
#endif
    if (!data.password_key_unavailable) {
      std::set<LoginKey> seen;
      bool read = false;
      // 存在 Google 账号里的（Login Data For Account）和只在这台电脑上的。
      for (const char* name : {kLoginDataFile, kAccountLoginDataFile}) {
        const base::FilePath file = dir.AppendASCII(name);
        if (base::PathExists(file)) {
          read |= ReadLogins(file, decryptor, seen, data);
        }
      }
      if (!read) {
        data.failed |= user_data_importer::PASSWORDS;
      }
    }
  }

  const bool engines = wanted(user_data_importer::SEARCH_ENGINES);
  const bool autofill = wanted(user_data_importer::AUTOFILL_FORM_DATA);
  if (engines || autofill) {
    DatabaseCopy copy;
    const bool opened = copy.Open(dir.AppendASCII(kWebDataFile));
    if (engines) {
      starting(user_data_importer::SEARCH_ENGINES);
      if (!opened || !ReadSearchEngines(copy.database(), data.search_engines)) {
        data.failed |= user_data_importer::SEARCH_ENGINES;
      }
    }
    if (autofill) {
      starting(user_data_importer::AUTOFILL_FORM_DATA);
      if (!opened || !ReadAutofill(copy.database(), data.autofill)) {
        data.failed |= user_data_importer::AUTOFILL_FORM_DATA;
      }
    }
  }
  return data;
}

}  // namespace starry
