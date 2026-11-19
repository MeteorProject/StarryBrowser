// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_PRIVACY_FINGERPRINT_PROTECTION_H_
#define STARRY_BROWSER_PRIVACY_FINGERPRINT_PROTECTION_H_

#include <string>
#include <string_view>

#include "content/public/common/child_process_id.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "starry/components/privacy/mojom/fingerprint_protection.mojom-forward.h"

class GURL;
class Profile;

namespace starry {

bool IsFingerprintProtectionMode(std::string_view mode);
bool FingerprintProtectionEnabled(Profile* profile);
bool FingerprintProtectionEnabledForSite(Profile* profile, const GURL& url);

// Exceptions use a registrable domain (including private registries), or an
// IP/local hostname. This matches the partition shared by frames and workers.
std::string NormalizeFingerprintSite(std::string_view input);
void SetFingerprintProtectionException(Profile* profile,
                                       std::string_view input,
                                       bool exempt);

void BindFingerprintProtection(
    content::ChildProcessId process_id,
    mojo::PendingReceiver<mojom::FingerprintProtection> receiver);

// Navigation client hints are added in //content; tell it which top-level
// sites are protected. Called once when the browser client is created.
void RegisterFingerprintClientHints();

}  // namespace starry

#endif  // STARRY_BROWSER_PRIVACY_FINGERPRINT_PROTECTION_H_
