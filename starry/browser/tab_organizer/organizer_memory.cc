// Copyright 2026 The Starry Browser Authors

#include "starry/browser/tab_organizer/organizer_memory.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "base/i18n/case_conversion.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/prefs/starry_prefs.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/tabs/tab_list_view.h"
#include "starry/browser/ui/tabs/tabs_view.h"
#include "url/gurl.h"

namespace starry::tab_organizer {

namespace {

constexpr char kRules[] = "rules";
constexpr char kPlaced[] = "placed";
constexpr char kRejected[] = "rejected";
constexpr char kNames[] = "names";
constexpr char kFolder[] = "folder";
constexpr char kLearned[] = "learned";
constexpr char kCount[] = "count";
constexpr char kUntil[] = "until";
// 不要的记多久；放进同一个文件夹几次学成规则；学到的规则被拒绝几次删掉。
constexpr base::TimeDelta kRejectFor = base::Days(30);
constexpr int kLearnAfter = 2;
constexpr int kForgetAfter = 2;
constexpr size_t kMaxNames = 10;
// 放过的次数、不要的最多记多少个网站（多了丢掉最早的那些）。
constexpr size_t kMaxSites = 500;

// 这个标签上点过「不提示」的文件夹（跟着标签，不存）。
class Dismissed : public content::WebContentsUserData<Dismissed> {
 public:
  ~Dismissed() override = default;

  std::set<std::string> folders;

