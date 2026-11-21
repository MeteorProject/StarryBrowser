// Copyright 2026 The Starry Browser Authors

#include "starry/browser/privacy/location_simulation.h"

#include <cmath>
#include <map>
#include <memory>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "net/base/url_util.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/privacy/fingerprint_protection.h"
#include "starry/browser/privacy/process_time_zone.h"
#include "starry/browser/privacy/simulated_geolocation.h"
#include "starry/browser/web_time_zone.h"
#include "starry/common/web_time_zone.h"
#include "url/gurl.h"

namespace starry {

namespace {

constexpr int kSessionKey = 0;
constexpr int kOffTheRecordKey = 0;
constexpr size_t kMaxLabelBytes = 256;

base::RepeatingClosureList& Observers() {
  static base::NoDestructor<base::RepeatingClosureList> observers;
  return *observers;
}

void NotifyChanged() {
  UpdateSimulatedGeolocation();
  UpdateProcessWebTimeZones();
  Observers().Notify();
}

class LocationSession : public base::SupportsUserData::Data,
                        public ProfileObserver {
 public:
  explicit LocationSession(Profile* profile) {
    observation_.Observe(profile);
    prefs_.Init(prefs::PrefsFor(profile));
    const auto changed = base::BindRepeating(&NotifyChanged);
    prefs_.Add(prefs::kLocationSimulation, changed);
    prefs_.Add(prefs::kSimulatedLocation, changed);
    prefs_.Add(prefs::kSimulatedLocationSites, changed);
  }

  void OnProfileWillBeDestroyed(Profile*) override {
    prefs_.RemoveAll();
    observation_.Reset();
  }

 private:
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<Profile, ProfileObserver> observation_{this};
};

void EnsureSession(Profile* profile) {
  Profile* original = profile->GetOriginalProfile();
  if (original->ShutdownStarted() || original->GetUserData(&kSessionKey)) {
    return;
  }
  original->SetUserData(&kSessionKey,
                        std::make_unique<LocationSession>(original));
}

// 无痕的用户资料上：这次无痕里改的网站设置，有就以它为准（包括「跟随全局」）。
class OffTheRecordRules : public base::SupportsUserData::Data {
 public:
  std::map<std::string, SiteLocationRule> rules;
};

OffTheRecordRules* OffTheRecordRulesFor(Profile* profile, bool create) {
  if (!profile->IsOffTheRecord()) {
    return nullptr;
  }
  auto* rules =
      static_cast<OffTheRecordRules*>(profile->GetUserData(&kOffTheRecordKey));
  if (!rules && create) {
    auto data = std::make_unique<OffTheRecordRules>();
    rules = data.get();
    profile->SetUserData(&kOffTheRecordKey, std::move(data));
  }
  return rules;
}

bool ValidCoordinate(std::optional<double> value, double limit) {
  return value && std::isfinite(*value) && std::abs(*value) <= limit;
}

base::DictValue RuleToValue(const SiteLocationRule& rule) {
  if (rule.mode == SiteLocationMode::kReal) {
    return base::DictValue().Set("mode", "real");
  }
  return LocationToValue(rule.location).Set("mode", "simulated");
}

SiteLocationRule RuleFromValue(const base::Value* value) {
  if (!value || !value->is_dict()) {
    return {};
  }
  const std::string* mode = value->GetDict().FindString("mode");
  if (mode && *mode == "real") {
    return {.mode = SiteLocationMode::kReal};
  }
  if (mode && *mode == "simulated") {
    if (auto location = LocationFromValue(value->GetDict())) {
      return {.mode = SiteLocationMode::kSimulated,
              .location = std::move(*location)};
    }
  }
  return {};
}

base::DictValue LocationState(Profile* profile,
                              const SimulatedLocation& location) {
  const std::string& zone = location.time_zone.empty()
                                ? EffectiveWebTimeZone(profile)
                                : location.time_zone;
  return LocationToValue(location)
      .Set("timeZoneOffset",
           zone.empty() ? std::u16string() : WebTimeZoneOffset(zone));
}

std::optional<SimulatedPosition> LookupPosition(
    content::BrowserContext* context,
    const GURL& top_url) {
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile || profile->ShutdownStarted() ||
      profile->GetOriginalProfile()->ShutdownStarted()) {
    return std::nullopt;
  }
  const LocationDecision decision = ResolveLocation(profile, top_url);
  if (!decision.simulated) {
    return std::nullopt;
  }
  SimulatedPosition position;
  if (decision.location) {
    position.latitude = decision.location->latitude;
    position.longitude = decision.location->longitude;
    position.accuracy = decision.location->accuracy;
    position.available = true;
  }
  return position;
}

}  // namespace

bool IsLocationSimulationMode(std::string_view mode) {
  return mode == "off" || mode == "privateBrowsing" || mode == "allBrowsing";
}

LocationDecision ResolveLocation(Profile* profile, const GURL& top_url) {
  if (!top_url.SchemeIsHTTPOrHTTPS()) {
    return {};
  }
  const std::string site = NormalizeFingerprintSite(top_url.spec());
  if (site.empty()) {
    return {};
  }
  EnsureSession(profile);
  SiteLocationRule rule = GetSiteLocationRule(profile, site);
  switch (rule.mode) {
    case SiteLocationMode::kReal:
      return {};
    case SiteLocationMode::kSimulated:
      return {.simulated = true, .location = std::move(rule.location)};
    case SiteLocationMode::kDefault:
      return ResolveDefaultLocation(profile);
  }
}

