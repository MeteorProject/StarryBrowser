// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/import_handler.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "components/user_data_importer/common/importer_data_types.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/importer/chromium_browsers.h"
#include "starry/browser/l10n/l10n.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "url/gurl.h"

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/devtools.h"
#endif

namespace starry {

namespace {

// 页面上几样的名字和对应的位，也是列出来的顺序。
struct ItemName {
  user_data_importer::ImportItem item;
  const char* name;
};

constexpr ItemName kItemNames[] = {
    {user_data_importer::FAVORITES, "bookmarks"},
    {user_data_importer::HISTORY, "history"},
    {user_data_importer::PASSWORDS, "passwords"},
    {user_data_importer::SEARCH_ENGINES, "searchEngines"},
    {user_data_importer::AUTOFILL_FORM_DATA, "autofill"},
};

base::DictValue ItemsDict(uint16_t items) {
  base::DictValue dict;
  for (const ItemName& entry : kItemNames) {
    dict.Set(entry.name, (items & entry.item) != 0);
  }
  return dict;
}

std::string ItemKey(user_data_importer::ImportItem item) {
  for (const ItemName& entry : kItemNames) {
    if (entry.item == item) {
      return entry.name;
    }
  }
  return std::string();
}

std::string StatusName(ImportService::Status status) {
  switch (status) {
    case ImportService::Status::kRunning:
      return "running";
    case ImportService::Status::kDone:
      return "done";
    case ImportService::Status::kFailed:
      return "failed";
  }
  return "failed";
}

base::DictValue JobDict(const ImportService::Job& job) {
  base::DictValue dict;
  dict.Set("sourceId", job.source_id);
  dict.Set("name", job.source_name);
  dict.Set("status", StatusName(job.status));
  dict.Set("current", ItemKey(job.current));
  dict.Set("items", ItemsDict(job.items));
  base::DictValue counts;
  counts.Set("bookmarks", static_cast<int>(job.bookmarks));
  counts.Set("history", static_cast<int>(job.history));
  counts.Set("passwords", static_cast<int>(job.passwords));
  counts.Set("searchEngines", static_cast<int>(job.search_engines));
  counts.Set("autofill", static_cast<int>(job.autofill));
  dict.Set("counts", std::move(counts));
  dict.Set("undecryptablePasswords",
           static_cast<int>(job.undecryptable_passwords));
  dict.Set("passwordKeyUnavailable", job.password_key_unavailable);
  dict.Set("failed", ItemsDict(job.failed_items));
  dict.Set("error", job.error);
  return dict;
}

}  // namespace

ImportHandler::ImportHandler(Profile* profile)
    : profile_(profile->GetOriginalProfile()) {}

ImportHandler::~ImportHandler() {
  if (select_file_dialog_) {
    select_file_dialog_->ListenerDestroyed();
  }
}

void ImportHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getImportState", &ImportHandler::HandleGetImportState);
  add("detectImportSources", &ImportHandler::HandleDetectImportSources);
  add("startImport", &ImportHandler::HandleStartImport);
  add("importBookmarksFile", &ImportHandler::HandleImportBookmarksFile);
  add("clearImport", &ImportHandler::HandleClearImport);
  add("openFullDiskAccess", &ImportHandler::HandleOpenFullDiskAccess);
}

void ImportHandler::OnJavascriptAllowed() {
  observation_.Observe(ImportService::Get(profile_));
}

void ImportHandler::OnJavascriptDisallowed() {
  observation_.Reset();
}

void ImportHandler::OnImportChanged() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("import-changed", base::Value(State()));
  }
}

base::DictValue ImportHandler::State() {
  ImportService* service = ImportService::Get(profile_);
  base::DictValue state;
  state.Set("detecting", service->detecting() || !service->detected());
  base::ListValue sources;
  for (const ImportSource& source : service->sources()) {
    base::DictValue entry;
    entry.Set("id", source.id);
    entry.Set("browser", source.browser);
    entry.Set("profile", source.profile);
    entry.Set("safari", source.id == "safari");
    entry.Set("items", ItemsDict(source.items));
#if BUILDFLAG(IS_MAC)
    // 读密码时系统问的是这个钥匙串项目。
    if (source.chromium && (source.items & user_data_importer::PASSWORDS)) {
      entry.Set("keychainItem", source.chromium->keychain_service);
    }
#endif
    sources.Append(std::move(entry));
  }
  state.Set("sources", std::move(sources));
  state.Set("safariNeedsAccess", service->safari_needs_access());
  if (service->job()) {
    state.Set("job", JobDict(*service->job()));
  }
  return state;
}

void ImportHandler::HandleGetImportState(const base::ListValue& args) {
  AllowJavascript();
  // 每次打开这一页都重新找一遍（可能刚装了浏览器、刚给了权限），找完之前先显示上次找到的。
  ImportService* service = ImportService::Get(profile_);
  service->DetectSources();
  ResolveJavascriptCallback(args[0], base::Value(State()));
}

void ImportHandler::HandleDetectImportSources(const base::ListValue& args) {
  AllowJavascript();
  ImportService::Get(profile_)->DetectSources();
}

void ImportHandler::HandleStartImport(const base::ListValue& args) {
  if (args.size() != 2 || !args[0].is_string() || !args[1].is_dict()) {
    return;
  }
  AllowJavascript();
  uint16_t items = user_data_importer::NONE;
  for (const ItemName& entry : kItemNames) {
    if (args[1].GetDict().FindBool(entry.name).value_or(false)) {
      items |= entry.item;
    }
  }
  ImportService::Get(profile_)->Start(args[0].GetString(), items);
}

void ImportHandler::HandleImportBookmarksFile(const base::ListValue& args) {
  AllowJavascript();
#if defined(STARRY_DEVTOOLS)
  if (!args.empty() && args[0].is_string() &&
      devtools::AllowDirectBookmarksFileImport()) {
    ImportService::Get(profile_)->StartBookmarksFile(
        base::FilePath::FromUTF8Unsafe(args[0].GetString()));
    return;
  }
#endif
  if (select_file_dialog_ || ImportService::Get(profile_)->running()) {
    return;
  }
  content::WebContents* contents = web_ui()->GetWebContents();
  select_file_dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(contents));
  ui::SelectFileDialog::FileTypeInfo types;
  types.extensions = {{FILE_PATH_LITERAL("html"), FILE_PATH_LITERAL("htm")}};
  types.include_all_files = true;
  select_file_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_OPEN_FILE,
      L("settings.import.bookmarksFile.choose"), base::FilePath(), &types, 1,
      FILE_PATH_LITERAL("html"), contents->GetTopLevelNativeWindow());
}

void ImportHandler::FileSelected(const ui::SelectedFileInfo& file, int index) {
  select_file_dialog_ = nullptr;
  ImportService::Get(profile_)->StartBookmarksFile(file.path());
}

void ImportHandler::FileSelectionCanceled() {
  select_file_dialog_ = nullptr;
}

void ImportHandler::HandleClearImport(const base::ListValue& args) {
  ImportService::Get(profile_)->ClearJob();
}

void ImportHandler::HandleOpenFullDiskAccess(const base::ListValue& args) {
#if BUILDFLAG(IS_MAC)
  platform_util::OpenExternal(
      GURL("x-apple.systempreferences:com.apple.preference.security?Privacy_"
           "AllFiles"));
#endif
}

}  // namespace starry
