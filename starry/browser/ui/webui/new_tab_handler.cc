// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/new_tab_handler.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "starry/browser/content_blocking/blocked_stats.h"
#include "starry/browser/content_blocking/content_blocking_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/new_tab/new_tab.h"
#include "starry/browser/new_tab/new_tab_data.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "ui/gfx/image/image.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "url/gurl.h"

namespace starry {

namespace {

using media_session::mojom::MediaSessionAction;

constexpr base::TimeDelta kChangedDelay = base::Milliseconds(150);
// 两行每行 6 格，留一格给「添加」。
constexpr size_t kMaxShortcuts = 11;
constexpr size_t kMaxClosed = 3;
constexpr int64_t kMaxImageBytes = 30 * 1024 * 1024;

// 用于拼接 background?v= 触发背景图重新加载。
int g_image_version = 0;

double TimeValue(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

bool CopyImage(const base::FilePath& from, const base::FilePath& to) {
  const std::optional<int64_t> size = base::GetFileSize(from);
  if (!size || *size <= 0 || *size > kMaxImageBytes) {
    return false;
  }
  const base::FilePath temp = to.AddExtension(FILE_PATH_LITERAL("tmp"));
  return base::CopyFile(from, temp) && base::ReplaceFile(temp, to, nullptr);
}

}  // namespace

NewTabHandler::NewTabHandler(Profile* profile)
    : profile_(profile->GetOriginalProfile()) {}

NewTabHandler::~NewTabHandler() {
  if (select_file_dialog_) {
    select_file_dialog_->ListenerDestroyed();
  }
}

// static
base::FilePath NewTabHandler::BackgroundPath(Profile* profile) {
  return profile->GetOriginalProfile()->GetPath().Append(
      FILE_PATH_LITERAL("New Tab Background"));
}

void NewTabHandler::RegisterMessages() {
  const auto add = [this](const char* name, void (NewTabHandler::*method)(
                                                const base::ListValue&)) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getNewTab", &NewTabHandler::HandleGetNewTab);
  add("newTabSearch", &NewTabHandler::HandleSearch);
  add("newTabSearchRect", &NewTabHandler::HandleSearchRect);
  add("newTabRestoreClosed", &NewTabHandler::HandleRestoreClosed);
  add("newTabShortcut", &NewTabHandler::HandleShortcut);
  add("newTabMedia", &NewTabHandler::HandleMedia);
  add("newTabSetNote", &NewTabHandler::HandleSetNote);
  add("newTabSetWidgets", &NewTabHandler::HandleSetWidgets);
  add("newTabSetShortcutRows", &NewTabHandler::HandleSetShortcutRows);
  add("newTabSetBackground", &NewTabHandler::HandleSetBackground);
  add("newTabChooseImage", &NewTabHandler::HandleChooseImage);
  add("newTabOpen", &NewTabHandler::HandleOpen);

  content::WebContents* contents = web_ui()->GetWebContents();
  new_tab::NewTabState::CreateForWebContents(contents);
  new_tab::NewTabState::FromWebContents(contents)->set_docked_listener(
      base::BindRepeating(&NewTabHandler::OnDocked,
                          weak_factory_.GetWeakPtr()));
}

void NewTabHandler::OnJavascriptAllowed() {
  const auto changed = base::BindRepeating(&NewTabHandler::ScheduleChanged,
                                           base::Unretained(this));
  prefs_.Init(profile_->GetPrefs());
  for (const char* pref :
       {prefs::kNewTabBackground, prefs::kNewTabWidgets,
        prefs::kNewTabShortcutRows, prefs::kBlockedStats}) {
    prefs_.Add(pref, changed);
  }
  shortcuts_ =
      std::make_unique<new_tab::Shortcuts>(profile_, kMaxShortcuts, changed);
  archive_observation_.Observe(tab_organizer::TabArchive::ForProfile(profile_));
  media_observation_.Observe(MediaHub::Get(profile_));
  if (sessions::TabRestoreService* service =
          TabRestoreServiceFactory::GetForProfile(profile_)) {
    restore_observation_.Observe(service);
    service->LoadTabsFromLastSession();
  }
}

void NewTabHandler::OnJavascriptDisallowed() {
  prefs_.RemoveAll();
  shortcuts_.reset();
  archive_observation_.Reset();
  media_observation_.Reset();
  restore_observation_.Reset();
  changed_timer_.Stop();
}

void NewTabHandler::OnArchiveChanged() {
  ScheduleChanged();
}

void NewTabHandler::OnMediaChanged() {
  ScheduleChanged();
}

void NewTabHandler::TabRestoreServiceChanged(
    sessions::TabRestoreService* service) {
  ScheduleChanged();
}

void NewTabHandler::TabRestoreServiceLoaded(
    sessions::TabRestoreService* service) {
  ScheduleChanged();
}

void NewTabHandler::TabRestoreServiceDestroyed(
    sessions::TabRestoreService* service) {
  restore_observation_.Reset();
}

void NewTabHandler::ScheduleChanged() {
  if (!IsJavascriptAllowed() || changed_timer_.IsRunning()) {
    return;
  }
  changed_timer_.Start(
      FROM_HERE, kChangedDelay,
      base::BindOnce(&NewTabHandler::SendChanged, base::Unretained(this)));
}

void NewTabHandler::SendChanged() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("newtab-changed", State());
  }
}

