// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/extensions_handler.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/extensions/commands/command_service.h"
#include "chrome/browser/extensions/devtools_util.h"
#include "chrome/browser/extensions/extension_commands_global_registry.h"
#include "chrome/browser/extensions/extension_install_prompt.h"
#include "chrome/browser/extensions/extension_management.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/extensions/updater/extension_updater.h"
#include "chrome/browser/extensions/webstore_install_with_prompt.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/extensions/extension_enable_flow.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "chrome/browser/ui/webui/extensions/extension_icon_source.h"
#include "chrome/common/pref_names.h"
#include "components/crx_file/id_util.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "extensions/browser/crx_installer.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extension_util.h"
#include "extensions/browser/install/crx_install_error.h"
#include "extensions/browser/install_prompt_data.h"
#include "extensions/browser/management_policy.h"
#include "extensions/browser/permissions/scripting_permissions_modifier.h"
#include "extensions/browser/permissions_manager.h"
#include "extensions/browser/process_manager.h"
#include "extensions/browser/ui_util.h"
#include "extensions/browser/unpacked_installer.h"
#include "extensions/browser/user_script_manager.h"
#include "extensions/common/command.h"
#include "extensions/common/extension.h"
#include "extensions/common/icons/extension_icon_set.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_handlers/background_info.h"
#include "extensions/common/manifest_handlers/description_info.h"
#include "extensions/common/manifest_handlers/options_page_info.h"
#include "extensions/common/manifest_handlers/manifest_url_handlers.h"
#include "extensions/common/permissions/permission_message.h"
#include "extensions/common/permissions/permission_set.h"
#include "extensions/common/permissions/permissions_data.h"
#include "starry/browser/l10n/l10n.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "url/gurl.h"