 private:
  friend class content::WebContentsUserData<Dismissed>;
  explicit Dismissed(content::WebContents* contents)
      : content::WebContentsUserData<Dismissed>(*contents) {}
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(Dismissed);

PrefService* Prefs(Profile* profile) {
  return prefs::PrefsFor(profile);
}

const base::DictValue& Memory(Profile* profile) {
  return Prefs(profile)->GetDict(prefs::kOrganizerMemory);
}

// 文件夹还在（pin_folders 的设置里有）。
const base::DictValue* FolderData(Profile* profile, const std::string& id) {
  return id.empty() ? nullptr
                    : Prefs(profile)->GetDict(prefs::kPinFolders).FindDict(id);
}

std::string NormalizeSite(const std::string& text) {
  std::string site(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
  if (site.find("://") != std::string::npos) {
    site = GURL(site).host();
  }
  site = base::ToLowerASCII(site);
  while (!site.empty() && (site.back() == '/' || site.back() == '.')) {
    site.pop_back();
  }
  if (base::StartsWith(site, "www.")) {
    site = site.substr(4);
  }
  return site;
}

// 主机名和它上面的各级域名（不含只有一级的顶级域名），长的在前。
std::vector<std::string> Suffixes(const std::string& site) {
  std::vector<std::string> result;
  std::string rest = site;
  while (!rest.empty()) {
    result.push_back(rest);
    const size_t dot = rest.find('.');
    if (dot == std::string::npos) {
      break;
    }
    rest = rest.substr(dot + 1);
    if (rest.find('.') == std::string::npos) {
      break;
    }
  }
  return result;
}

bool RejectedNow(const base::DictValue* site_rejections,
                 const std::string& key) {
  if (!site_rejections || key.empty()) {
    return false;
  }
  const base::DictValue* entry = site_rejections->FindDict(key);
  const std::optional<double> until =
      entry ? entry->FindDouble(kUntil) : std::nullopt;
  return until &&
         base::Time::FromMillisecondsSinceUnixEpoch(*until) > base::Time::Now();
}

// 某一类（放过的、不要的）记的网站太多了：丢掉排在前面的（keep 这个网站留着）。
void Cap(base::DictValue* dict, const std::string& keep) {
  std::vector<std::string> drop;
  for (const auto [site, value] : *dict) {
    if (dict->size() - drop.size() <= kMaxSites) {
      break;
    }
    if (site != keep) {
      drop.push_back(site);
    }
  }
  for (const std::string& site : drop) {
    dict->Remove(site);
  }
}

}  // namespace

std::string SiteOf(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return std::string();
  }
  return NormalizeSite(std::string(url.host()));
}

std::string NameKey(const std::u16string& name) {
  std::u16string trimmed;
  base::TrimWhitespace(name, base::TRIM_ALL, &trimmed);
  return trimmed.empty()
             ? std::string()
             : base::StrCat(
                   {"name:", base::UTF16ToUTF8(base::i18n::ToLower(trimmed))});
}

std::vector<SiteRule> ReadRules(Profile* profile) {
  std::vector<SiteRule> rules;
  const base::DictValue* dict = Memory(profile).FindDict(kRules);
  if (!dict) {
    return rules;
  }
  for (const auto [site, value] : *dict) {
    const base::DictValue* rule = value.GetIfDict();
    const std::string* folder = rule ? rule->FindString(kFolder) : nullptr;
    if (!folder || !FolderData(profile, *folder)) {
      continue;
    }
    rules.push_back({site, *folder, rule->FindBool(kLearned).value_or(false)});
  }
  return rules;
}

std::optional<SiteRule> RuleFor(Profile* profile, const GURL& url) {
  const std::string site = SiteOf(url);
  const base::DictValue* dict = Memory(profile).FindDict(kRules);
  if (site.empty() || !dict) {
    return std::nullopt;
  }
  for (const std::string& candidate : Suffixes(site)) {
    const base::DictValue* rule = dict->FindDict(candidate);
    const std::string* folder = rule ? rule->FindString(kFolder) : nullptr;
    if (folder && FolderData(profile, *folder)) {
      return SiteRule{candidate, *folder,
                      rule->FindBool(kLearned).value_or(false)};
    }
  }
  return std::nullopt;
}

void AddRule(Profile* profile,
             const std::string& site,
             const std::string& folder,
             bool learned) {
  const std::string normalized = NormalizeSite(site);
  if (normalized.empty() || !FolderData(profile, folder)) {
    return;
  }
  ScopedDictPrefUpdate update(Prefs(profile), prefs::kOrganizerMemory);
  update->EnsureDict(kRules)->Set(
      normalized,
      base::DictValue().Set(kFolder, folder).Set(kLearned, learned));
}

void RemoveRule(Profile* profile, const std::string& site) {
  const std::string normalized = NormalizeSite(site);
  const base::DictValue* rules = Memory(profile).FindDict(kRules);
  if (!rules || !rules->FindDict(normalized)) {
    return;
  }
  ScopedDictPrefUpdate update(Prefs(profile), prefs::kOrganizerMemory);
  update->EnsureDict(kRules)->Remove(normalized);
}

void NotePlacement(Profile* profile,
                   const GURL& url,
                   const std::string& folder) {
  const std::string site = SiteOf(url);
  if (site.empty() || profile->IsOffTheRecord() ||
      !FolderData(profile, folder)) {
    return;
  }
  ScopedDictPrefUpdate update(Prefs(profile), prefs::kOrganizerMemory);
  base::DictValue* placed = update->EnsureDict(kPlaced);
  placed->EnsureDict(site);
  Cap(placed, site);
  base::DictValue* counts = placed->FindDict(site);
  const int count = counts->FindInt(folder).value_or(0) + 1;
  counts->Set(folder, count);
  if (count < kLearnAfter) {
    return;
  }
  const base::DictValue* rejected = update->FindDict(kRejected);
  if (RejectedNow(rejected ? rejected->FindDict(site) : nullptr, folder)) {
    return;
  }
  base::DictValue* rules = update->EnsureDict(kRules);
  const base::DictValue* existing = rules->FindDict(site);
  if (existing) {
    // 用户自己加的不换；学到的只在新的文件夹放得更多时换。
    const std::string* old = existing->FindString(kFolder);
    if (!existing->FindBool(kLearned).value_or(false) || !old ||
        *old == folder || count <= counts->FindInt(*old).value_or(0)) {
      return;
    }
  }
  rules->Set(site, base::DictValue().Set(kFolder, folder).Set(kLearned, true));
}

void NoteManualMove(tabs::TabInterface* tab,
                    const std::string& from,
                    const std::string& to) {
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser || browser->GetProfile()->IsOffTheRecord() || from == to) {
    return;
  }
  const GURL url = tab->GetContents()->GetVisibleURL();
  if (!to.empty()) {
    NotePlacement(browser->GetProfile(), url, to);
  }
  if (!from.empty()) {
    NoteRejection(browser->GetProfile(), url, from);
  }
}

void NoteRejection(Profile* profile,
                   const GURL& url,
                   const std::string& folder) {
  const std::string site = SiteOf(url);
  if (site.empty() || folder.empty() || profile->IsOffTheRecord()) {
    return;
  }
  ScopedDictPrefUpdate update(Prefs(profile), prefs::kOrganizerMemory);
  base::DictValue* rejected = update->EnsureDict(kRejected);
  rejected->EnsureDict(site);
  Cap(rejected, site);
  base::DictValue* entries = rejected->FindDict(site);
  // 过期的顺手清掉。
  const base::Time now = base::Time::Now();
  std::vector<std::string> expired;
  for (const auto [key, value] : *entries) {
    const base::DictValue* entry = value.GetIfDict();
    const std::optional<double> until =
        entry ? entry->FindDouble(kUntil) : std::nullopt;
    if (!until || base::Time::FromMillisecondsSinceUnixEpoch(*until) <= now) {
      expired.push_back(key);
    }
  }
  for (const std::string& key : expired) {
    entries->Remove(key);
  }
  base::DictValue* entry = entries->EnsureDict(folder);
  const int count = entry->FindInt(kCount).value_or(0) + 1;
  entry->Set(kCount, count);
  entry->Set(kUntil, (now + kRejectFor).InMillisecondsFSinceUnixEpoch());
  // 放过的次数清零：再放一次不会马上又学回来。
  if (base::DictValue* placed = update->FindDict(kPlaced)) {
    if (base::DictValue* counts = placed->FindDict(site)) {
      counts->Remove(folder);
    }
  }
  // 学到的规则被拒绝两次：删掉。
  if (base::DictValue* rules = update->FindDict(kRules)) {
    const base::DictValue* rule = rules->FindDict(site);
    const std::string* rule_folder = rule ? rule->FindString(kFolder) : nullptr;
    if (rule_folder && *rule_folder == folder &&
        rule->FindBool(kLearned).value_or(false) && count >= kForgetAfter) {
      rules->Remove(site);
    }
  }
}

bool IsRejected(Profile* profile,
                const GURL& url,
                const std::string& folder,
                const std::u16string& name) {
  const std::string site = SiteOf(url);
  const base::DictValue* rejected = Memory(profile).FindDict(kRejected);
  if (site.empty() || !rejected) {
    return false;
  }
  const base::DictValue* entries = rejected->FindDict(site);
  return RejectedNow(entries, folder) || RejectedNow(entries, NameKey(name));
}

std::vector<std::string> RejectedFor(Profile* profile, const GURL& url) {
  std::vector<std::string> keys;
  const std::string site = SiteOf(url);
  const base::DictValue* rejected = Memory(profile).FindDict(kRejected);
  const base::DictValue* entries =
      site.empty() || !rejected ? nullptr : rejected->FindDict(site);
  if (!entries) {
    return keys;
  }
  for (const auto [key, value] : *entries) {
    if (RejectedNow(entries, key)) {
      keys.push_back(key);
    }
  }
  return keys;
}

void NoteName(Profile* profile, const std::u16string& name) {
  std::u16string trimmed;
  base::TrimWhitespace(name, base::TRIM_ALL, &trimmed);
  if (trimmed.empty() || profile->IsOffTheRecord()) {
    return;
  }
  const std::string text = base::UTF16ToUTF8(trimmed);
  ScopedDictPrefUpdate update(Prefs(profile), prefs::kOrganizerMemory);
  base::ListValue* names = update->EnsureList(kNames);
  base::ListValue updated;
  updated.Append(text);
  for (const base::Value& value : *names) {
    if (value.is_string() && value.GetString() != text &&
        updated.size() < kMaxNames) {
      updated.Append(value.GetString());
    }
  }
  *names = std::move(updated);
}

std::vector<std::u16string> PreferredNames(Profile* profile) {
  std::vector<std::u16string> names;
  if (const base::ListValue* list = Memory(profile).FindList(kNames)) {
    for (const base::Value& value : *list) {
      if (value.is_string()) {
        names.push_back(base::UTF8ToUTF16(value.GetString()));
      }
    }
  }
  return names;
}

void Forget(Profile* profile) {
  ScopedDictPrefUpdate update(Prefs(profile), prefs::kOrganizerMemory);
  update->Remove(kPlaced);
  update->Remove(kRejected);
  update->Remove(kNames);
  if (base::DictValue* rules = update->FindDict(kRules)) {
    std::vector<std::string> learned;
    for (const auto [site, value] : *rules) {
      const base::DictValue* rule = value.GetIfDict();
      if (!rule || rule->FindBool(kLearned).value_or(false)) {
        learned.push_back(site);
      }
    }
    for (const std::string& site : learned) {
      rules->Remove(site);
    }
  }
}

std::optional<Suggestion> SuggestFolder(tabs::TabInterface* tab) {
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser || tab->IsPinned() ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    return std::nullopt;
  }
  Profile* profile = browser->GetProfile();
  if (profile->IsOffTheRecord() ||
      !Prefs(profile)->GetBoolean(prefs::kOrganizeSuggest)) {
    return std::nullopt;
  }
  content::WebContents* contents = tab->GetContents();
  const GURL& url = contents->GetVisibleURL();
  const std::optional<SiteRule> rule = RuleFor(profile, url);
  if (!rule) {
    return std::nullopt;
  }
  const base::DictValue* folder = FolderData(profile, rule->folder);
  const std::string* name = folder ? folder->FindString("name") : nullptr;
  if (!name) {
    return std::nullopt;
  }
  Suggestion suggestion;
  suggestion.folder = rule->folder;
  suggestion.name = base::UTF8ToUTF16(*name);
  if (const std::string* color = folder->FindString("color")) {
    suggestion.color = *color;
  }
  // 不要过一次的学到的规则照常提示（这个标签上点过「不提示」的不提示）；被拒绝两次的
  // 规则已经删了。
  if (const Dismissed* dismissed = Dismissed::FromWebContents(contents);
      dismissed && dismissed->folders.contains(rule->folder)) {
    return std::nullopt;
  }
  return suggestion;
}

