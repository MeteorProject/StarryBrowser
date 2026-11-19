// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/assistant_history.h"

#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace starry::assistant {

namespace {

constexpr base::FilePath::CharType kFileName[] =
    FILE_PATH_LITERAL("Assistant History");
constexpr char kUserDataKey[] = "starry.assistant.history";
constexpr int kListLimit = 200;

// LIKE 的通配符当普通字符找。
std::string EscapeLike(const std::string& word) {
  std::string escaped;
  for (char c : word) {
    if (c == '\\' || c == '%' || c == '_') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

class HistoryHolder : public base::SupportsUserData::Data {
 public:
  explicit HistoryHolder(const base::FilePath& path) : history(path) {}
  History history;
};

}  // namespace

HistoryRecord::HistoryRecord() = default;
HistoryRecord::HistoryRecord(HistoryRecord&&) = default;
HistoryRecord& HistoryRecord::operator=(HistoryRecord&&) = default;
HistoryRecord::~HistoryRecord() = default;

// 在后台序列上。打不开（文件坏了）时什么都不做，列表是空的。
class HistoryStore {
 public:
  explicit HistoryStore(const base::FilePath& path)
      : db_(sql::DatabaseOptions(), "StarryAssistantHistory") {
    const bool opened = path.empty() ? db_.OpenInMemory() : db_.Open(path);
    if (!opened) {
      return;
    }
    // 删掉的记录不只是把页标成空闲（里面可能有敏感内容）：Chromium 的 SQLite 编译时就开了
    // SQLITE_SECURE_DELETE。
    if (!db_.Execute("CREATE TABLE IF NOT EXISTS conversation ("
                     "id TEXT PRIMARY KEY, "
                     "title TEXT NOT NULL DEFAULT '', "
                     "site TEXT, "
                     "created REAL NOT NULL, "
                     "updated REAL NOT NULL, "
                     "search TEXT NOT NULL DEFAULT '', "
                     "payload BLOB NOT NULL)") ||
        !db_.Execute("CREATE INDEX IF NOT EXISTS conversation_updated "
                     "ON conversation(updated DESC)") ||
        !db_.Execute("CREATE TABLE IF NOT EXISTS image ("
                     "id TEXT PRIMARY KEY, "
                     "conversation_id TEXT NOT NULL, "
                     "mime TEXT NOT NULL, "
                     "data BLOB NOT NULL)") ||
        !db_.Execute("CREATE INDEX IF NOT EXISTS image_conversation "
                     "ON image(conversation_id)")) {
      db_.Close();
      return;
    }
    ready_ = true;
  }

  void Save(HistoryRecord record) {
    if (!ready_) {
      return;
    }
    sql::Transaction transaction(&db_);
    if (!transaction.Begin()) {
      return;
    }
    // 已有的覆盖（Chromium 的 SQLite 关了 UPSERT：先保证有这一行，再改）。
    sql::Statement insert(db_.GetCachedStatement(
        SQL_FROM_HERE,
        "INSERT OR IGNORE INTO conversation (id, created, updated, payload) "
        "VALUES (?, ?, ?, '')"));
    insert.BindString(0, record.id);
    insert.BindDouble(1, record.created.InSecondsFSinceUnixEpoch());
    insert.BindDouble(2, record.updated.InSecondsFSinceUnixEpoch());
    sql::Statement update(db_.GetCachedStatement(
        SQL_FROM_HERE,
        "UPDATE conversation SET title = ?, site = ?, updated = ?, "
        "search = ?, payload = ? WHERE id = ?"));
    update.BindString(0, record.title);
    if (record.site.empty()) {
      update.BindNull(1);
    } else {
      update.BindString(1, record.site);
    }
    update.BindDouble(2, record.updated.InSecondsFSinceUnixEpoch());
    update.BindString(3, record.search);
    update.BindBlob(4, std::move(record.payload));
    update.BindString(5, record.id);
    if (!insert.Run() || !update.Run()) {
      return;
    }
    for (auto& [image_id, image] : record.images) {
      std::string data;
      if (!base::Base64Decode(image.base64, &data)) {
        continue;
      }
      sql::Statement add(db_.GetCachedStatement(
          SQL_FROM_HERE,
          "INSERT OR REPLACE INTO image (id, conversation_id, mime, data) "
          "VALUES (?, ?, ?, ?)"));
      add.BindString(0, image_id);
      add.BindString(1, record.id);
      add.BindString(2, image.mime_type);
      add.BindBlob(3, std::move(data));
      if (!add.Run()) {
        return;
      }
    }
    std::ignore = transaction.Commit();
  }

  std::vector<HistorySummary> List(const std::string& query) {
    std::vector<HistorySummary> list;
    if (!ready_) {
      return list;
    }
    const std::vector<std::string> words =
        base::SplitString(query, base::kWhitespaceASCII, base::TRIM_WHITESPACE,
                          base::SPLIT_WANT_NONEMPTY);
    std::string sql =
        "SELECT id, title, site, created, updated FROM conversation";
    for (size_t i = 0; i < words.size(); ++i) {
      base::StrAppend(
          &sql, {i == 0 ? " WHERE " : " AND ",
                 "(title LIKE ? ESCAPE '\\' OR search LIKE ? ESCAPE '\\')"});
    }
    sql += " ORDER BY updated DESC LIMIT ?";
    sql::Statement statement(db_.GetUniqueStatement(sql));
    int index = 0;
    for (const std::string& word : words) {
      const std::string pattern = base::StrCat({"%", EscapeLike(word), "%"});
      statement.BindString(index++, pattern);
      statement.BindString(index++, pattern);
    }
    statement.BindInt(index, kListLimit);
    while (statement.Step()) {
      HistorySummary summary;
      summary.id = statement.ColumnString(0);
      summary.title = statement.ColumnString(1);
      summary.site = statement.ColumnString(2);
      summary.created =
          base::Time::FromSecondsSinceUnixEpoch(statement.ColumnDouble(3));
      summary.updated =
          base::Time::FromSecondsSinceUnixEpoch(statement.ColumnDouble(4));
      list.push_back(std::move(summary));
    }
    return list;
  }

  std::optional<HistoryRecord> Load(const std::string& id) {
    if (!ready_) {
      return std::nullopt;
    }
    sql::Statement statement(db_.GetCachedStatement(
        SQL_FROM_HERE,
        "SELECT title, site, created, updated, payload FROM conversation "
        "WHERE id = ?"));
    statement.BindString(0, id);
    if (!statement.Step()) {
      return std::nullopt;
    }
    HistoryRecord record;
    record.id = id;
    record.title = statement.ColumnString(0);
    record.site = statement.ColumnString(1);
    record.created =
        base::Time::FromSecondsSinceUnixEpoch(statement.ColumnDouble(2));
    record.updated =
        base::Time::FromSecondsSinceUnixEpoch(statement.ColumnDouble(3));
    record.payload = statement.ColumnBlobAsString(4);
    sql::Statement images(db_.GetCachedStatement(
        SQL_FROM_HERE,
        "SELECT id, mime, data FROM image WHERE conversation_id = ?"));
    images.BindString(0, id);
    while (images.Step()) {
      agent::ToolImage image;
      image.mime_type = images.ColumnString(1);
      image.base64 = base::Base64Encode(images.ColumnBlobAsString(2));
      record.images[images.ColumnString(0)] = std::move(image);
    }
    return record;
  }

  void Delete(const std::string& id) {
    if (!ready_) {
      return;
    }
    sql::Transaction transaction(&db_);
    if (!transaction.Begin()) {
      return;
    }
    sql::Statement images(db_.GetCachedStatement(
        SQL_FROM_HERE, "DELETE FROM image WHERE conversation_id = ?"));
    images.BindString(0, id);
    sql::Statement conversation(db_.GetCachedStatement(
        SQL_FROM_HERE, "DELETE FROM conversation WHERE id = ?"));
    conversation.BindString(0, id);
    if (!images.Run() || !conversation.Run()) {
      return;
    }
    std::ignore = transaction.Commit();
  }

  void DeleteAll() {
    if (!ready_) {
      return;
    }
    if (db_.Execute("DELETE FROM image") &&
        db_.Execute("DELETE FROM conversation")) {
      std::ignore = db_.Execute("VACUUM");
    }
  }

 private:
  sql::Database db_;
  bool ready_ = false;
};

// static
History* History::ForProfile(Profile* profile) {
  Profile* original = profile->GetOriginalProfile();
  auto* holder =
      static_cast<HistoryHolder*>(original->GetUserData(kUserDataKey));
  if (!holder) {
    auto created =
        std::make_unique<HistoryHolder>(original->GetPath().Append(kFileName));
    holder = created.get();
    original->SetUserData(kUserDataKey, std::move(created));
  }
  return &holder->history;
}

History::History(const base::FilePath& path)
    : ephemeral_(path.empty()),
      store_(base::ThreadPool::CreateSequencedTaskRunner(
                 {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
                  base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
             path) {}

History::~History() = default;

void History::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void History::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void History::Changed() {
  for (Observer& observer : observers_) {
    observer.OnHistoryChanged();
  }
}

void History::Save(HistoryRecord record) {
  store_.AsyncCall(&HistoryStore::Save)
      .WithArgs(std::move(record))
      .Then(base::BindOnce(&History::Changed, weak_factory_.GetWeakPtr()));
}

void History::List(
    const std::string& query,
    base::OnceCallback<void(std::vector<HistorySummary>)> callback) {
  store_.AsyncCall(&HistoryStore::List)
      .WithArgs(query)
      .Then(std::move(callback));
}

void History::Load(
    const std::string& id,
    base::OnceCallback<void(std::optional<HistoryRecord>)> callback) {
  store_.AsyncCall(&HistoryStore::Load).WithArgs(id).Then(std::move(callback));
}

void History::Delete(const std::string& id) {
  store_.AsyncCall(&HistoryStore::Delete)
      .WithArgs(id)
      .Then(base::BindOnce(&History::Changed, weak_factory_.GetWeakPtr()));
}

void History::DeleteAll() {
  store_.AsyncCall(&HistoryStore::DeleteAll)
      .Then(base::BindOnce(&History::Changed, weak_factory_.GetWeakPtr()));
}

}  // namespace starry::assistant