namespace starry {

namespace {

using extensions::Extension;
using extensions::ExtensionRegistry;

// 一连串的变化（重新载入、一次改几项）合成一次通知。
constexpr base::TimeDelta kChangedDelay = base::Milliseconds(60);

// 用户目录换成 ~（同设置页的下载位置）。
std::string DisplayPath(const base::FilePath& path) {
  const base::FilePath home = base::GetHomeDir();
  base::FilePath relative;
  if (!home.empty() && home.AppendRelativePath(path, &relative)) {
    return base::StrCat({"~/", relative.AsUTF8Unsafe()});
  }
  return path.AsUTF8Unsafe();
}

std::string SourceName(const Extension& extension, Profile* profile) {
  const auto location = extension.location();
  if (extensions::Manifest::IsUnpackedLocation(location)) {
    return "unpacked";
  }
  if (extensions::Manifest::IsPolicyLocation(location)) {
    return "policy";
  }
  if (extensions::Manifest::IsComponentLocation(location)) {
    return "component";
  }
  if (extensions::ExtensionManagementFactory::GetForBrowserContext(profile)
          ->UpdatesFromWebstore(extension)) {
    return "store";
  }
  return "other";
}

// 停用的原因里要用户知道的那一个，和对应的操作（restart、approve、
// developerMode，没有时为空）。
std::pair<std::u16string, std::string> ProblemOf(const Extension& extension,
                                                  Profile* profile) {
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
  const std::string& id = extension.id();
  if (registry->terminated_extensions().Contains(id)) {
    return {L("extensions.problem.terminated"), "restart"};
  }
  if (registry->blocklisted_extensions().Contains(id)) {
    return {L("extensions.problem.blocklisted"), ""};
  }
  if (registry->enabled_extensions().Contains(id)) {
    return {};
  }
  using namespace extensions::disable_reason;
  const extensions::DisableReasonSet reasons =
      extensions::ExtensionPrefs::Get(profile)->GetDisableReasons(id);
  if (reasons.contains(DISABLE_PERMISSIONS_INCREASE)) {
    return {L("extensions.problem.permissionsIncrease"), "approve"};
  }
  if (reasons.contains(DISABLE_CORRUPTED)) {
    return {L("extensions.problem.corrupted"), ""};
  }
  if (reasons.contains(DISABLE_NOT_VERIFIED) ||
      reasons.contains(DISABLE_NOT_ALLOWLISTED)) {
    return {L("extensions.problem.notVerified"), ""};
  }
  if (reasons.contains(DISABLE_GREYLIST)) {
    return {L("extensions.problem.greylist"), ""};
  }
  if (reasons.contains(DISABLE_BLOCKED_BY_POLICY)) {
    return {L("extensions.problem.blockedByPolicy"), ""};
  }
  if (reasons.contains(DISABLE_UPDATE_REQUIRED_BY_POLICY)) {
    return {L("extensions.problem.updateRequired"), ""};
  }
  if (reasons.contains(DISABLE_PUBLISHED_IN_STORE_REQUIRED_BY_POLICY)) {
    return {L("extensions.problem.storeRequired"), ""};
  }
  if (reasons.contains(DISABLE_UNSUPPORTED_MANIFEST_VERSION)) {
    return {L("extensions.problem.manifestV2"), ""};
  }
  if (reasons.contains(DISABLE_UNSUPPORTED_DEVELOPER_EXTENSION)) {
    return {L("extensions.problem.developerMode"), "developerMode"};
  }
  if (reasons.contains(DISABLE_UNSUPPORTED_REQUIREMENT)) {
    return {L("extensions.problem.requirement"), ""};
  }
  return {};
}

base::DictValue CommandEntry(const ui::Command& command,
                             bool active,
                             bool is_action) {
  base::DictValue entry;
  entry.Set("name", command.command_name());
  entry.Set("description",
            is_action || command.description().empty()
                ? L("extensions.shortcut.activate")
                : command.description());
  entry.Set("shortcut", active && command.accelerator().key_code() !=
                                      ui::VKEY_UNKNOWN
                            ? command.accelerator().GetShortcutText()
                            : std::u16string());
  entry.Set("global", command.global());
  return entry;
}

base::ListValue Commands(Profile* profile, const std::string& id) {
  extensions::CommandService* service =
      extensions::CommandService::Get(profile);
  base::ListValue commands;
  for (auto type : {extensions::ActionInfo::Type::kBrowser,
                    extensions::ActionInfo::Type::kPage,
                    extensions::ActionInfo::Type::kAction}) {
    bool active = false;
    extensions::Command command;
    if (service->GetExtensionActionCommand(id, type,
                                           extensions::CommandService::ALL,
                                           &command, &active)) {
      commands.Append(CommandEntry(command, active, /*is_action=*/true));
    }
  }
  ui::CommandMap named;
  if (service->GetNamedCommands(id, extensions::CommandService::ALL,
                                extensions::CommandService::ANY_SCOPE,
                                &named)) {
    for (auto& [name, command] : named) {
      // 同 Chrome 的扩展页：快捷键以 FindCommandByName 为准（用户改过的）。
      const extensions::Command current =
          service->FindCommandByName(id, command.command_name());
      command.set_accelerator(current.accelerator());
      command.set_global(current.global());
      commands.Append(CommandEntry(
          command, command.accelerator().key_code() != ui::VKEY_UNKNOWN,
          /*is_action=*/false));
    }
  }
  return commands;
}

base::ListValue PermissionList(const Extension& extension) {
  base::ListValue list;
  for (const extensions::PermissionMessage& message :
       extension.permissions_data()->GetPermissionMessages()) {
    base::DictValue entry;
    entry.Set("message", message.message());
    base::ListValue details;
    for (const std::u16string& detail : message.submessages()) {
      details.Append(detail);
    }
    entry.Set("details", std::move(details));
    list.Append(std::move(entry));
  }
  return list;
}

// 网站访问（Chrome 的「点击时」「在特定网站上」「在所有网站上」），见
// extension_info_generator.cc 的 CreateRuntimeHostPermissionsInfo。扩展没有可以
// 收回的网站权限时为空。
std::string SiteAccessMode(const Extension& extension, Profile* profile) {
  extensions::PermissionsManager* manager =
      extensions::PermissionsManager::Get(profile);
  if (!manager->CanAffectExtension(extension)) {
    return "";
  }
  if (!manager->HasWithheldHostPermissions(extension)) {
    return "all";
  }
  std::unique_ptr<const extensions::PermissionSet> granted =
      extensions::ExtensionPrefs::Get(profile)->GetRuntimeGrantedPermissions(
          extension.id());
  if (!granted || granted->effective_hosts().is_empty()) {
    return "onClick";
  }
  return granted->ShouldWarnAllHosts(false) ? "all" : "specific";
}

std::string BackgroundKind(const Extension& extension) {
  if (extensions::BackgroundInfo::IsServiceWorkerBased(&extension)) {
    return "worker";
  }
  if (extensions::BackgroundInfo::HasBackgroundPage(&extension)) {
    return "page";
  }
  return "";
}

// 应用店的网址（chromewebstore.google.com/detail/名字/<ID>）或者直接是 ID。
std::string StoreIdFrom(std::string_view input) {
  const std::string text = base::ToLowerASCII(base::TrimWhitespaceASCII(
      input, base::TRIM_ALL));
  constexpr size_t kIdLength = 32;
  if (text.size() < kIdLength) {
    return "";
  }
  for (size_t start = 0; start + kIdLength <= text.size(); ++start) {
    const std::string candidate = text.substr(start, kIdLength);
    const bool boundary_before =
        start == 0 || !base::IsAsciiAlpha(text[start - 1]);
    const bool boundary_after = start + kIdLength == text.size() ||
                                !base::IsAsciiAlpha(text[start + kIdLength]);
    if (boundary_before && boundary_after &&
        crx_file::id_util::IdIsValid(candidate)) {
      return candidate;
    }
  }
  return "";
}

}  // namespace

ExtensionsHandler::ExtensionsHandler(Profile* profile)
    : profile_(profile->GetOriginalProfile()) {}

ExtensionsHandler::~ExtensionsHandler() {
  if (select_file_dialog_) {
    select_file_dialog_->ListenerDestroyed();
  }
  if (shortcuts_suspended_) {
    extensions::ExtensionCommandsGlobalRegistry::Get(profile_)
        ->SetShortcutHandlingSuspended(false);
  }
}

void ExtensionsHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getExtensions", &ExtensionsHandler::HandleGetExtensions);
  add("extensionAction", &ExtensionsHandler::HandleExtensionAction);
  add("setShortcut", &ExtensionsHandler::HandleSetShortcut);
  add("suspendShortcuts", &ExtensionsHandler::HandleSuspendShortcuts);
  add("addExtension", &ExtensionsHandler::HandleAddExtension);
  add("checkUpdates", &ExtensionsHandler::HandleCheckUpdates);
}

