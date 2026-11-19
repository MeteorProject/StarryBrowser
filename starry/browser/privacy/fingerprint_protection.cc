// Copyright 2026 The Starry Browser Authors

#include "starry/browser/privacy/fingerprint_protection.h"

#include <array>
#include <memory>
#include <set>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/numerics/byte_conversions.h"
#include "base/rand_util.h"
#include "base/scoped_observation.h"
#include "base/strings/string_util.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/render_process_host.h"
#include "crypto/hmac.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/schemeful_site.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/privacy/fingerprint_client_hints.h"
#include "starry/browser/privacy/fingerprint_worker_site.h"
#include "starry/common/fingerprint_protection.h"
#include "starry/components/privacy/mojom/fingerprint_protection.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace starry {

namespace {

constexpr int kSessionKey = 0;

class FingerprintSession : public base::SupportsUserData::Data,
                           public ProfileObserver {
 public:
  explicit FingerprintSession(Profile* profile)
      : policy(base::ReadOnlySharedMemoryRegion::Create(
            sizeof(FingerprintPolicyVersion))),
        profile_(profile) {
    base::RandBytes(key);
    if (policy.IsValid()) {
      std::construct_at(policy.mapping.GetMemoryAs<FingerprintPolicyVersion>())
          ->enabled.store(FingerprintProtectionEnabled(profile),
                          std::memory_order_relaxed);
    }
    observation_.Observe(profile);
    prefs_.Init(prefs::PrefsFor(profile));
    const auto changed = base::BindRepeating(&FingerprintSession::Changed,
                                             base::Unretained(this));
    prefs_.Add(prefs::kFingerprintProtection, changed);
    prefs_.Add(prefs::kFingerprintProtectionExceptions, changed);
  }

  void OnProfileWillBeDestroyed(Profile*) override {
    prefs_.RemoveAll();
    observation_.Reset();
    profile_ = nullptr;
  }

  uint32_t generation() const {
    return policy.IsValid()
               ? policy.mapping.GetMemoryAs<FingerprintPolicyVersion>()
                     ->generation.load(std::memory_order_acquire)
               : 0;
  }

  std::array<uint8_t, 32> key;
  base::MappedReadOnlyRegion policy;

 private:
  void Changed() {
    if (!policy.IsValid()) {
      return;
    }
    auto* version = policy.mapping.GetMemoryAs<FingerprintPolicyVersion>();
    if (profile_) {
      version->enabled.store(FingerprintProtectionEnabled(profile_),
                             std::memory_order_relaxed);
    }
    version->generation.fetch_add(1, std::memory_order_release);
  }

  raw_ptr<Profile> profile_;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<Profile, ProfileObserver> observation_{this};
};

FingerprintSession& SessionFor(Profile* profile) {
  auto* session =
      static_cast<FingerprintSession*>(profile->GetUserData(&kSessionKey));
  if (!session) {
    auto data = std::make_unique<FingerprintSession>(profile);
    session = data.get();
    // Deliberately use this Profile, not GetOriginalProfile(): private browsing
    // gets a separate key, discarded when its Profile is destroyed.
    profile->SetUserData(&kSessionKey, std::move(data));
  }
  return *session;
}

uint64_t SeedFor(FingerprintSession& session, const net::SchemefulSite& site) {
  if (site.opaque()) {
    return base::RandUint64();
  }
  auto hash = crypto::hmac::SignSha256(session.key,
                                       base::as_byte_span(site.Serialize()));
  return base::U64FromLittleEndian(base::span(hash).first<8>());
}

bool Exempt(Profile* profile, const std::string& site) {
  if (site.empty()) {
    return false;
  }
  for (const auto& entry : prefs::PrefsFor(profile)->GetList(
           prefs::kFingerprintProtectionExceptions)) {
    if (entry.is_string() &&
        NormalizeFingerprintSite(entry.GetString()) == site) {
      return true;
    }
  }
  return false;
}

class FingerprintProtection : public mojom::FingerprintProtection {
 public:
  explicit FingerprintProtection(content::ChildProcessId process_id)
      : process_id_(process_id) {}

