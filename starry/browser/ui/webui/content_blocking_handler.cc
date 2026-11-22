// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/content_blocking_handler.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/l10n/l10n.h"

namespace starry {

namespace {

using content_blocking::ContentBlockingService;

const char* ActivityName(ContentBlockingService::Activity activity) {
  switch (activity) {
    case ContentBlockingService::Activity::kIdle:
      return "idle";
    case ContentBlockingService::Activity::kDownloading:
      return "downloading";
    case ContentBlockingService::Activity::kBuilding:
      return "building";
  }
}

double JsTime(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

}  // namespace

ContentBlockingHandler::ContentBlockingHandler() = default;

ContentBlockingHandler::~ContentBlockingHandler() = default;

void ContentBlockingHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getContentBlocking", &ContentBlockingHandler::HandleGet);
  add("setContentBlocking", &ContentBlockingHandler::HandleSet);
  add("setFilterListEnabled", &ContentBlockingHandler::HandleSetListEnabled);
  add("addFilterList", &ContentBlockingHandler::HandleAddList);
  add("removeFilterList", &ContentBlockingHandler::HandleRemoveList);
  add("updateFilterLists", &ContentBlockingHandler::HandleUpdate);
  add("getCustomRules", &ContentBlockingHandler::HandleGetCustomRules);
  add("setCustomRules", &ContentBlockingHandler::HandleSetCustomRules);
  add("setAllowlisted", &ContentBlockingHandler::HandleSetAllowlisted);
}

void ContentBlockingHandler::OnJavascriptAllowed() {
  observation_.Observe(ContentBlockingService::Get());
}

void ContentBlockingHandler::OnJavascriptDisallowed() {
  observation_.Reset();
  weak_factory_.InvalidateWeakPtrs();
}

void ContentBlockingHandler::OnContentBlockingChanged() {
  FireWebUIListener("content-blocking-changed", base::Value(State()));
}

base::DictValue ContentBlockingHandler::State() const {
  const ContentBlockingService* service = ContentBlockingService::Get();
  base::ListValue subscriptions;
  base::Time last_updated;
  for (const ContentBlockingService::Subscription& subscription :
       service->subscriptions()) {
    base::DictValue item;
    item.Set("id", subscription.id);
    item.Set("url", subscription.url.spec());
    item.Set("title", subscription.title.empty()
                          ? base::UTF16ToUTF8(L(subscription.title_key))
                          : subscription.title);
    item.Set("summary", subscription.summary_key.empty()
                            ? std::string()
                            : base::UTF16ToUTF8(L(subscription.summary_key)));
    if (subscription.homepage.is_valid()) {
      item.Set("homepage", subscription.homepage.spec());
    }
    item.Set("category",
             ContentBlockingService::CategoryName(subscription.category));
    base::ListValue languages;
    for (const std::string& code : subscription.languages) {
      languages.Append(code);
    }
    item.Set("languages", std::move(languages));
    item.Set("enabled", subscription.enabled);
    item.Set("builtin", subscription.builtin || subscription.local);
    item.Set("hasText", subscription.has_text);
    item.Set("hasStats", subscription.has_stats);
    item.Set("rules", static_cast<double>(subscription.rules));
    item.Set("skipped", static_cast<double>(subscription.skipped));
    item.Set("lastUpdated", JsTime(subscription.last_updated));
    item.Set("lastError", subscription.last_error);
    if (subscription.enabled && subscription.last_updated > last_updated) {
      last_updated = subscription.last_updated;
    }
    subscriptions.Append(std::move(item));
  }
  base::ListValue allowlist;
  for (const std::string& site : service->allowlist()) {
    allowlist.Append(site);
  }
  base::ListValue user_languages;
  for (const std::string& code : service->languages()) {
    user_languages.Append(code);
  }
  base::DictValue state;
  state.Set("languages", std::move(user_languages));
  state.Set("enabled", service->enabled());
  state.Set("stripTrackingParameters", service->strip_tracking_parameters());
  state.Set("activity", ActivityName(service->activity()));
  state.Set("downloadsDone", service->downloads_done());
  state.Set("downloadsTotal", service->downloads_total());
  state.Set("ready", service->ready());
  state.Set("totalRules", static_cast<double>(service->total_rules()));
  state.Set("lastUpdated", JsTime(last_updated));
  state.Set("subscriptions", std::move(subscriptions));
  state.Set("hasCustomRules",
            !base::TrimWhitespaceASCII(service->custom_rules(), base::TRIM_ALL)
                 .empty());
  state.Set("customRuleCount", static_cast<double>(service->custom_rule_count()));
  state.Set("customSkipped", static_cast<double>(service->custom_skipped()));
  state.Set("allowlist", std::move(allowlist));
  return state;
}

void ContentBlockingHandler::HandleGet(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(State()));
}

void ContentBlockingHandler::HandleSet(const base::ListValue& args) {
  if (args.size() < 2 || !args[0].is_string() || !args[1].is_bool()) {
    return;
  }
  const std::string& key = args[0].GetString();
  ContentBlockingService* service = ContentBlockingService::Get();
  if (key == "enabled") {
    service->SetEnabled(args[1].GetBool());
  } else if (key == "stripTrackingParameters") {
    service->SetStripTrackingParameters(args[1].GetBool());
  }
}

void ContentBlockingHandler::HandleSetListEnabled(const base::ListValue& args) {
  if (args.size() < 2 || !args[0].is_string() || !args[1].is_bool()) {
    return;
  }
  ContentBlockingService::Get()->SetSubscriptionEnabled(args[0].GetString(),
                                                        args[1].GetBool());
}

void ContentBlockingHandler::HandleAddList(const base::ListValue& args) {
  AllowJavascript();
  if (args.size() < 2 || !args[1].is_string()) {
    return;
  }
  ContentBlockingService::Get()->AddSubscription(
      args[1].GetString(),
      base::BindOnce(&ContentBlockingHandler::OnListAdded,
                     weak_factory_.GetWeakPtr(), args[0].GetString()));
}

void ContentBlockingHandler::OnListAdded(std::string callback_id,
                                         std::string error) {
  base::DictValue result;
  result.Set("error", std::move(error));
  ResolveJavascriptCallback(base::Value(callback_id), base::Value(std::move(result)));
}

void ContentBlockingHandler::HandleRemoveList(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  ContentBlockingService::Get()->RemoveSubscription(args[0].GetString());
}

void ContentBlockingHandler::HandleUpdate(const base::ListValue& args) {
  ContentBlockingService::Get()->UpdateNow();
}

void ContentBlockingHandler::HandleGetCustomRules(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(
      args[0], base::Value(ContentBlockingService::Get()->custom_rules()));
}

void ContentBlockingHandler::HandleSetCustomRules(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  ContentBlockingService::Get()->SetCustomRules(args[0].GetString());
}

void ContentBlockingHandler::HandleSetAllowlisted(const base::ListValue& args) {
  if (args.size() < 2 || !args[0].is_string() || !args[1].is_bool()) {
    return;
  }
  ContentBlockingService::Get()->SetAllowlisted(args[0].GetString(),
                                                args[1].GetBool());
}

}  // namespace starry