void ExtensionsHandler::OnJavascriptAllowed() {
  registry_observation_.Observe(ExtensionRegistry::Get(profile_));
  prefs_observation_.Observe(extensions::ExtensionPrefs::Get(profile_));
  if (ToolbarActionsModel* model = ToolbarActionsModel::Get(profile_)) {
    toolbar_observation_.Observe(model);
  }
  pref_registrar_.Init(profile_->GetPrefs());
  pref_registrar_.Add(::prefs::kExtensionCommands,
                      base::BindRepeating(&ExtensionsHandler::ScheduleChanged,
                                          base::Unretained(this)));
}

void ExtensionsHandler::OnJavascriptDisallowed() {
  registry_observation_.Reset();
  prefs_observation_.Reset();
  toolbar_observation_.Reset();
  pref_registrar_.RemoveAll();
  changed_timer_.Stop();
  weak_factory_.InvalidateWeakPtrs();
}

void ExtensionsHandler::OnExtensionLoaded(
    content::BrowserContext* browser_context,
    const Extension* extension) {
  ScheduleChanged();
}

void ExtensionsHandler::OnExtensionUnloaded(
    content::BrowserContext* browser_context,
    const Extension* extension,
    extensions::UnloadedExtensionReason reason) {
  ScheduleChanged();
}

void ExtensionsHandler::OnExtensionInstalled(
    content::BrowserContext* browser_context,
    const Extension* extension,
    bool is_update) {
  ScheduleChanged();
}

void ExtensionsHandler::OnExtensionUninstalled(
    content::BrowserContext* browser_context,
    const Extension* extension,
    extensions::UninstallReason reason) {
  ScheduleChanged();
}

void ExtensionsHandler::OnExtensionDisableReasonsChanged(
    const extensions::ExtensionId& extension_id,
    extensions::DisableReasonSet disabled_reasons) {
  ScheduleChanged();
}