LocationDecision ResolveDefaultLocation(Profile* profile) {
  EnsureSession(profile);
  const std::string& mode =
      prefs::PrefsFor(profile)->GetString(prefs::kLocationSimulation);
  if (mode != "allBrowsing" &&
      !(mode == "privateBrowsing" && profile->IsOffTheRecord())) {
    return {};
  }
  return {.simulated = true, .location = GetGlobalSimulatedLocation(profile)};
}

std::optional<SimulatedLocation> GetGlobalSimulatedLocation(Profile* profile) {
  return LocationFromValue(
      prefs::PrefsFor(profile)->GetDict(prefs::kSimulatedLocation));
}

void SetGlobalSimulatedLocation(Profile* profile,
                                const SimulatedLocation& location) {
  EnsureSession(profile);
  prefs::PrefsFor(profile)->SetDict(prefs::kSimulatedLocation,
                                    LocationToValue(location));
}

SiteLocationRule GetSiteLocationRule(Profile* profile, std::string_view site) {
  const std::string normalized = NormalizeFingerprintSite(site);
  if (normalized.empty()) {
    return {};
  }
  if (OffTheRecordRules* rules = OffTheRecordRulesFor(profile, false)) {
    auto found = rules->rules.find(normalized);
    if (found != rules->rules.end()) {
      return found->second;
    }
  }
  return RuleFromValue(prefs::PrefsFor(profile)
                           ->GetDict(prefs::kSimulatedLocationSites)
                           .Find(normalized));
}

std::string NormalizeLocationSiteInput(std::string_view input) {
  const std::string text(base::TrimWhitespaceASCII(input, base::TRIM_ALL));
  if (text.empty() || text.front() == '/' || text.contains('\\')) {
    return {};
  }
  const GURL url(text.contains("://") ? text : "https://" + text);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() ||
      !(url.HostIsIPAddress() ||
        net::IsCanonicalizedHostCompliant(url.host()))) {
    return {};
  }
  return NormalizeFingerprintSite(url.spec());
}

void SetSiteLocationRule(Profile* profile,
                         std::string_view site,
                         const SiteLocationRule& rule) {
  // 已有规则仍可按原来的键删除。
  const std::string normalized = rule.mode == SiteLocationMode::kDefault
                                     ? NormalizeFingerprintSite(site)
                                     : NormalizeLocationSiteInput(site);
  if (normalized.empty()) {
    return;
  }
  EnsureSession(profile);
  if (OffTheRecordRules* rules = OffTheRecordRulesFor(profile, true)) {
    rules->rules[normalized] = rule;
    NotifyChanged();
    return;
  }
  PrefService* prefs = prefs::PrefsFor(profile);
  base::DictValue sites =
      prefs->GetDict(prefs::kSimulatedLocationSites).Clone();
  if (rule.mode == SiteLocationMode::kDefault) {
    sites.Remove(normalized);
  } else {
    sites.Set(normalized, RuleToValue(rule));
  }
  prefs->SetDict(prefs::kSimulatedLocationSites, std::move(sites));
}

base::DictValue LocationToValue(const SimulatedLocation& location) {
  return base::DictValue()
      .Set("latitude", location.latitude)
      .Set("longitude", location.longitude)
      .Set("accuracy", location.accuracy)
      .Set("timeZone", location.time_zone)
      .Set("label", location.label);
}

std::optional<SimulatedLocation> LocationFromValue(
    const base::DictValue& value) {
  const std::optional<double> latitude = value.FindDouble("latitude");
  const std::optional<double> longitude = value.FindDouble("longitude");
  if (!ValidCoordinate(latitude, 90) || !ValidCoordinate(longitude, 180)) {
    return std::nullopt;
  }
  SimulatedLocation location;
  location.latitude = *latitude;
  location.longitude = *longitude;
  const std::optional<double> accuracy = value.FindDouble("accuracy");
  if (accuracy && std::isfinite(*accuracy) && *accuracy >= 1 &&
      *accuracy <= 100000) {
    location.accuracy = *accuracy;
  }
  const std::string* time_zone = value.FindString("timeZone");
  if (time_zone && CreateWebTimeZone(*time_zone)) {
    location.time_zone = *time_zone;
  }
  if (const std::string* label = value.FindString("label")) {
    base::TruncateUTF8ToByteSize(*label, kMaxLabelBytes, &location.label);
  }
  return location;
}

void AddLocationSimulationSettings(Profile* profile, base::DictValue& state) {
  const std::string& mode =
      prefs::PrefsFor(profile)->GetString(prefs::kLocationSimulation);
  state.Set("locationSimulation",
            IsLocationSimulationMode(mode) ? mode : "off");
  if (auto location = GetGlobalSimulatedLocation(profile)) {
    state.Set("simulatedLocation", LocationState(profile, *location));
  }
  base::ListValue sites;
  for (const auto [site, value] :
       prefs::PrefsFor(profile)->GetDict(prefs::kSimulatedLocationSites)) {
    SiteLocationRule rule = RuleFromValue(&value);
    if (rule.mode == SiteLocationMode::kDefault ||
        NormalizeFingerprintSite(site) != site) {
      continue;
    }
    base::DictValue item;
    item.Set("site", site);
    if (rule.mode == SiteLocationMode::kReal) {
      item.Set("mode", "real");
    } else {
      item.Set("mode", "simulated");
      item.Set("location", LocationState(profile, rule.location));
    }
    sites.Append(std::move(item));
  }
  state.Set("siteLocations", std::move(sites));
}

base::CallbackListSubscription AddLocationSimulationObserver(
    base::RepeatingClosure callback) {
  return Observers().Add(std::move(callback));
}

void RegisterLocationSimulation() {
  SetSimulatedGeolocationLookup(&LookupPosition);
}

}  // namespace starry
