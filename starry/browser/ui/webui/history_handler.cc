// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/history_handler.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/history/core/browser/history_types.h"
#include "components/keyed_service/core/service_access_type.h"
#include "content/public/browser/web_ui.h"
#include "url/gurl.h"

namespace starry {

namespace {

constexpr int kMaxQuery = 500;
constexpr int kMaxOverview = 3000;
// 别的标签访问网页时，最多这么久通知页面一次。
constexpr base::TimeDelta kChangedDelay = base::Milliseconds(400);

std::string DisplayHost(const GURL& url) {
  std::string host(url.host());
  if (host.starts_with("www.")) {
    host = host.substr(4);
  }
  return host;
}

std::optional<double> OptionalDouble(const base::Value* value) {
  if (!value) {
    return std::nullopt;
  }
  if (value->is_double()) {
    return value->GetDouble();
  }
  if (value->is_int()) {
    return static_cast<double>(value->GetInt());
  }
  return std::nullopt;
}

base::Time OptionalTime(const base::Value* value) {
  const std::optional<double> ms = OptionalDouble(value);
  return ms && *ms > 0 ? base::Time::FromMillisecondsSinceUnixEpoch(*ms)
                       : base::Time();
}

void BuildExpireEntry(const GURL& url,
                      double time_ms,
                      bool exact,
                      std::vector<history::ExpireHistoryArgs>* out) {
  if (!url.is_valid() || time_ms <= 0) {
    return;
  }
  history::ExpireHistoryArgs expire;
  expire.urls.insert(url);
  const base::Time visit_time =
      base::Time::FromMillisecondsSinceUnixEpoch(time_ms);
  if (exact) {
    expire.begin_time = visit_time - base::Milliseconds(1);
    expire.end_time = visit_time + base::Milliseconds(1);
  } else {
    expire.SetTimeRangeForOneDay(visit_time);
  }
  out->push_back(std::move(expire));
}

}  // namespace

HistoryHandler::HistoryHandler(Profile* profile)
    : profile_(profile->GetOriginalProfile()),
      incognito_(profile->IsOffTheRecord()) {}

HistoryHandler::~HistoryHandler() = default;

history::HistoryService* HistoryHandler::history() {
  return HistoryServiceFactory::GetForProfile(
      profile_, ServiceAccessType::EXPLICIT_ACCESS);
}

void HistoryHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("queryHistory", &HistoryHandler::HandleQueryHistory);
  add("historyCount", &HistoryHandler::HandleHistoryCount);
  add("historyOverview", &HistoryHandler::HandleHistoryOverview);
  add("removeEntry", &HistoryHandler::HandleRemoveEntry);
  add("removeEntries", &HistoryHandler::HandleRemoveEntries);
  add("removeSite", &HistoryHandler::HandleRemoveSite);
  add("clearHistory", &HistoryHandler::HandleClearHistory);
  add("toggleBookmark", &HistoryHandler::HandleToggleBookmark);
}

void HistoryHandler::OnJavascriptAllowed() {
  if (history::HistoryService* service = history()) {
    observation_.Observe(service);
  }
}

void HistoryHandler::OnJavascriptDisallowed() {
  observation_.Reset();
  changed_timer_.Stop();
  tracker_.TryCancelAll();
}

void HistoryHandler::OnURLVisited(
    history::HistoryService* history_service,
    const history::VisitedURLInfo& visited_url_info) {
  ScheduleChanged();
}

void HistoryHandler::OnHistoryDeletions(
    history::HistoryService* history_service,
    const history::DeletionInfo& deletion_info) {
  ScheduleChanged();
}

void HistoryHandler::ScheduleChanged() {
  if (changed_timer_.IsRunning()) {
    return;
  }
  changed_timer_.Start(FROM_HERE, kChangedDelay,
                       base::BindOnce(
                           [](HistoryHandler* handler) {
                             if (handler->IsJavascriptAllowed()) {
                               handler->FireWebUIListener("history-changed");
                             }
                           },
                           base::Unretained(this)));
}