void ExtensionsHandler::OnExtensionPrefsUpdated(
    const extensions::ExtensionId& extension_id) {
  ScheduleChanged();
}

void ExtensionsHandler::OnExtensionRuntimePermissionsChanged(
    const extensions::ExtensionId& extension_id) {
  ScheduleChanged();
}

void ExtensionsHandler::OnToolbarActionAdded(
    const ToolbarActionsModel::ActionId& id) {
  ScheduleChanged();
}

void ExtensionsHandler::OnToolbarActionRemoved(
    const ToolbarActionsModel::ActionId& id) {
  ScheduleChanged();
}

void ExtensionsHandler::OnToolbarActionUpdated(
    const ToolbarActionsModel::ActionId& id) {
  // 图标、徽标的变化，页面上不显示。
}

void ExtensionsHandler::OnToolbarModelInitialized() {
  ScheduleChanged();
}

void ExtensionsHandler::OnToolbarPinnedActionsChanged() {
  ScheduleChanged();
}

void ExtensionsHandler::ScheduleChanged() {
  if (!IsJavascriptAllowed() || changed_timer_.IsRunning()) {
    return;
  }
  changed_timer_.Start(FROM_HERE, kChangedDelay,
                       base::BindOnce(
                           [](ExtensionsHandler* handler) {
                             if (handler->IsJavascriptAllowed()) {
                               handler->FireWebUIListener(
                                   "extensions-changed",
                                   base::Value(handler->List()));
                             }
                           },
                           base::Unretained(this)));
}

void ExtensionsHandler::Notice(const std::u16string& text, bool error) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  base::DictValue notice;
  notice.Set("text", text);
  notice.Set("error", error);
  FireWebUIListener("extensions-notice", base::Value(std::move(notice)));
}

base::ListValue ExtensionsHandler::List() {
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile_);
  std::vector<scoped_refptr<const Extension>> list;
  for (const auto& extension : registry->GenerateInstalledExtensionsSet(
           ExtensionRegistry::ENABLED | ExtensionRegistry::DISABLED |
           ExtensionRegistry::TERMINATED | ExtensionRegistry::BLOCKLISTED)) {
    if (extensions::ui_util::ShouldDisplayInExtensionSettings(*extension)) {
      list.push_back(extension);
    }
  }
  std::ranges::sort(list, [](const auto& a, const auto& b) {
    return base::ToLowerASCII(a->name()) < base::ToLowerASCII(b->name());
  });
  base::ListValue out;
  for (const auto& extension : list) {
    out.Append(Describe(*extension));
  }
  return out;
}