  void GetSeed(const blink::ExecutionContextToken& context,
               const url::Origin& top_origin,
               GetSeedCallback callback) override {
    auto* process = content::RenderProcessHost::FromID(process_id_);
    if (!process) {
      std::move(callback).Run(false, 0, 0, base::ReadOnlySharedMemoryRegion());
      return;
    }
    Profile* profile =
        Profile::FromBrowserContext(process->GetBrowserContext());
    if (profile->ShutdownStarted() ||
        profile->GetOriginalProfile()->ShutdownStarted()) {
      // The destruction notification has already fired. Do not install a new
      // preference observer after it; closing contexts receive isolated noise.
      std::move(callback).Run(true, base::RandUint64(), 0,
                              base::ReadOnlySharedMemoryRegion());
      return;
    }
    auto& session = SessionFor(profile);
    const auto reply = [&](bool enabled, uint64_t seed) {
      std::move(callback).Run(enabled, seed, session.generation(),
                              session.policy.IsValid()
                                  ? session.policy.region.Duplicate()
                                  : base::ReadOnlySharedMemoryRegion());
    };
    if (!FingerprintProtectionEnabled(profile)) {
      reply(false, 0);
      return;
    }

    net::SchemefulSite site(top_origin);
    if (context.Is<blink::DedicatedWorkerToken>() ||
        context.Is<blink::SharedWorkerToken>() ||
        context.Is<blink::ServiceWorkerToken>()) {
      // A worker can outlive its page. Resolve its partition in the browser,
      // rather than treating the worker script's own origin as its top site.
      const auto worker_site = FingerprintWorkerSite(*process, context);
      if (!worker_site) {
        // Unknown/closing contexts receive an isolated seed, never an exemption
        // belonging to a different website.
        reply(true, base::RandUint64());
        return;
      }
      site = *worker_site;
    }
    const GURL url = site.GetURL();
    if ((!site.opaque() && !url.SchemeIsHTTPOrHTTPS() && !url.SchemeIsFile()) ||
        Exempt(profile, NormalizeFingerprintSite(url.spec()))) {
      reply(false, 0);
      return;
    }
    reply(true, SeedFor(session, site));
  }

 private:
  const content::ChildProcessId process_id_;
};

}  // namespace

bool IsFingerprintProtectionMode(std::string_view mode) {
  return mode == "off" || mode == "privateBrowsing" || mode == "allBrowsing";
}

bool FingerprintProtectionEnabled(Profile* profile) {
  const std::string& mode =
      prefs::PrefsFor(profile)->GetString(prefs::kFingerprintProtection);
  return mode == "allBrowsing" || (mode != "off" && profile->IsOffTheRecord());
}

bool FingerprintProtectionEnabledForSite(Profile* profile, const GURL& url) {
  return FingerprintProtectionEnabled(profile) && url.SchemeIsHTTPOrHTTPS() &&
         !Exempt(profile, NormalizeFingerprintSite(url.spec()));
}

std::string NormalizeFingerprintSite(std::string_view input) {
  std::string text(base::TrimWhitespaceASCII(input, base::TRIM_ALL));
  GURL url(text);
  if (!url.is_valid() || !url.has_host()) {
    url = GURL("https://" + text);
  }
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !url.has_host()) {
    return {};
  }
  std::string host(url.host());
  while (host.ends_with('.')) {
    host.pop_back();
  }
  if (host.empty()) {
    return {};
  }
  return std::string(
      net::SchemefulSite(GURL("https://" + host)).GetURL().host());
}

void SetFingerprintProtectionException(Profile* profile,
                                       std::string_view input,
                                       bool exempt) {
  const std::string site = NormalizeFingerprintSite(input);
  if (site.empty()) {
    return;
  }
  PrefService* prefs = prefs::PrefsFor(profile);
  std::set<std::string> sites;
  for (const auto& entry :
       prefs->GetList(prefs::kFingerprintProtectionExceptions)) {
    if (entry.is_string()) {
      std::string normalized = NormalizeFingerprintSite(entry.GetString());
      if (!normalized.empty()) {
        sites.insert(std::move(normalized));
      }
    }
  }
  if (exempt) {
    sites.insert(site);
  } else {
    sites.erase(site);
  }
  base::ListValue list;
  for (const auto& name : sites) {
    list.Append(name);
  }
  prefs->SetList(prefs::kFingerprintProtectionExceptions, std::move(list));
}

void BindFingerprintProtection(
    content::ChildProcessId process_id,
    mojo::PendingReceiver<mojom::FingerprintProtection> receiver) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<FingerprintProtection>(process_id), std::move(receiver));
}

void RegisterFingerprintClientHints() {
  SetFingerprintClientHintsCheck(
      [](content::BrowserContext* context, const url::Origin& top_origin) {
        return FingerprintProtectionEnabledForSite(
            Profile::FromBrowserContext(context), top_origin.GetURL());
      });
}

}  // namespace starry