void HistoryHandler::HandleQueryHistory(const base::ListValue& args) {
  AllowJavascript();
  std::string callback_id = args[0].GetString();
  history::HistoryService* service = history();
  if (!service) {
    base::DictValue empty;
    empty.Set("entries", base::ListValue());
    empty.Set("finished", true);
    empty.Set("nextCursor", 0.0);
    empty.Set("incognito", incognito_);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }

  std::u16string query;
  history::QueryOptions options;
  options.duplicate_policy = history::QueryOptions::REMOVE_DUPLICATES_PER_DAY;
  options.policy_for_404_visits = history::VisitQuery404sPolicy::kInclude404s;
  int max = kMaxQuery;
  bool starred_only = false;

  if (args.size() > 1 && args[1].is_dict()) {
    const base::DictValue& params = args[1].GetDict();
    if (const std::string* q = params.FindString("query")) {
      query = base::UTF8ToUTF16(*q);
    }
    options.begin_time = OptionalTime(params.Find("after"));
    options.end_time = OptionalTime(params.Find("before"));
    if (const std::string* host = params.FindString("host")) {
      options.hostname_suffix = *host;
    }
    if (params.FindBool("allVisits").value_or(false)) {
      options.duplicate_policy = history::QueryOptions::KEEP_ALL_DUPLICATES;
    }
    starred_only = params.FindBool("starredOnly").value_or(false);
    if (const std::optional<int> requested = params.FindInt("max")) {
      max = std::clamp(*requested, 1, kMaxQuery);
    }
    if (starred_only) {
      max = kMaxQuery;
    }
  } else {
    if (args.size() > 1 && args[1].is_string()) {
      query = base::UTF8ToUTF16(args[1].GetString());
    }
    if (args.size() > 2) {
      options.end_time = OptionalTime(&args[2]);
    }
    if (args.size() > 3 && args[3].is_int()) {
      max = std::clamp(args[3].GetInt(), 1, kMaxQuery);
    }
  }
  options.max_count = max;

  service->QueryHistory(
      query, options,
      base::BindOnce(&HistoryHandler::OnQueryDone, weak_factory_.GetWeakPtr(),
                     std::move(callback_id), max, starred_only),
      &tracker_);
}

void HistoryHandler::OnQueryDone(std::string callback_id,
                                 int max,
                                 bool starred_only,
                                 history::QueryResults results) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  const bool finished =
      results.reached_beginning() || static_cast<int>(results.size()) < max;
  const double next_cursor =
      results.empty()
          ? 0.0
          : results.back().visit_time().InMillisecondsFSinceUnixEpoch();

  bookmarks::BookmarkModel* bookmarks =
      BookmarkModelFactory::GetForBrowserContext(profile_);
  base::ListValue entries;
  for (const history::URLResult& result : results) {
    const GURL& url = result.url();
    const bool starred =
        bookmarks && bookmarks->loaded() && bookmarks->IsBookmarked(url);
    if (starred_only && !starred) {
      continue;
    }
    base::DictValue entry;
    entry.Set("id", static_cast<double>(result.visit_id()));
    entry.Set("url", url.spec());
    entry.Set("title", result.title());
    entry.Set("time", result.visit_time().InMillisecondsFSinceUnixEpoch());
    entry.Set("host", DisplayHost(url));
    entry.Set("starred", starred);
    entry.Set("visitCount", result.visit_count());
    entries.Append(std::move(entry));
  }
  base::DictValue reply;
  reply.Set("entries", std::move(entries));
  reply.Set("finished", finished);
  reply.Set("nextCursor", next_cursor);
  reply.Set("incognito", incognito_);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(reply)));
}