base::DictValue ExtensionsHandler::Describe(const Extension& extension) {
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile_);
  extensions::ManagementPolicy* policy =
      extensions::ExtensionSystem::Get(profile_)->management_policy();
  const std::string& id = extension.id();
  const bool enabled = registry->enabled_extensions().Contains(id);
  const bool terminated = registry->terminated_extensions().Contains(id);
  const bool blocklisted = registry->blocklisted_extensions().Contains(id);

  base::DictValue entry;
  entry.Set("id", id);
  entry.Set("name", extension.name());
  entry.Set("version", extension.VersionString());
  entry.Set("description",
            extensions::DescriptionInfo::GetDescription(extension));
  entry.Set("enabled", enabled);
  entry.Set("icon", extensions::ExtensionIconSource::GetIconURL(
                        &extension, /*icon_size=*/48,
                        ExtensionIconSet::Match::kBigger,
                        /*grayscale=*/false)
                        .spec());
  entry.Set("hasOptions",
            extensions::OptionsPageInfo::HasOptionsPage(&extension));
  const bool user_may_modify =
      policy->UserMayModifySettings(&extension, nullptr);
  entry.Set("mayToggle", user_may_modify && !terminated && !blocklisted &&
                             !policy->MustRemainEnabled(&extension, nullptr) &&
                             !policy->MustRemainDisabled(&extension, nullptr));
  entry.Set("mayUninstall",
            user_may_modify &&
                !policy->MustRemainInstalled(&extension, nullptr));

  const std::string source = SourceName(extension, profile_);
  entry.Set("source", source);
  entry.Set("path", source == "unpacked" ? DisplayPath(extension.path()) : "");
  auto [problem, problem_action] = ProblemOf(extension, profile_);
  entry.Set("problem", problem);
  entry.Set("problemAction", problem_action);
  entry.Set("terminated", terminated);

  // 侧栏上的扩展按钮（Chrome 工具栏的「固定」）。
  ToolbarActionsModel* toolbar = ToolbarActionsModel::Get(profile_);
  const bool pinnable = toolbar && toolbar->HasAction(id);
  entry.Set("pinnable", pinnable);
  entry.Set("pinned", pinnable && toolbar->IsActionPinned(id));
  entry.Set("forcePinned", pinnable && toolbar->IsActionForcePinned(id));

  base::DictValue incognito;
  incognito.Set("available", extensions::util::CanBeIncognitoEnabled(&extension));
  incognito.Set("allowed", extensions::util::IsIncognitoEnabled(id, profile_));
  entry.Set("incognito", std::move(incognito));

  base::DictValue file_access;
  file_access.Set("available", extension.wants_file_access());
  file_access.Set("allowed", extensions::util::AllowFileAccess(id, profile_));
  entry.Set("fileAccess", std::move(file_access));

  base::DictValue user_scripts;
  extensions::UserScriptManager* scripts =
      extensions::ExtensionSystem::Get(profile_)->user_script_manager();
  user_scripts.Set(
      "available",
      scripts &&
          extensions::UserScriptManager::IsUserScriptsAPIPermissionAvailable(
              extension));
  user_scripts.Set("allowed", scripts && scripts->AreUserScriptsAllowed(extension));
  entry.Set("userScripts", std::move(user_scripts));

  entry.Set("siteAccess",
            enabled ? SiteAccessMode(extension, profile_) : std::string());
  entry.Set("commands", Commands(profile_, id));
  entry.Set("permissions", PermissionList(extension));
  entry.Set("background", BackgroundKind(extension));
  entry.Set("homepage",
            extensions::ManifestURL::SpecifiedHomepageURL(&extension)
                ? extensions::ManifestURL::GetHomepageURL(&extension).spec()
                : std::string());
  entry.Set("storeUrl",
            source == "store"
                ? extensions::ManifestURL::GetDetailsURL(&extension).spec()
                : std::string());
  return entry;
}

void ExtensionsHandler::HandleGetExtensions(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(List()));
}