void NewTabHandler::OnDocked(bool docked) {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("newtab-docked", base::Value(docked));
  }
}

BrowserWindowInterface* NewTabHandler::Browser() {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  return tab ? tab->GetBrowserWindowInterface() : nullptr;
}

base::Value NewTabHandler::MediaState() {
  const MediaEntry* entry = MediaHub::Get(profile_)->Primary();
  if (!entry) {
    return base::Value();
  }
  if (!entry->artwork.isNull() &&
      !entry->artwork.BackedBySameObjectAs(artwork_)) {
    artwork_ = entry->artwork;
    scoped_refptr<base::RefCountedMemory> png =
        gfx::Image(artwork_).As1xPNGBytes();
    artwork_url_ =
        base::StrCat({"data:image/png;base64,",
                      base::Base64Encode(base::span<const uint8_t>(*png))});
  } else if (entry->artwork.isNull()) {
    artwork_ = gfx::ImageSkia();
    artwork_url_.clear();
  }
  return base::Value(
      base::DictValue()
          .Set("title", entry->title)
          .Set("artist", entry->artist)
          .Set("source", entry->source)
          .Set("playing", entry->playing)
          .Set("artwork", artwork_url_)
          .Set("canPrevious", entry->Can(MediaSessionAction::kPreviousTrack))
          .Set("canNext", entry->Can(MediaSessionAction::kNextTrack)));
}

base::DictValue NewTabHandler::State() {
  PrefService* prefs = profile_->GetPrefs();
  base::DictValue state;
  state.Set("background", prefs->GetString(prefs::kNewTabBackground));
  state.Set("imageVersion", g_image_version);
  base::ListValue widgets;
  for (const base::Value& id : prefs->GetList(prefs::kNewTabWidgets)) {
    if (id.is_string() && new_tab::IsWidgetId(id.GetString())) {
      widgets.Append(id.GetString());
    }
  }
  state.Set("widgets", std::move(widgets));
  state.Set("shortcutRows",
            std::clamp(prefs->GetInteger(prefs::kNewTabShortcutRows), 1, 2));
  state.Set("note", prefs->GetString(prefs::kNewTabNote));

  base::ListValue shortcuts;
  if (shortcuts_) {
    for (const new_tab::Shortcuts::Tile& tile : shortcuts_->tiles()) {
      shortcuts.Append(base::DictValue()
                           .Set("url", tile.url.spec())
                           .Set("title", tile.title));
    }
  }
  state.Set("shortcuts", std::move(shortcuts));
  state.Set("shortcutsCustomized", shortcuts_ && shortcuts_->customized());

  base::ListValue closed;
  for (const new_tab::ClosedEntry& entry :
       new_tab::RecentlyClosed(profile_, kMaxClosed)) {
    closed.Append(base::DictValue()
                      .Set("id", base::NumberToString(entry.id.id()))
                      .Set("url", entry.url.spec())
                      .Set("title", entry.title)
                      .Set("closed", TimeValue(entry.closed))
                      .Set("window", entry.window)
                      .Set("tabs", entry.tab_count));
  }
  state.Set("closed", std::move(closed));

  const new_tab::ArchiveSummary archive =
      new_tab::SummarizeArchive(profile_, 5);
  base::ListValue recent;
  for (const auto& [url, title] : archive.recent) {
    recent.Append(base::DictValue().Set("url", url.spec()).Set("title", title));
  }
  state.Set("archive", base::DictValue()
                           .Set("today", archive.today)
                           .Set("week", archive.week)
                           .Set("total", archive.total)
                           .Set("recent", std::move(recent)));

  state.Set("media", MediaState());

  const content_blocking::WeekBlocked blocked =
      content_blocking::BlockedThisWeek(profile_);
  base::ListValue days;
  for (int day : blocked.days) {
    days.Append(day);
  }
  auto* blocking = content_blocking::ContentBlockingService::Get();
  state.Set("blocked", base::DictValue()
                           .Set("total", blocked.total)
                           .Set("days", std::move(days))
                           .Set("enabled", blocking && blocking->enabled()));

  content::WebContents* contents = web_ui()->GetWebContents();
  auto* tab_state = new_tab::NewTabState::FromWebContents(contents);
  state.Set("docked", tab_state && tab_state->search_docked());
  return state;
}