void HistoryHandler::HandleHistoryCount(const base::ListValue& args) {
  AllowJavascript();
  std::string callback_id = args[0].GetString();
  history::HistoryService* service = history();
  if (!service) {
    ResolveJavascriptCallback(base::Value(callback_id), base::Value(0));
    return;
  }
  const base::Time begin =
      args.size() > 1 ? OptionalTime(&args[1]) : base::Time();
  base::Time end = args.size() > 2 ? OptionalTime(&args[2]) : base::Time();
  if (end.is_null()) {
    end = base::Time::Max();
  }
  service->GetHistoryCount(
      begin, end, history::VisitQuery404sPolicy::kInclude404s,
      base::BindOnce(
          [](base::WeakPtr<HistoryHandler> handler, std::string callback_id,
             history::HistoryCountResult result) {
            if (handler && handler->IsJavascriptAllowed()) {
              handler->ResolveJavascriptCallback(
                  base::Value(callback_id),
                  base::Value(result.success ? result.count : 0));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(callback_id)),
      &tracker_);
}

void HistoryHandler::HandleHistoryOverview(const base::ListValue& args) {
  AllowJavascript();
  std::string callback_id = args[0].GetString();
  history::HistoryService* service = history();
  if (!service) {
    base::DictValue empty;
    empty.Set("hosts", base::ListValue());
    empty.Set("visits", base::ListValue());
    empty.Set("incognito", incognito_);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }
  history::QueryOptions options;
  options.duplicate_policy = history::QueryOptions::REMOVE_DUPLICATES_PER_DAY;
  options.policy_for_404_visits = history::VisitQuery404sPolicy::kInclude404s;
  options.begin_time = args.size() > 1 ? OptionalTime(&args[1])
                                       : (base::Time::Now().LocalMidnight() -
                                          base::Days(30) + base::Hours(12))
                                             .LocalMidnight();
  if (args.size() > 2) {
    options.end_time = OptionalTime(&args[2]);
  }
  options.max_count = kMaxOverview;
  service->QueryHistory(
      std::u16string(), options,
      base::BindOnce(&HistoryHandler::OnOverviewDone,
                     weak_factory_.GetWeakPtr(), std::move(callback_id)),
      &tracker_);
}

void HistoryHandler::OnOverviewDone(std::string callback_id,
                                    history::QueryResults results) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  std::map<std::string, int> host_indices;
  base::ListValue hosts;
  base::ListValue visits;
  for (const history::URLResult& result : results) {
    const GURL& url = result.url();
    const std::string host = DisplayHost(url);
    int host_index = -1;
    if (!host.empty()) {
      auto [it, inserted] =
          host_indices.emplace(host, static_cast<int>(hosts.size()));
      host_index = it->second;
      if (inserted) {
        base::DictValue info;
        info.Set("host", host);
        info.Set("url", url.spec());
        hosts.Append(std::move(info));
      }
    }
    base::ListValue visit;
    visit.Append(result.visit_time().InMillisecondsFSinceUnixEpoch());
    visit.Append(host_index);
    visits.Append(std::move(visit));
  }
  base::DictValue reply;
  reply.Set("hosts", std::move(hosts));
  reply.Set("visits", std::move(visits));
  reply.Set("incognito", incognito_);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(reply)));
}

void HistoryHandler::HandleRemoveEntry(const base::ListValue& args) {
  history::HistoryService* service = history();
  if (!service || args.size() < 2 || !args[0].is_string()) {
    return;
  }
  const GURL url(args[0].GetString());
  const double time = OptionalDouble(&args[1]).value_or(0);
  const bool exact = args.size() > 2 && args[2].is_bool() && args[2].GetBool();
  std::vector<history::ExpireHistoryArgs> expires;
  BuildExpireEntry(url, time, exact, &expires);
  if (!expires.empty()) {
    service->ExpireHistory(expires, base::DoNothing(), &tracker_);
  }
}