void ExtensionsHandler::HandleExtensionAction(const base::ListValue& args) {
  if (args.size() < 2 || !args[0].is_string() || !args[1].is_string()) {
    return;
  }
  const std::string id = args[0].GetString();
  const std::string& action = args[1].GetString();
  const base::Value* value = args.size() > 2 ? &args[2] : nullptr;
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile_);
  const Extension* extension = registry->GetInstalledExtension(id);
  if (!extension) {
    return;
  }
  extensions::ExtensionRegistrar* registrar =
      extensions::ExtensionRegistrar::Get(profile_);
  content::WebContents* contents = web_ui()->GetWebContents();
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;

  const auto start_enable_flow = [&]() {
    // 新版本要更多权限：Chrome 的流程先问用户（安装提示框），同意了才启用。
    enable_flow_ = std::make_unique<ExtensionEnableFlow>(profile_, id, this);
    enable_flow_->StartForWebContents(contents);
  };

  if (action == "enable") {
    if (extensions::ExtensionPrefs::Get(profile_)
            ->GetDisableReasons(id)
            .contains(extensions::disable_reason::DISABLE_PERMISSIONS_INCREASE)) {
      start_enable_flow();
      return;
    }
    registrar->EnableExtension(id);
  } else if (action == "approve") {
    start_enable_flow();
  } else if (action == "disable") {
    registrar->DisableExtension(
        id, {extensions::disable_reason::DISABLE_USER_ACTION});
  } else if (action == "uninstall") {
    std::u16string error;
    if (!registrar->UninstallExtension(
            id, extensions::UNINSTALL_REASON_USER_INITIATED, &error) &&
        !error.empty()) {
      Notice(error, /*error=*/true);
    }
  } else if (action == "reload" || action == "restart") {
    registrar->ReloadExtension(id);
  } else if (action == "developerMode") {
    // Chrome 把放在用户目录里的未打包扩展（kUnpacked）在开发者模式关着时停用。
    // Starry 没有开发者模式的开关，打开后它们照常启用。
    profile_->GetPrefs()->SetBoolean(::prefs::kExtensionsUIDeveloperMode, true);
  } else if (action == "options") {
    if (browser) {
      extensions::ExtensionTabUtil::OpenOptionsPage(extension, browser);
    }
  } else if (action == "inspect") {
    const Extension* running = registry->enabled_extensions().GetByID(id);
    if (!running) {
      return;
    }
    if (extensions::BackgroundInfo::IsServiceWorkerBased(running)) {
      const bool active = !extensions::ProcessManager::Get(profile_)
                               ->GetServiceWorkersForExtension(id)
                               .empty();
      if (active) {
        extensions::devtools_util::InspectServiceWorkerBackground(
            running, profile_, DevToolsOpenedByAction::kInspectLink);
      } else {
        extensions::devtools_util::InspectInactiveServiceWorkerBackground(
            running, profile_, DevToolsOpenedByAction::kInspectLink);
      }
    } else if (extensions::BackgroundInfo::HasBackgroundPage(running)) {
      extensions::devtools_util::InspectBackgroundPage(
          running, profile_, DevToolsOpenedByAction::kInspectLink);
    }
  } else if (action == "pin" && value && value->is_bool()) {
    ToolbarActionsModel* toolbar = ToolbarActionsModel::Get(profile_);
    if (toolbar && toolbar->HasAction(id) &&
        toolbar->IsActionPinned(id) != value->GetBool()) {
      toolbar->SetActionVisibility(id, value->GetBool());
    }
  } else if (action == "incognito" && value && value->is_bool()) {
    // 会重新载入扩展，extension 之后不能再用。
    extensions::util::SetIsIncognitoEnabled(id, profile_, value->GetBool());
  } else if (action == "fileAccess" && value && value->is_bool()) {
    extensions::util::SetAllowFileAccess(id, profile_, value->GetBool());
  } else if (action == "userScripts" && value && value->is_bool()) {
    if (extensions::UserScriptManager* scripts =
            extensions::ExtensionSystem::Get(profile_)->user_script_manager()) {
      scripts->SetUserScriptPrefEnabled(id, value->GetBool());
    }
    ScheduleChanged();
  } else if (action == "siteAccess" && value && value->is_string()) {
    if (!extensions::PermissionsManager::Get(profile_)->CanAffectExtension(
            *extension)) {
      return;
    }
    extensions::ScriptingPermissionsModifier modifier(profile_, extension);
    if (value->GetString() == "onClick") {
      modifier.SetWithholdHostPermissions(true);
      modifier.RemoveAllGrantedHostPermissions();
    } else if (value->GetString() == "all") {
      modifier.SetWithholdHostPermissions(false);
    }
    ScheduleChanged();
  }
}

void ExtensionsHandler::HandleSetShortcut(const base::ListValue& args) {
  AllowJavascript();
  const base::Value& callback_id = args[0];
  if (args.size() < 4 || !args[1].is_string() || !args[2].is_string() ||
      !args[3].is_string()) {
    ResolveJavascriptCallback(callback_id, base::Value());
    return;
  }
  const std::string& id = args[1].GetString();
  const std::string& command_name = args[2].GetString();
  const std::string& keys = args[3].GetString();
  base::DictValue result;
  if (!ExtensionRegistry::Get(profile_)->GetInstalledExtension(id)) {
    ResolveJavascriptCallback(callback_id, base::Value(std::move(result)));
    return;
  }
  extensions::CommandService* service =
      extensions::CommandService::Get(profile_);
  if (keys.empty()) {
    service->RemoveKeybindingPrefs(id, command_name);
    result.Set("shortcut", "");
  } else {
    const ui::Accelerator accelerator =
        extensions::Command::StringToAccelerator(keys, command_name);
    if (accelerator.key_code() == ui::VKEY_UNKNOWN) {
      result.Set("error", L("extensions.shortcut.invalid"));
    } else {
      // 别的扩展用着同一个组合键时，Chrome 把它让给这一个（同 Chrome 的扩展页）。
      service->UpdateKeybindingPrefs(id, command_name, keys);
      result.Set("shortcut", accelerator.GetShortcutText());
    }
  }
  ResolveJavascriptCallback(callback_id, base::Value(std::move(result)));
  ScheduleChanged();
}