bool AcceptSuggestion(tabs::TabInterface* tab) {
  const std::optional<Suggestion> suggestion = SuggestFolder(tab);
  if (!suggestion) {
    return false;
  }
  BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
  const GURL url = tab->GetContents()->GetVisibleURL();
  TabsView* view = pin_folders::VisibleTabs(browser);
  if (view) {
    view->BeginBatch();
  }
  pin_folders::Place(browser, pin_folders::Child{tab->GetHandle(), {}},
                     suggestion->folder);
  if (view) {
    view->EndBatch(TabListView::Change::kFold);
  }
  NotePlacement(browser->GetProfile(), url, suggestion->folder);
  return tab->IsPinned();
}

void DismissSuggestion(tabs::TabInterface* tab) {
  const std::optional<Suggestion> suggestion = SuggestFolder(tab);
  if (!suggestion) {
    return;
  }
  content::WebContents* contents = tab->GetContents();
  Dismissed::CreateForWebContents(contents);
  Dismissed::FromWebContents(contents)->folders.insert(suggestion->folder);
  // 记一次不要（设置变了，标签列表跟着刷新，这一行的提示没了）。
  NoteRejection(tab->GetBrowserWindowInterface()->GetProfile(),
                contents->GetVisibleURL(), suggestion->folder);
}