void HistoryHandler::HandleRemoveEntries(const base::ListValue& args) {
  history::HistoryService* service = history();
  if (!service || args.empty() || !args[0].is_list()) {
    return;
  }
  std::vector<history::ExpireHistoryArgs> expires;
  for (const base::Value& item_value : args[0].GetList()) {
    if (!item_value.is_dict()) {
      continue;
    }
    const base::DictValue& item = item_value.GetDict();
    const std::string* url_str = item.FindString("url");
    const std::optional<double> time = OptionalDouble(item.Find("time"));
    if (!url_str || !time) {
      continue;
    }
    BuildExpireEntry(GURL(*url_str), *time,
                     item.FindBool("exact").value_or(false), &expires);
  }
  if (!expires.empty()) {
    service->ExpireHistory(expires, base::DoNothing(), &tracker_);
  }
}

void HistoryHandler::HandleRemoveSite(const base::ListValue& args) {
  history::HistoryService* service = history();
  if (!service || args.empty() || !args[0].is_string()) {
    return;
  }
  // 先找出这个网站的全部网址（含子域名），再整个删掉。
  history::QueryOptions options;
  options.hostname_suffix = args[0].GetString();
  options.duplicate_policy = history::QueryOptions::REMOVE_ALL_DUPLICATES;
  options.max_count = 0;
  service->QueryHistory(std::u16string(), options,
                        base::BindOnce(
                            [](base::WeakPtr<HistoryHandler> handler,
                               history::QueryResults results) {
                              if (!handler || !handler->history()) {
                                return;
                              }
                              std::vector<GURL> urls;
                              for (const history::URLResult& result : results) {
                                urls.push_back(result.url());
                              }
                              handler->history()->DeleteURLs(urls);
                            },
                            weak_factory_.GetWeakPtr()),
                        &tracker_);
}

void HistoryHandler::HandleClearHistory(const base::ListValue& args) {
  history::HistoryService* service = history();
  if (!service || args.empty() || !args[0].is_string()) {
    return;
  }
  const std::string& range = args[0].GetString();
  const base::Time now = base::Time::Now();
  base::Time begin;
  base::Time end;
  if (range == "lastHour") {
    begin = now - base::Hours(1);
  } else if (range == "today") {
    begin = now.LocalMidnight();
  } else if (range == "todayAndYesterday") {
    begin = (now.LocalMidnight() - base::Hours(12)).LocalMidnight();
  } else if (range == "last7Days") {
    begin =
        (now.LocalMidnight() - base::Days(6) + base::Hours(12)).LocalMidnight();
  } else if (range == "range") {
    begin = args.size() > 1 ? OptionalTime(&args[1]) : base::Time();
    end = args.size() > 2 ? OptionalTime(&args[2]) : base::Time();
    if (begin.is_null() || end.is_null() || begin >= end) {
      return;
    }
  }
  // 清除所选时段的访问记录及其关联的网站图标。
  service->ExpireHistoryBetween(std::set<GURL>(), std::nullopt, begin, end,
                                /*user_initiated=*/true, base::DoNothing(),
                                &tracker_);
}

void HistoryHandler::HandleToggleBookmark(const base::ListValue& args) {
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(profile_);
  if (!model || !model->loaded() || args.size() < 3 || !args[1].is_string()) {
    ResolveJavascriptCallback(base::Value(callback_id), base::Value(false));
    return;
  }
  const GURL url(args[1].GetString());
  const std::u16string title = args[2].is_string()
                                   ? base::UTF8ToUTF16(args[2].GetString())
                                   : std::u16string();
  bool starred = false;
  if (model->IsBookmarked(url)) {
    bookmarks::RemoveAllBookmarks(model, url, FROM_HERE);
  } else {
    bookmarks::AddIfNotBookmarked(model, url, title);
    starred = true;
  }
  ResolveJavascriptCallback(base::Value(callback_id), base::Value(starred));
}

}  // namespace starry
