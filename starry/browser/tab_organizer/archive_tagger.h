// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_TAB_ORGANIZER_ARCHIVE_TAGGER_H_
#define STARRY_BROWSER_TAB_ORGANIZER_ARCHIVE_TAGGER_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "starry/browser/tab_organizer/tab_archive.h"

class Profile;

namespace starry::tab_organizer {

class OrganizerModel;

// 归档时打主题：归档里加了东西以后等几秒，把还没问过的
// （新的在前、30 天内归档的，一次最多 kBatch 个）的标题和网站发给 AI 整理用的模型，回来的
// 主题记进归档（能按主题搜，整页能按主题筛）。已经有的主题（用得最多的几个）一起给模型，
// 同一类的用同一个词。设置里关了（prefs::kArchiveTopics）、没设置模型时不做；出错的这次
// 运行里不再问（下次启动再试）。
class ArchiveTagger : public TabArchive::Observer {
 public:
  // 一次最多问几个；还有没问的时隔多久接着问。
  static constexpr size_t kBatch = 60;

  explicit ArchiveTagger(Profile* profile);
  ArchiveTagger(const ArchiveTagger&) = delete;
  ArchiveTagger& operator=(const ArchiveTagger&) = delete;
  ~ArchiveTagger() override;

  // 调试用：不等待延迟立即请求一次（忽略设置开关）；done 收到结果
  // （「tagged=N」或者出错的原因）。
  void RunNowForTesting(base::OnceCallback<void(std::string)> done);
  bool running() const { return model_ != nullptr; }

  // TabArchive::Observer:
  void OnArchiveChanged() override;

 private:
  void Schedule(base::TimeDelta delay);
  void Run(bool force);
  void OnAnswer(std::vector<std::string> ids,
                base::expected<std::string, std::string> result);

  const raw_ptr<Profile> profile_;
  base::OneShotTimer timer_;
  std::unique_ptr<OrganizerModel> model_;
  // 这次运行里问出错的：不再问。
  std::set<std::string> failed_;
  base::OnceCallback<void(std::string)> done_;
  base::ScopedObservation<TabArchive, TabArchive::Observer> observation_{this};
  base::WeakPtrFactory<ArchiveTagger> weak_factory_{this};
};

// 给模型的说明、这一次的输入（JSON：界面语言、已有的主题、标签 1……的标题和网站）；读
// 回答 {"topics":{"1":"…","2":null}}：序号 → 主题（最多 24 个字，一行）。读不懂时为空。
std::string TopicSystemPrompt();
std::string TopicPrompt(const std::vector<const ArchivedTab*>& tabs,
                        const std::vector<std::u16string>& known);
std::optional<std::map<size_t, std::u16string>> ParseTopics(
    const std::string& text,
    size_t tab_count);

}  // namespace starry::tab_organizer

#endif  // STARRY_BROWSER_TAB_ORGANIZER_ARCHIVE_TAGGER_H_
