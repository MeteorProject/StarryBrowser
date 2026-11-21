// Copyright 2026 The Starry Browser Authors

#include "starry/browser/web_time_zone.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/common/web_time_zone.h"
#include "third_party/icu/source/common/unicode/strenum.h"

namespace starry {

namespace {

constexpr int kEffectiveTimeZoneKey = 0;

class EffectiveTimeZone : public base::SupportsUserData::Data {
 public:
  explicit EffectiveTimeZone(std::string id) : id(std::move(id)) {}
  const std::string id;
};

struct CommonZone {
  const char* id;
  std::u16string city;
};

const std::vector<CommonZone>& CommonZones() {
  static const base::NoDestructor<std::vector<CommonZone>> zones(
      std::vector<CommonZone>{
          {"UTC", L("region.timeZone.city.utc")},
          {"Asia/Shanghai", L("region.timeZone.city.shanghai")},
          {"Asia/Hong_Kong", L("region.timeZone.city.hongKong")},
          {"Asia/Taipei", L("region.timeZone.city.taipei")},
          {"Asia/Tokyo", L("region.timeZone.city.tokyo")},
          {"Asia/Seoul", L("region.timeZone.city.seoul")},
          {"Asia/Singapore", L("region.timeZone.city.singapore")},
          {"Asia/Kolkata", L("region.timeZone.city.kolkata")},
          {"Asia/Dubai", L("region.timeZone.city.dubai")},
          {"Europe/London", L("region.timeZone.city.london")},
          {"Europe/Paris", L("region.timeZone.city.paris")},
          {"Europe/Berlin", L("region.timeZone.city.berlin")},
          {"Europe/Moscow", L("region.timeZone.city.moscow")},
          {"America/New_York", L("region.timeZone.city.newYork")},
          {"America/Chicago", L("region.timeZone.city.chicago")},
          {"America/Denver", L("region.timeZone.city.denver")},
          {"America/Los_Angeles", L("region.timeZone.city.losAngeles")},
          {"America/Sao_Paulo", L("region.timeZone.city.saoPaulo")},
          {"Australia/Sydney", L("region.timeZone.city.sydney")},
      });
  return *zones;
}

std::string Offset(const icu::TimeZone& zone, UDate now) {
  int32_t raw = 0;
  int32_t dst = 0;
  UErrorCode status = U_ZERO_ERROR;
  zone.getOffset(now, false, raw, dst, status);
  const int minutes = (raw + dst) / 60000;
  if (U_FAILURE(status) || minutes == 0) {
    return "GMT";
  }
  const int absolute = std::abs(minutes);
  const int remainder = absolute % 60;
  return base::StrCat({"GMT", minutes > 0 ? "+" : "-",
                       base::NumberToString(absolute / 60),
                       remainder ? (remainder < 10 ? ":0" : ":") : "",
                       remainder ? base::NumberToString(remainder) : ""});
}

std::u16string Label(std::string_view id, UDate now) {
  std::u16string city;
  for (const auto& common : CommonZones()) {
    if (id == common.id) {
      city = common.city;
      break;
    }
  }
  if (city.empty()) {
    std::string name(id.substr(id.find_last_of('/') + 1));
    std::replace(name.begin(), name.end(), '_', ' ');
    city = base::UTF8ToUTF16(name);
  }
  auto zone = CreateWebTimeZone(id);
  return L("region.timeZone.label",
           {{"city", city}, {"offset", zone ? Offset(*zone, now) : "GMT"}});
}

const std::set<std::string>& ZoneIds() {
  static const base::NoDestructor<std::set<std::string>> ids([] {
    std::set<std::string> result;
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::StringEnumeration> enumeration(
        icu::TimeZone::createEnumeration(status));
    while (U_SUCCESS(status) && enumeration) {
      const icu::UnicodeString* name = enumeration->snext(status);
      if (!name) {
        break;
      }
      icu::UnicodeString iana;
      UErrorCode canonical_status = U_ZERO_ERROR;
      icu::TimeZone::getIanaID(*name, iana, canonical_status);
      if (U_SUCCESS(canonical_status)) {
        std::string id;
        iana.toUTF8String(id);
        result.insert(std::move(id));
      }
    }
    return result;
  }());
  return *ids;
}

base::DictValue ZoneItem(const std::string& id,
                         const std::string& selected,
                         UDate now) {
  return base::DictValue()
      .Set("id", id)
      .Set("title", Label(id, now))
      .Set("checked", id == selected);
}

}  // namespace

const std::string& EffectiveWebTimeZone(Profile* profile) {
  profile = profile->GetOriginalProfile();
  auto* data = static_cast<EffectiveTimeZone*>(
      profile->GetUserData(&kEffectiveTimeZoneKey));
  if (!data) {
    std::string id = prefs::PrefsFor(profile)->GetString(prefs::kWebTimeZone);
    if (!CreateWebTimeZone(id)) {
      id.clear();
    }
    auto value = std::make_unique<EffectiveTimeZone>(std::move(id));
    data = value.get();
    profile->SetUserData(&kEffectiveTimeZoneKey, std::move(value));
  }
  return data->id;
}

std::string SystemTimeZoneId() {
  std::unique_ptr<icu::TimeZone> zone(icu::TimeZone::detectHostTimeZone());
  icu::UnicodeString id;
  zone->getID(id);
  std::string result;
  id.toUTF8String(result);
  return result;
}

std::u16string WebTimeZoneOffset(std::string_view id) {
  auto zone = CreateWebTimeZone(id);
  return base::UTF8ToUTF16(
      zone ? Offset(*zone, base::Time::Now().InMillisecondsFSinceUnixEpoch())
           : std::string("GMT"));
}

void AddWebTimeZoneSettings(Profile* profile, base::DictValue& state) {
  std::string selected =
      prefs::PrefsFor(profile)->GetString(prefs::kWebTimeZone);
  if (!CreateWebTimeZone(selected)) {
    selected.clear();
  }
  const UDate now = base::Time::Now().InMillisecondsFSinceUnixEpoch();
  std::unique_ptr<icu::TimeZone> system(icu::TimeZone::createDefault());
  icu::UnicodeString system_id;
  system->getID(system_id);
  std::string system_name;
  system_id.toUTF8String(system_name);
  const std::u16string system_label =
      L("region.timeZone.system", {{"zone", Label(system_name, now)}});

  base::ListValue menu;
  menu.Append(base::DictValue()
                  .Set("id", "")
                  .Set("title", system_label)
                  .Set("checked", selected.empty()));
  menu.Append(base::DictValue().Set("separator", true));
  menu.Append(base::DictValue().Set("header", L("region.timeZone.common")));
  for (const auto& common : CommonZones()) {
    menu.Append(ZoneItem(common.id, selected, now));
  }

  // 与引擎共用 ICU 的数据库，不依赖操作系统的时区名字。保留旧配置里的有效别名。
  std::set<std::string> ids = ZoneIds();
  if (!selected.empty()) {
    ids.insert(selected);
  }
  std::map<std::string, base::ListValue> regions;
  for (const auto& id : ids) {
    const size_t slash = id.find('/');
    regions[slash == std::string::npos ? "Other" : id.substr(0, slash)].Append(
        ZoneItem(id, selected, now));
  }
  base::ListValue groups;
  const auto add_region = [&](const char* id, const std::u16string& name) {
    auto found = regions.find(id);
    if (found == regions.end()) {
      return;
    }
    auto& items = found->second;
    std::ranges::sort(items, [](const base::Value& a, const base::Value& b) {
      return *a.GetDict().FindString("title") <
             *b.GetDict().FindString("title");
    });
    groups.Append(base::DictValue()
                      .Set("id", id)
                      .Set("title", name)
                      .Set("children", std::move(items)));
    regions.erase(found);
  };
  add_region("Asia", L("region.timeZone.group.asia"));
  add_region("Europe", L("region.timeZone.group.europe"));
  add_region("America", L("region.timeZone.group.america"));
  add_region("Africa", L("region.timeZone.group.africa"));
  add_region("Australia", L("region.timeZone.group.australia"));
  add_region("Pacific", L("region.timeZone.group.pacific"));
  add_region("Atlantic", L("region.timeZone.group.atlantic"));
  add_region("Indian", L("region.timeZone.group.indian"));
  add_region("Antarctica", L("region.timeZone.group.antarctica"));
  base::ListValue other;
  for (auto& [id, items] : regions) {
    for (auto& item : items) {
      other.Append(std::move(item));
    }
  }
  if (!other.empty()) {
    groups.Append(base::DictValue()
                      .Set("id", "Other")
                      .Set("title", L("region.timeZone.group.other"))
                      .Set("children", std::move(other)));
  }
  menu.Append(base::DictValue().Set("separator", true));
  menu.Append(base::DictValue()
                  .Set("id", "regions")
                  .Set("title", L("region.timeZone.byRegion"))
                  .Set("children", std::move(groups)));

  state.Set("webTimeZone", selected);
  state.Set("webTimeZoneLabel",
            selected.empty() ? system_label : Label(selected, now));
  state.Set("webTimeZoneMenu", std::move(menu));
  state.Set("webTimeZoneNeedsRelaunch",
            selected != EffectiveWebTimeZone(profile));
}

}  // namespace starry
