// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/tracking_parameters.h"

#include <string>
#include <vector>

#include "base/strings/escape.h"
#include "base/strings/string_util.h"

namespace starry::content_filter {

namespace {

constexpr std::string_view kNames[] = {
    // 广告平台的点击 ID
    "fbclid", "gclid", "dclid", "gbraid", "wbraid", "msclkid", "yclid",
    "twclid", "ttclid", "li_fat_id", "epik", "rdt_cid", "igshid", "igsh",
    "srsltid", "_gl", "_ga",
    // 邮件营销
    "mc_cid", "mc_eid", "_hsenc", "_hsmi", "__hssc", "__hstc", "__hsfp",
    "hsctatracking", "mkt_tok", "vero_id", "vero_conv", "ml_subscriber",
    "ml_subscriber_hash", "oly_anon_id", "oly_enc_id", "rb_clickid", "s_cid",
    "ss_email_id", "bsft_clkid", "bsft_uid", "wickedid", "_openstat", "__s",
    // 社交网站分享
    "ref_src", "ref_url",
};

constexpr std::string_view kPrefixes[] = {
    "utm_", "pk_", "piwik_", "matomo_", "hsa_", "oft_",
};

}  // namespace

bool IsTrackingParameter(std::string_view name) {
  const std::string lower = base::ToLowerASCII(name);
  for (std::string_view known : kNames) {
    if (lower == known) {
      return true;
    }
  }
  for (std::string_view prefix : kPrefixes) {
    if (lower.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

GURL RemoveQueryParameters(
    const GURL& url,
    base::FunctionRef<bool(std::string_view name, std::string_view value)>
        should_remove) {
  if (!url.SchemeIsHTTPOrHTTPS() || !url.has_query()) {
    return GURL();
  }
  const std::string_view query = url.query();
  std::vector<std::string_view> kept;
  bool removed = false;
  size_t start = 0;
  while (start <= query.size()) {
    size_t end = query.find('&', start);
    if (end == std::string_view::npos) {
      end = query.size();
    }
    const std::string_view part = query.substr(start, end - start);
    start = end + 1;
    if (part.empty()) {
      continue;
    }
    const size_t equals = part.find('=');
    const std::string_view name = part.substr(0, equals);
    const std::string_view value =
        equals == std::string_view::npos ? std::string_view()
                                         : part.substr(equals + 1);
    const std::string decoded = base::UnescapeBinaryURLComponent(name);
    if (should_remove(decoded, value)) {
      removed = true;
      continue;
    }
    kept.push_back(part);
  }
  if (!removed) {
    return GURL();
  }
  const std::string new_query = base::JoinString(kept, "&");
  GURL::Replacements replacements;
  if (new_query.empty()) {
    replacements.ClearQuery();
  } else {
    replacements.SetQueryStr(new_query);
  }
  return url.ReplaceComponents(replacements);
}

GURL StripTrackingParameters(const GURL& url) {
  return RemoveQueryParameters(
      url, [](std::string_view name, std::string_view) {
        return IsTrackingParameter(name);
      });
}

}  // namespace starry::content_filter
