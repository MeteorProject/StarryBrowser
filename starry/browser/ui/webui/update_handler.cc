// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/update_handler.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/updater/platform_updater.h"

namespace starry {

namespace {

using updater::AutoUpdateMode;
using updater::UpdaterService;

double JsTime(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

}  // namespace

UpdateHandler::UpdateHandler() = default;

UpdateHandler::~UpdateHandler() = default;

void UpdateHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getUpdateState", &UpdateHandler::HandleGetState);
  add("checkForUpdates", &UpdateHandler::HandleCheck);
  add("downloadUpdate", &UpdateHandler::HandleDownload);
  add("cancelUpdateDownload", &UpdateHandler::HandleCancelDownload);
  add("applyUpdate", &UpdateHandler::HandleApply);
  add("revealUpdatePackage", &UpdateHandler::HandleReveal);
  add("setAutoUpdateMode", &UpdateHandler::HandleSetMode);
  add("setUpdateMirror", &UpdateHandler::HandleSetMirror);
}

void UpdateHandler::OnJavascriptAllowed() {
  observation_.Observe(UpdaterService::Get());
}

void UpdateHandler::OnJavascriptDisallowed() {
  observation_.Reset();
}

void UpdateHandler::OnUpdateStateChanged() {
  FireWebUIListener("update-changed", base::Value(State()));
}

base::DictValue UpdateHandler::State() const {
  const UpdaterService::StateSnapshot s = UpdaterService::Get()->Snapshot();
  base::DictValue dict;
  dict.Set("status", UpdaterService::StatusName(s.status));
  dict.Set("mode", UpdaterService::ModeName(s.mode));
  dict.Set("installKind", updater::InstallKindName(s.install_kind));
  dict.Set("canAutoInstall", s.can_auto_install);
  dict.Set("currentVersion", s.current_version);
  dict.Set("availableVersion", s.available_version);
  dict.Set("releaseTag", s.release_tag);
  dict.Set("releaseUrl", s.release_url.spec());
  dict.Set("releaseNotes", s.release_notes);
  dict.Set("publishedAt", s.published_at);
  dict.Set("assetName", s.asset_name);
  dict.Set("lastChecked", JsTime(s.last_checked));
  dict.Set("customMirror", s.custom_mirror);
  dict.Set("activeRouteHost", s.active_route_host);
  dict.Set("bytesReceived", static_cast<double>(s.bytes_received));
  dict.Set("totalBytes", static_cast<double>(s.total_bytes));
  dict.Set("error", s.error_message);
  dict.Set("hasAttention", UpdaterService::Get()->HasAttention());
  return dict;
}

void UpdateHandler::HandleGetState(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(State()));
}

void UpdateHandler::HandleCheck(const base::ListValue& args) {
  UpdaterService::Get()->CheckNow(/*user_initiated=*/true);
}

void UpdateHandler::HandleDownload(const base::ListValue& args) {
  UpdaterService::Get()->DownloadUpdate();
}

void UpdateHandler::HandleCancelDownload(const base::ListValue& args) {
  UpdaterService::Get()->CancelDownload();
}

void UpdateHandler::HandleApply(const base::ListValue& args) {
  UpdaterService::Get()->ApplyUpdate();
}

void UpdateHandler::HandleReveal(const base::ListValue& args) {
  UpdaterService::Get()->RevealStagedPackage();
}

void UpdateHandler::HandleSetMode(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  if (std::optional<AutoUpdateMode> mode =
          UpdaterService::ModeFromName(args[0].GetString())) {
    UpdaterService::Get()->SetAutoUpdateMode(*mode);
  }
}

void UpdateHandler::HandleSetMirror(const base::ListValue& args) {
  AllowJavascript();
  if (args.size() < 2 || !args[1].is_string()) {
    return;
  }
  const std::string error =
      UpdaterService::Get()->SetCustomMirror(args[1].GetString());
  base::DictValue result;
  result.Set("error", error);
  ResolveJavascriptCallback(args[0], base::Value(std::move(result)));
}

}  // namespace starry