std::string DescribeMemoryForTesting(Profile* profile) {
  std::map<std::string, std::string> names;
  for (const auto [id, value] : Prefs(profile)->GetDict(prefs::kPinFolders)) {
    if (const base::DictValue* folder = value.GetIfDict()) {
      if (const std::string* name = folder->FindString("name")) {
        names[id] = *name;
      }
    }
  }
  const auto folder_name = [&](const std::string& key) {
    auto found = names.find(key);
    return found != names.end() ? base::StrCat({"[", found->second, "]"}) : key;
  };
  std::string text = "memory\n";
  for (const SiteRule& rule : ReadRules(profile)) {
    base::StrAppend(&text,
                    {"  rule ", rule.site, " -> ", folder_name(rule.folder),
                     rule.learned ? " learned" : " manual", "\n"});
  }
  const base::DictValue& memory = Memory(profile);
  if (const base::DictValue* placed = memory.FindDict(kPlaced)) {
    for (const auto [site, value] : *placed) {
      if (const base::DictValue* counts = value.GetIfDict()) {
        for (const auto [folder, count] : *counts) {
          base::StrAppend(
              &text,
              {"  placed ", site, " -> ", folder_name(folder), " x",
               base::NumberToString(count.GetIfInt().value_or(0)), "\n"});
        }
      }
    }
  }
  const base::Time now = base::Time::Now();
  if (const base::DictValue* rejected = memory.FindDict(kRejected)) {
    for (const auto [site, value] : *rejected) {
      const base::DictValue* entries = value.GetIfDict();
      if (!entries) {
        continue;
      }
      for (const auto [key, entry] : *entries) {
        const base::DictValue* data = entry.GetIfDict();
        const std::optional<double> until =
            data ? data->FindDouble(kUntil) : std::nullopt;
        if (!until) {
          continue;
        }
        const base::TimeDelta left =
            base::Time::FromMillisecondsSinceUnixEpoch(*until) - now;
        base::StrAppend(
            &text, {"  rejected ", site, " -> ", folder_name(key), " x",
                    base::NumberToString(data->FindInt(kCount).value_or(0)),
                    " days_left=", base::NumberToString(left.InDays()), "\n"});
      }
    }
  }
  for (const std::u16string& name : PreferredNames(profile)) {
    base::StrAppend(&text, {"  name \"", base::UTF16ToUTF8(name), "\"\n"});
  }
  return text;
}

}  // namespace starry::tab_organizer