void NewTabHandler::HandleGetNewTab(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(State()));
}

void NewTabHandler::HandleSearch(const base::ListValue& args) {
  content::WebContents* contents = web_ui()->GetWebContents();
  BrowserWindowInterface* browser = Browser();
  StarryBrowserView* view =
      browser ? StarryBrowserView::From(browser) : nullptr;
  if (!view) {
    return;
  }
  std::u16string text;
  if (!args.empty() && args[0].is_string()) {
    text = base::UTF8ToUTF16(args[0].GetString());
  }
  view->OpenNewTabSearch(contents, text);
}

void NewTabHandler::HandleSearchRect(const base::ListValue& args) {
  if (args.size() < 4) {
    return;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!args[i].is_double() && !args[i].is_int()) {
      return;
    }
  }
  const gfx::RectF rect(args[0].GetDouble(), args[1].GetDouble(),
                        args[2].GetDouble(), args[3].GetDouble());
  content::WebContents* contents = web_ui()->GetWebContents();
  new_tab::NewTabState::FromWebContents(contents)->set_search_rect(rect);
  if (BrowserWindowInterface* browser = Browser()) {
    if (StarryBrowserView* view = StarryBrowserView::From(browser)) {
      view->NewTabSearchMoved(contents);
    }
  }
}

void NewTabHandler::HandleRestoreClosed(const base::ListValue& args) {
  int64_t id = 0;
  if (args.empty() || !args[0].is_string() ||
      !base::StringToInt64(args[0].GetString(), &id)) {
    return;
  }
  const bool new_tab =
      args.size() > 1 && args[1].is_bool() && args[1].GetBool();
  // 换掉这个新标签页（带着原来的前进后退），⌘ 点的开新标签。
  new_tab::RestoreClosed(Browser(),
                         SessionID::FromSerializedValue(static_cast<int>(id)),
                         new_tab ? WindowOpenDisposition::NEW_BACKGROUND_TAB
                                 : WindowOpenDisposition::CURRENT_TAB);
}

void NewTabHandler::HandleShortcut(const base::ListValue& args) {
  if (!shortcuts_ || args.empty() || !args[0].is_string()) {
    return;
  }
  const std::string& action = args[0].GetString();
  if (action == "reset") {
    shortcuts_->Reset();
    return;
  }
  if (args.size() < 2 || !args[1].is_string()) {
    return;
  }
  GURL url(args[1].GetString());
  if (action == "add" && !url.is_valid()) {
    url = GURL(base::StrCat({"https://", args[1].GetString()}));
  }
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  const std::u16string title = args.size() > 2 && args[2].is_string()
                                   ? base::UTF8ToUTF16(args[2].GetString())
                                   : std::u16string();
  if (action == "add") {
    shortcuts_->Add(url, title.empty() ? base::UTF8ToUTF16(url.host()) : title);
  } else if (action == "rename") {
    shortcuts_->Rename(url, title);
  } else if (action == "remove") {
    shortcuts_->Remove(url);
  }
}

