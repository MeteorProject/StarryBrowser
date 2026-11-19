// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_HISTORY_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_HISTORY_HANDLER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_service_observer.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace history {
class QueryResults;
}

namespace starry {

// 历史记录页（pages/history/）的数据：Chrome 的 HistoryService。无痕窗口查看原始
// 用户资料的历史（普通窗口共用历史，无痕窗口不记录）。
//
//   queryHistory(callbackId, queryOrOptions, before, max)
//     支持按词、时间区间（after..before，毫秒）、网站（host）、是否保留同一天的多次访问
//     （allVisits，时间线视图用）和只看书签（starredOnly）查询。
//     回 {entries: [{id, url, title, time, host, starred, visitCount}],
//         finished, nextCursor, incognito}。
//   historyCount(callbackId, after?, before?)：区间内一共多少条（同一天同一网址算一条）。
//   historyOverview(callbackId, after?, before?)
//     取区间内按天去重后的访问时间和站点表，供 30 天活动柱状图和常访网站筛选使用。
//   removeEntry(url, time, exact?)：删掉那一天（exact 为真时只删那一次）对这个网址的访问。
//   removeEntries([{url, time, exact?}])：批量删除选中的访问记录。
//   removeSite(host)：删掉这个网站的全部记录。
//   clearHistory(range, begin?, end?)
//     lastHour、today、todayAndYesterday、last7Days、all，或 range（指定起止毫秒）。
//   toggleBookmark(callbackId, url, title)：回加了还是去掉了书签。
// 历史变了（别的标签访问了网页、删了记录）时发 history-changed。
class HistoryHandler : public content::WebUIMessageHandler,
                       public history::HistoryServiceObserver {
 public:
  explicit HistoryHandler(Profile* profile);
  HistoryHandler(const HistoryHandler&) = delete;
  HistoryHandler& operator=(const HistoryHandler&) = delete;
  ~HistoryHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // history::HistoryServiceObserver:
  void OnURLVisited(history::HistoryService* history_service,
                    const history::VisitedURLInfo& visited_url_info) override;
  void OnHistoryDeletions(history::HistoryService* history_service,
                          const history::DeletionInfo& deletion_info) override;

 private:
  void HandleQueryHistory(const base::ListValue& args);
  void HandleHistoryCount(const base::ListValue& args);
  void HandleHistoryOverview(const base::ListValue& args);
  void HandleRemoveEntry(const base::ListValue& args);
  void HandleRemoveEntries(const base::ListValue& args);
  void HandleRemoveSite(const base::ListValue& args);
  void HandleClearHistory(const base::ListValue& args);
  void HandleToggleBookmark(const base::ListValue& args);
  void OnQueryDone(std::string callback_id,
                   int max,
                   bool starred_only,
                   history::QueryResults results);
  void OnOverviewDone(std::string callback_id, history::QueryResults results);
  // 一连串的变化合成一次通知。
  void ScheduleChanged();
  history::HistoryService* history();

  const raw_ptr<Profile> profile_;
  const bool incognito_;
  base::CancelableTaskTracker tracker_;
  base::OneShotTimer changed_timer_;
  base::ScopedObservation<history::HistoryService,
                          history::HistoryServiceObserver>
      observation_{this};

  base::WeakPtrFactory<HistoryHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_HISTORY_HANDLER_H_