void ExtensionsHandler::HandleSuspendShortcuts(const base::ListValue& args) {
  if (args.empty() || !args[0].is_bool()) {
    return;
  }
  shortcuts_suspended_ = args[0].GetBool();
  extensions::ExtensionCommandsGlobalRegistry::Get(profile_)
      ->SetShortcutHandlingSuspended(shortcuts_suspended_);
}

void ExtensionsHandler::HandleAddExtension(const base::ListValue& args) {
  AllowJavascript();
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  const std::string& kind = args[0].GetString();
  if (kind == "unpacked") {
    ChooseFile(FileKind::kUnpacked);
  } else if (kind == "crx") {
    ChooseFile(FileKind::kCrx);
  } else if (kind == "store" && args.size() > 1 && args[1].is_string()) {
    InstallFromStore(args[1].GetString());
  }
}

void ExtensionsHandler::ChooseFile(FileKind kind) {
  if (select_file_dialog_) {
    return;
  }
  content::WebContents* contents = web_ui()->GetWebContents();
  file_kind_ = kind;
  select_file_dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(contents));
  if (kind == FileKind::kUnpacked) {
    select_file_dialog_->SelectFile(
        ui::SelectFileDialog::SELECT_EXISTING_FOLDER,
        L("extensions.unpacked.message"), base::FilePath(), nullptr, 0,
        base::FilePath::StringType(), contents->GetTopLevelNativeWindow());
    return;
  }
  ui::SelectFileDialog::FileTypeInfo types;
  types.extensions = {{FILE_PATH_LITERAL("crx")}};
  types.include_all_files = false;
  select_file_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_OPEN_FILE,
      L("extensions.crxPanel.message"),
      base::FilePath(), &types, 1, FILE_PATH_LITERAL("crx"),
      contents->GetTopLevelNativeWindow());
}

void ExtensionsHandler::FileSelected(const ui::SelectedFileInfo& file,
                                     int index) {
  select_file_dialog_ = nullptr;
  const FileKind kind = std::exchange(file_kind_, FileKind::kNone);
  if (kind == FileKind::kUnpacked) {
    LoadUnpacked(file.path());
  } else if (kind == FileKind::kCrx) {
    InstallCrx(file.path());
  }
}

void ExtensionsHandler::FileSelectionCanceled() {
  select_file_dialog_ = nullptr;
  file_kind_ = FileKind::kNone;
}

void ExtensionsHandler::LoadUnpacked(const base::FilePath& path) {
  // 同 Chrome：放在用户目录里的未打包扩展要开发者模式（见 HandleExtensionAction 的
  // developerMode）。Starry 没有这个开关，载入时打开。
  profile_->GetPrefs()->SetBoolean(::prefs::kExtensionsUIDeveloperMode, true);
  scoped_refptr<extensions::UnpackedInstaller> installer =
      extensions::UnpackedInstaller::Create(profile_);
  installer->set_be_noisy_on_failure(false);
  installer->set_completion_callback(base::BindOnce(
      &ExtensionsHandler::OnUnpackedLoaded, weak_factory_.GetWeakPtr()));
  installer->Load(path);
}

void ExtensionsHandler::OnUnpackedLoaded(const Extension* extension,
                                         const base::FilePath& path,
                                         const std::u16string& error) {
  if (extension) {
    Notice(L("extensions.notice.loaded", {{"name", extension->name()}}),
           /*error=*/false);
    return;
  }
  Notice(L("extensions.error.loadUnpackedFailed",
           {{"name", path.BaseName().LossyDisplayName()}, {"error", error}}),
         /*error=*/true);
}

