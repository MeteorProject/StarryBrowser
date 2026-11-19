// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_ASSISTANT_HISTORY_H_
#define STARRY_BROWSER_ASSISTANT_ASSISTANT_HISTORY_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "starry/browser/agent/browser_tools.h"

class Profile;

// 内置助手的聊天记录：普通窗口共用用户资料目录里的一份 SQLite（Assistant History）；
// 无痕窗口每个窗口一份内存数据库，窗口关掉就没了。
//
// 对话的内容对这里是一段不透明的 JSON（AssistantConversation 的 Record()），图片单独一张
// 表：列表和搜索不用读它们。删掉的记录不只是把页标成空闲（secure_delete）：里面可能有
// 敏感内容。读写都在后台的一个序列上。
namespace starry::assistant {

// 列表里的一行（不含内容）。
struct HistorySummary {
  std::string id;
  std::string title;
  // 第一条消息所在的网址：行首的网站图标。
  std::string site;
  base::Time created;
  base::Time updated;
};

// 存下来的一个对话。
struct HistoryRecord {
  HistoryRecord();
  HistoryRecord(HistoryRecord&&);
  HistoryRecord& operator=(HistoryRecord&&);
  ~HistoryRecord();

  std::string id;
  std::string title;
  std::string site;
  base::Time created;
  base::Time updated;
  // 搜索用的文字：用户的话和回答。
  std::string search;
  std::string payload;
  // 图片 ID → 图片（只给新的：已经存过的留着）。
  std::map<std::string, agent::ToolImage> images;
};

class HistoryStore;

class History {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // 列表变了（存了、删了）：聊天记录开着的要重新取。
    virtual void OnHistoryChanged() = 0;
  };

  // 普通窗口共用的那一份（用户资料目录里），第一次用时打开。
  static History* ForProfile(Profile* profile);

  // path 是空的：只在内存里（无痕窗口）。
  explicit History(const base::FilePath& path);
  History(const History&) = delete;
  History& operator=(const History&) = delete;
  ~History();

  bool ephemeral() const { return ephemeral_; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  void Save(HistoryRecord record);
  // 最近的在前。query 里每个词都要出现在标题或内容里（不分大小写）；空的是全部。
  void List(const std::string& query,
            base::OnceCallback<void(std::vector<HistorySummary>)> callback);
  // 内容（JSON）和这个对话的所有图片；没有这一条时是 nullopt。
  void Load(const std::string& id,
            base::OnceCallback<void(std::optional<HistoryRecord>)> callback);
  void Delete(const std::string& id);
  void DeleteAll();

 private:
  void Changed();

  const bool ephemeral_;
  base::SequenceBound<HistoryStore> store_;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<History> weak_factory_{this};
};

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_ASSISTANT_HISTORY_H_