void NewTabHandler::HandleMedia(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  MediaHub* hub = MediaHub::Get(profile_);
  const MediaEntry* entry = hub->Primary();
  if (!entry) {
    return;
  }
  const base::UnguessableToken id = entry->id;
  const std::string& action = args[0].GetString();
  if (action == "toggle") {
    hub->TogglePlayPause(id);
  } else if (action == "next") {
    hub->Perform(id, MediaSessionAction::kNextTrack);
  } else if (action == "previous") {
    hub->Perform(id, MediaSessionAction::kPreviousTrack);
  } else if (action == "show") {
    hub->ShowTab(id);
  }
}

void NewTabHandler::HandleSetNote(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  // 便签不用回传：页面上就是用户打的字。
  profile_->GetPrefs()->SetString(prefs::kNewTabNote,
                                  args[0].GetString().substr(0, 4000));
}

void NewTabHandler::HandleSetWidgets(const base::ListValue& args) {
  if (args.empty() || !args[0].is_list()) {
    return;
  }
  base::ListValue widgets;
  for (const base::Value& id : args[0].GetList()) {
    if (id.is_string() && new_tab::IsWidgetId(id.GetString()) &&
        !widgets.contains(id.GetString())) {
      widgets.Append(id.GetString());
    }
  }
  profile_->GetPrefs()->SetList(prefs::kNewTabWidgets, std::move(widgets));
}

void NewTabHandler::HandleSetShortcutRows(const base::ListValue& args) {
  if (!args.empty() && args[0].is_int()) {
    profile_->GetPrefs()->SetInteger(prefs::kNewTabShortcutRows,
                                     std::clamp(args[0].GetInt(), 1, 2));
  }
}

void NewTabHandler::HandleSetBackground(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  const std::string& kind = args[0].GetString();
  PrefService* prefs = profile_->GetPrefs();
  if (kind == "theme" || kind == "sky" || kind == "solid") {
    prefs->SetString(prefs::kNewTabBackground, kind);
  } else if (kind == "image") {
    // 这次启动里选过图：换回去；没选过就请用户选一张。
    if (g_image_version > 0) {
      prefs->SetString(prefs::kNewTabBackground, kind);
    } else {
      HandleChooseImage(args);
    }
  }
}

void NewTabHandler::HandleChooseImage(const base::ListValue& args) {
  if (select_file_dialog_) {
    return;
  }
  content::WebContents* contents = web_ui()->GetWebContents();
  select_file_dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(contents));
  ui::SelectFileDialog::FileTypeInfo types;
  types.extensions = {{FILE_PATH_LITERAL("jpg"), FILE_PATH_LITERAL("jpeg"),
                       FILE_PATH_LITERAL("png"), FILE_PATH_LITERAL("webp"),
                       FILE_PATH_LITERAL("heic"), FILE_PATH_LITERAL("gif")}};
  types.include_all_files = false;
  select_file_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_OPEN_FILE, L("newTab.background.choose"),
      base::FilePath(), &types, 1, base::FilePath::StringType(),
      contents->GetTopLevelNativeWindow());
}

void NewTabHandler::FileSelected(const ui::SelectedFileInfo& file, int index) {
  select_file_dialog_ = nullptr;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&CopyImage, file.path(), BackgroundPath(profile_)),
      base::BindOnce(&NewTabHandler::OnImageCopied,
                     weak_factory_.GetWeakPtr()));
}

void NewTabHandler::FileSelectionCanceled() {
  select_file_dialog_ = nullptr;
}

void NewTabHandler::OnImageCopied(bool ok) {
  if (!ok) {
    return;
  }
  ++g_image_version;
  PrefService* prefs = profile_->GetPrefs();
  if (prefs->GetString(prefs::kNewTabBackground) == "image") {
    // 设置没变也要让所有新标签页重新读图。
    ScheduleChanged();
  }
  prefs->SetString(prefs::kNewTabBackground, "image");
}

void NewTabHandler::HandleOpen(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  BrowserWindowInterface* browser = Browser();
  if (!browser) {
    return;
  }
  const std::string& what = args[0].GetString();
  if (what == "archive") {
    OpenStarryArchive(browser);
  } else if (what == "settings") {
    OpenStarrySettings(browser, "newTab");
  } else if (what == "privacy") {
    OpenStarrySettings(browser, "privacy");
  }
}

}  // namespace starry