void ExtensionsHandler::InstallCrx(const base::FilePath& path) {
  content::WebContents* contents = web_ui()->GetWebContents();
  auto prompt = std::make_unique<ExtensionInstallPrompt>(
      contents, std::make_unique<extensions::InstallPromptData>(
                    extensions::InstallPromptData::UNSET_PROMPT_TYPE));
  scoped_refptr<extensions::CrxInstaller> installer =
      extensions::CrxInstaller::Create(profile_, std::move(prompt));
  installer->set_error_on_unsupported_requirements(true);
  installer->set_off_store_install_allow_reason(
      extensions::CrxInstaller::OffStoreInstallAllowedFromSettingsPage);
  installer->set_install_immediately(true);
  installer->AddInstallerCallback(
      base::BindOnce(&ExtensionsHandler::OnCrxInstalled,
                     weak_factory_.GetWeakPtr(),
                     path.BaseName().LossyDisplayName()));
  installer->InstallCrx(path);
}

void ExtensionsHandler::OnCrxInstalled(
    const std::u16string& file_name,
    const std::optional<extensions::CrxInstallError>& error) {
  // 成功时列表会自己更新；用户在提示框里取消的不算错。
  if (!error || error->type() == extensions::CrxInstallErrorType::NONE ||
      error->detail() == extensions::CrxInstallErrorDetail::USER_CANCELED ||
      error->message().empty()) {
    return;
  }
  Notice(L("extensions.error.installFailed",
           {{"name", file_name}, {"error", error->message()}}),
         /*error=*/true);
}

void ExtensionsHandler::InstallFromStore(const std::string& input) {
  const std::string id = StoreIdFrom(input);
  if (id.empty()) {
    Notice(L("extensions.notice.noStoreId"), /*error=*/true);
    return;
  }
  if (ExtensionRegistry::Get(profile_)->GetInstalledExtension(id)) {
    Notice(L("extensions.notice.alreadyInstalled"), /*error=*/false);
    return;
  }
  auto installer = base::MakeRefCounted<extensions::WebstoreInstallWithPrompt>(
      id, profile_, web_ui()->GetWebContents()->GetTopLevelNativeWindow(),
      base::BindOnce(&ExtensionsHandler::OnStoreInstalled,
                     weak_factory_.GetWeakPtr()));
  installer->BeginInstall();
}

void ExtensionsHandler::OnStoreInstalled(
    bool success,
    const std::string& error,
    extensions::webstore_install::Result result) {
  if (success || result == extensions::webstore_install::USER_CANCELLED ||
      result == extensions::webstore_install::ABORTED) {
    return;
  }
  Notice(L("extensions.notice.installFailed", {{"error", error}}),
         /*error=*/true);
}

void ExtensionsHandler::HandleCheckUpdates(const base::ListValue& args) {
  AllowJavascript();
  std::string callback_id = args[0].GetString();
  extensions::ExtensionUpdater* updater =
      extensions::ExtensionUpdater::Get(profile_);
  if (!updater || !updater->enabled()) {
    base::DictValue result;
    result.Set("updated", 0);
    result.Set("unavailable", true);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  std::map<std::string, std::string> versions;
  for (const auto& extension :
       ExtensionRegistry::Get(profile_)->GenerateInstalledExtensionsSet()) {
    versions[extension->id()] = extension->VersionString();
  }
  extensions::ExtensionUpdater::CheckParams params;
  params.fetch_priority = extensions::DownloadFetchPriority::kForeground;
  params.install_immediately = true;
  params.callback = base::BindOnce(&ExtensionsHandler::OnUpdatesChecked,
                                   weak_factory_.GetWeakPtr(),
                                   std::move(callback_id), std::move(versions));
  updater->CheckNow(std::move(params));
}

void ExtensionsHandler::OnUpdatesChecked(
    std::string callback_id,
    std::map<std::string, std::string> versions_before) {
  int updated = 0;
  for (const auto& extension :
       ExtensionRegistry::Get(profile_)->GenerateInstalledExtensionsSet()) {
    auto it = versions_before.find(extension->id());
    if (it != versions_before.end() &&
        it->second != extension->VersionString()) {
      ++updated;
    }
  }
  base::DictValue result;
  result.Set("updated", updated);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

void ExtensionsHandler::ExtensionEnableFlowFinished() {
  enable_flow_.reset();
  ScheduleChanged();
}

void ExtensionsHandler::ExtensionEnableFlowAborted(bool user_initiated) {
  enable_flow_.reset();
  ScheduleChanged();
}

}  // namespace starry
