// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/location_picker_handler.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "net/base/url_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/privacy/location_simulation.h"
#include "starry/browser/ui/location/location_picker.h"
#include "starry/browser/web_time_zone.h"

namespace starry {

namespace {

constexpr char kTileHost[] = "tiles.openfreemap.org";
constexpr size_t kMaxTileBytes = 8 << 20;

constexpr net::NetworkTrafficAnnotationTag kMapAnnotation =
    net::DefineNetworkTrafficAnnotation("starry_location_picker_map", R"(
        semantics {
          sender: "Starry Browser location simulation"
          description:
            "Downloads OpenFreeMap vector map tiles for the map where the user "
            "chooses a simulated location."
          trigger: "The user opens the location picker (privacy settings, the "
            "shield card in the address bar, or a location permission "
            "request) and moves the map."
          data: "The tile coordinates of the area shown on the map. No "
            "cookies, referrer or user data."
          destination: OTHER
          destination_other: "OpenFreeMap (tiles.openfreemap.org)."
        }
        policy {
          cookies_allowed: NO
          setting: "Not opened unless the user chooses a simulated location; "
            "the picker falls back to an offline map when tiles can't load."
          policy_exception_justification: "Not implemented."
        })");

}  // namespace

LocationPickerHandler::LocationPickerHandler(Profile* profile)
    : profile_(profile) {}

LocationPickerHandler::~LocationPickerHandler() = default;

void LocationPickerHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getLocationPicker", &LocationPickerHandler::HandleGet);
  add("pickLocation", &LocationPickerHandler::HandlePick);
  add("cancelLocationPicker", &LocationPickerHandler::HandleCancel);
  add("fetchMapResource", &LocationPickerHandler::HandleFetch);
}

void LocationPickerHandler::OnJavascriptDisallowed() {
  loaders_.clear();
  weak_factory_.InvalidateWeakPtrs();
}

std::optional<std::string> LocationPickerHandler::Site() {
  content::WebContents* contents = web_ui()->GetWebContents();
  if (LocationPickerOwner* owner =
          LocationPickerOwner::FromWebContents(contents)) {
    return owner->site();
  }
  std::string site;
  if (!net::GetValueForKeyInQuery(contents->GetLastCommittedURL(), "site",
                                  &site)) {
    return std::string();
  }
  site = NormalizeLocationSiteInput(site);
  return site.empty() ? std::nullopt : std::optional(std::move(site));
}

void LocationPickerHandler::Close(bool saved) {
  content::WebContents* contents = web_ui()->GetWebContents();
  if (LocationPickerOwner* owner =
          LocationPickerOwner::FromWebContents(contents)) {
    owner->Finish(saved);
    return;
  }
  // 单独开在标签里：关掉这个标签（不在网页的消息里同步关）。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&content::WebContents::ClosePage, contents->GetWeakPtr()));
}

void LocationPickerHandler::HandleGet(const base::ListValue& args) {
  AllowJavascript();
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  auto site = Site();
  if (!site) {
    ResolveJavascriptCallback(args[0], base::Value());
    Close(false);
    return;
  }
  base::DictValue result;
  result.Set("title", site->empty()
                          ? L("location.picker.titleGlobal")
                          : L("location.picker.titleSite", {{"site", *site}}));
  // 从这个网站现在的模拟位置开始；没有时从全局位置开始。
  std::optional<SimulatedLocation> location;
  if (!site->empty()) {
    SiteLocationRule rule = GetSiteLocationRule(profile_, *site);
    if (rule.mode == SiteLocationMode::kSimulated) {
      location = std::move(rule.location);
    }
  }
  if (!location) {
    location = GetGlobalSimulatedLocation(profile_);
  }
  if (location) {
    result.Set("location", LocationToValue(*location));
  }
  // 时区菜单就是「网页时区」那份（常用城市和按地区），去掉开头的「跟随系统」。
  base::DictValue zones;
  AddWebTimeZoneSettings(profile_->GetOriginalProfile(), zones);
  base::ListValue menu;
  if (base::ListValue* items = zones.FindList("webTimeZoneMenu")) {
    for (size_t i = 0; i < items->size(); ++i) {
      const base::DictValue& item = (*items)[i].GetDict();
      const std::string* id = item.FindString("id");
      if (i < 2 &&
          ((id && id->empty()) || item.FindBool("separator").value_or(false))) {
        continue;
      }
      menu.Append((*items)[i].Clone());
    }
  }
  result.Set("timeZoneMenu", std::move(menu));
  const std::string& zone = EffectiveWebTimeZone(profile_);
  result.Set("defaultZone", zone.empty() ? SystemTimeZoneId() : zone);
  ResolveJavascriptCallback(args[0], result);
}

void LocationPickerHandler::HandlePick(const base::ListValue& args) {
  if (args.size() != 1 || !args[0].is_dict()) {
    return;
  }
  std::optional<SimulatedLocation> location =
      LocationFromValue(args[0].GetDict());
  if (!location) {
    return;
  }
  auto site = Site();
  if (!site) {
    Close(false);
    return;
  }
  if (site->empty()) {
    SetGlobalSimulatedLocation(profile_, *location);
  } else {
    SetSiteLocationRule(
        profile_, *site,
        {.mode = SiteLocationMode::kSimulated, .location = *location});
  }
  Close(true);
}

void LocationPickerHandler::HandleCancel(const base::ListValue& args) {
  Close(false);
}

void LocationPickerHandler::HandleFetch(const base::ListValue& args) {
  AllowJavascript();
  if (args.size() != 2 || !args[0].is_string() || !args[1].is_string()) {
    return;
  }
  const std::string& callback_id = args[0].GetString();
  const GURL url(args[1].GetString());
  if (!url.is_valid() || !url.SchemeIs(url::kHttpsScheme) ||
      url.host() != kTileHost) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::DictValue().Set("error", "blocked"));
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  auto loader =
      network::SimpleURLLoader::Create(std::move(request), kMapAnnotation);
  loader->SetTimeoutDuration(base::Seconds(20));
  network::SimpleURLLoader* raw = loader.get();
  loaders_[raw] = std::move(loader);
  raw->DownloadToString(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(&LocationPickerHandler::Fetched,
                     weak_factory_.GetWeakPtr(), callback_id, raw),
      kMaxTileBytes);
}

void LocationPickerHandler::Fetched(std::string callback_id,
                                    network::SimpleURLLoader* loader,
                                    std::optional<std::string> body) {
  auto found = loaders_.find(loader);
  const int status = found != loaders_.end() && loader->ResponseInfo() &&
                             loader->ResponseInfo()->headers
                         ? loader->ResponseInfo()->headers->response_code()
                         : 0;
  if (found != loaders_.end()) {
    loaders_.erase(found);
  }
  // HTTP 错误或取不到：网页让 MapLibre 报 error 事件（取不到时换离线底图）。空的
  // 2xx（没有数据的地方）是空瓦片。
  if (!body || status < 200 || status >= 300) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::DictValue()
                                  .Set("error", status ? "http" : "network")
                                  .Set("status", status));
    return;
  }
  ResolveJavascriptCallback(
      base::Value(callback_id),
      base::DictValue().Set("data", base::Base64Encode(*body)));
}

}  // namespace starry
