// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMMON_WIN_INSTALL_MODES_H_
#define STARRY_COMMON_WIN_INSTALL_MODES_H_

#include <array>

#include "chrome/app/chrome_dll_resource.h"
#include "chrome/common/chrome_icon_resources_win.h"
#include "chrome/install_static/install_constants.h"
#include "starry/common/platform_branding.h"

namespace starry {

inline constexpr wchar_t kCompanyPathName[] = L"";
inline constexpr wchar_t kProductPathName[] = STARRY_PRODUCT_FULLNAME_W;
inline constexpr char kSafeBrowsingName[] = STARRY_PRODUCT_ID;

// The upstream installer resource generator uses the single-mode index name.
enum InstallConstantIndex {
  CHROMIUM_INDEX,
  NUM_INSTALL_MODES,
};

inline constexpr auto kInstallModes =
    std::to_array<install_static::InstallConstants>({{
        .size = sizeof(install_static::InstallConstants),
        .index = CHROMIUM_INDEX,
        .install_switch = "",
        .install_suffix = L"",
        .logo_suffix = L"",
        .app_guid = L"",
        .base_app_name = STARRY_PRODUCT_FULLNAME_W,
        .base_app_id = STARRY_WINDOWS_APP_ID,
        .browser_prog_id_prefix = STARRY_WINDOWS_HTML_PROG_ID,
        .browser_prog_id_description = STARRY_WINDOWS_HTML_DESCRIPTION,
        .direct_launch_url_scheme = STARRY_PRODUCT_ID,
        .pdf_prog_id_prefix = STARRY_WINDOWS_PDF_PROG_ID,
        .pdf_prog_id_description = STARRY_WINDOWS_PDF_DESCRIPTION,
        .active_setup_guid = STARRY_WINDOWS_ACTIVE_SETUP_GUID,
        .toast_activator_clsid = STARRY_WINDOWS_TOAST_CLSID,
        .elevator_clsid = STARRY_WINDOWS_ELEVATOR_CLSID,
        .elevator_iid = STARRY_WINDOWS_ELEVATOR2_IID,
        .old_elevator_iids = {},
        .tracing_service_clsid = STARRY_WINDOWS_TRACING_CLSID,
        .tracing_service_iid = STARRY_WINDOWS_TRACING_IID,
        .old_tracing_service_iids = {},
        .default_channel_name = L"",
        .channel_strategy = install_static::ChannelStrategy::UNSUPPORTED,
        .supports_system_level = true,
        .supports_set_as_default_browser = true,
        .app_icon_resource_index = icon_resources::kApplicationIndex,
        .app_icon_resource_id = IDR_MAINFRAME,
        .html_doc_icon_resource_index = icon_resources::kHtmlDocIndex,
        .pdf_doc_icon_resource_index = icon_resources::kPDFDocIndex,
        // Shared renderer sandbox capability, also used by Chrome stable/beta.
        // It is not a product registration ID.
        .sandbox_sid_prefix =
            L"S-1-15-2-3251537155-1984446955-2931258699-841473695-"
            L"1938553385-924012148-",
    }});

}  // namespace starry

#endif  // STARRY_COMMON_WIN_INSTALL_MODES_H_
