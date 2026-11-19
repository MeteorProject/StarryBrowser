// Copyright 2026 The Starry Browser Authors

#include "starry/browser/importer/chromium_browsers.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/user_data_importer/common/importer_data_types.h"

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/devtools.h"
#endif

#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#include "base/path_service.h"
#elif !BUILDFLAG(IS_MAC)
#include "base/environment.h"
#include "base/nix/xdg_util.h"
#endif

namespace starry {

namespace {

// Local State 一般几十 KB，多个用户资料、装了很多扩展时几 MB。
constexpr size_t kMaxLocalStateSize = 32 * 1024 * 1024;

// clang-format off
constexpr ChromiumBrowser kBrowsers[] = {
    // id, 名字,
    // macOS, Windows, Windows 上在 Roaming, Linux,
    // 钥匙串的项目和账户, 密码能不能导入
    {"chrome", "Google Chrome",
     "Google/Chrome", "Google/Chrome/User Data", false, "google-chrome",
     "Chrome Safe Storage", "Chrome", true},
    {"chrome-beta", "Google Chrome Beta",
     "Google/Chrome Beta", "Google/Chrome Beta/User Data", false,
     "google-chrome-beta",
     "Chrome Safe Storage", "Chrome", true},
    {"chrome-dev", "Google Chrome Dev",
     "Google/Chrome Dev", "Google/Chrome Dev/User Data", false,
     "google-chrome-unstable",
     "Chrome Safe Storage", "Chrome", true},
    {"chrome-canary", "Google Chrome Canary",
     "Google/Chrome Canary", "Google/Chrome SxS/User Data", false, "",
     "Chrome Safe Storage", "Chrome", true},
    {"edge", "Microsoft Edge",
     "Microsoft Edge", "Microsoft/Edge/User Data", false, "microsoft-edge",
     "Microsoft Edge Safe Storage", "Microsoft Edge", true},
    {"edge-beta", "Microsoft Edge Beta",
     "Microsoft Edge Beta", "Microsoft/Edge Beta/User Data", false,
     "microsoft-edge-beta",
     "Microsoft Edge Safe Storage", "Microsoft Edge", true},
    {"edge-dev", "Microsoft Edge Dev",
     "Microsoft Edge Dev", "Microsoft/Edge Dev/User Data", false,
     "microsoft-edge-dev",
     "Microsoft Edge Safe Storage", "Microsoft Edge", true},
    {"edge-canary", "Microsoft Edge Canary",
     "Microsoft Edge Canary", "Microsoft/Edge SxS/User Data", false, "",
     "Microsoft Edge Safe Storage", "Microsoft Edge", true},
    {"arc", "Arc",
     "Arc/User Data", "", false, "",
     "Arc Safe Storage", "Arc", true},
    {"brave", "Brave",
     "BraveSoftware/Brave-Browser", "BraveSoftware/Brave-Browser/User Data",
     false, "BraveSoftware/Brave-Browser",
     "Brave Safe Storage", "Brave", true},
    {"brave-beta", "Brave Beta",
     "BraveSoftware/Brave-Browser-Beta",
     "BraveSoftware/Brave-Browser-Beta/User Data", false,
     "BraveSoftware/Brave-Browser-Beta",
     "Brave Safe Storage", "Brave", true},
    {"brave-nightly", "Brave Nightly",
     "BraveSoftware/Brave-Browser-Nightly",
     "BraveSoftware/Brave-Browser-Nightly/User Data", false,
     "BraveSoftware/Brave-Browser-Nightly",
     "Brave Safe Storage", "Brave", true},
    {"vivaldi", "Vivaldi",
     "Vivaldi", "Vivaldi/User Data", false, "vivaldi",
     "Vivaldi Safe Storage", "Vivaldi", true},
    // Opera 的数据直接放在数据目录里，没有 Default。
    {"opera", "Opera",
     "com.operasoftware.Opera", "Opera Software/Opera Stable", true, "opera",
     "Opera Safe Storage", "Opera", true},
    {"opera-gx", "Opera GX",
     "com.operasoftware.OperaGX", "Opera Software/Opera GX Stable", true, "",
     "Opera GX Safe Storage", "Opera GX", true},
    {"yandex", "Yandex Browser",
     "Yandex/YandexBrowser", "Yandex/YandexBrowser/User Data", false,
     "yandex-browser",
     "Yandex Safe Storage", "Yandex", false},
    {"whale", "NAVER Whale",
     "Naver/Whale", "Naver/Naver Whale/User Data", false, "naver-whale",
     "Whale Safe Storage", "Whale", true},
    {"chromium", "Chromium",
     "Chromium", "Chromium/User Data", false, "chromium",
     "Chromium Safe Storage", "Chromium", true},
};
// clang-format on

base::FilePath AppendRelative(base::FilePath root, std::string_view relative) {
  for (std::string_view part : base::SplitStringPiece(
           relative, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    root = root.AppendASCII(part);
  }
  return root;
}

// 这个浏览器在这个平台上的数据目录；这个平台上没有它时是 nullopt。
std::optional<base::FilePath> UserDataDir(
    const ChromiumBrowser& browser,
    const std::optional<base::FilePath>& home) {
#if BUILDFLAG(IS_MAC)
  const std::string_view relative = browser.mac;
  const base::FilePath root =
      (home ? *home : base::GetHomeDir())
          .Append(FILE_PATH_LITERAL("Library"))
          .Append(FILE_PATH_LITERAL("Application Support"));
#elif BUILDFLAG(IS_WIN)
  const std::string_view relative = browser.windows;
  base::FilePath root;
  if (home) {
    root = home->Append(FILE_PATH_LITERAL("AppData"))
               .Append(browser.windows_roaming ? FILE_PATH_LITERAL("Roaming")
                                               : FILE_PATH_LITERAL("Local"));
  } else if (!base::PathService::Get(browser.windows_roaming
                                         ? base::DIR_ROAMING_APP_DATA
                                         : base::DIR_LOCAL_APP_DATA,
                                     &root)) {
    return std::nullopt;
  }
#else
  const std::string_view relative = browser.linux;
  base::FilePath root;
  if (home) {
    root = home->Append(FILE_PATH_LITERAL(".config"));
  } else {
    std::unique_ptr<base::Environment> env = base::Environment::Create();
    root = base::nix::GetXDGDirectory(
        env.get(), base::nix::kXdgConfigHomeEnvVar, base::nix::kDotConfigDir);
  }
#endif
  if (relative.empty() || root.empty()) {
    return std::nullopt;
  }
  return AppendRelative(root, relative);
}

uint16_t ItemsIn(const base::FilePath& dir, const ChromiumBrowser& browser) {
  const auto exists = [&dir](const char* name) {
    return base::PathExists(dir.AppendASCII(name));
  };
  uint16_t items = user_data_importer::NONE;
  if (exists(kBookmarksFile) || exists(kAccountBookmarksFile)) {
    items |= user_data_importer::FAVORITES;
  }
  if (exists(kHistoryFile)) {
    items |= user_data_importer::HISTORY;
  }
  if (browser.passwords &&
      (exists(kLoginDataFile) || exists(kAccountLoginDataFile))) {
    items |= user_data_importer::PASSWORDS;
  }
  if (exists(kWebDataFile)) {
    items |= user_data_importer::SEARCH_ENGINES |
             user_data_importer::AUTOFILL_FORM_DATA;
  }
  return items;
}

// 最后一次用它的时间：Preferences 每次开着浏览器都会写，History 每次浏览都会写。
base::Time LastUsed(const base::FilePath& dir) {
  base::Time last_used;
  for (const char* name : {"Preferences", kHistoryFile}) {
    base::File::Info info;
    if (base::GetFileInfo(dir.AppendASCII(name), &info)) {
      last_used = std::max(last_used, info.last_modified);
    }
  }
  return last_used;
}

struct ProfileEntry {
  base::FilePath dir;
  std::u16string name;
};

// Local State 的 profile.info_cache（目录名 → {name, …}），按 profile.profiles_order
// 排（浏览器的用户资料菜单里的顺序），不在里面的按目录名排在后面。
std::vector<ProfileEntry> LocalStateProfiles(
    const base::FilePath& user_data_dir) {
  std::string text;
  if (!base::ReadFileToStringWithMaxSize(
          user_data_dir.AppendASCII(kLocalStateFile), &text,
          kMaxLocalStateSize)) {
    return {};
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(text, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const base::DictValue* cache =
      root ? root->FindDictByDottedPath("profile.info_cache") : nullptr;
  if (!cache) {
    return {};
  }
  std::vector<std::string> order;
  if (const base::ListValue* listed =
          root->FindListByDottedPath("profile.profiles_order")) {
    for (const base::Value& dir : *listed) {
      if (dir.is_string() && cache->FindDict(dir.GetString()) &&
          !std::ranges::contains(order, dir.GetString())) {
        order.push_back(dir.GetString());
      }
    }
  }
  for (const auto [dir, info] : *cache) {
    if (info.is_dict() && !std::ranges::contains(order, dir)) {
      order.push_back(dir);
    }
  }
  std::vector<ProfileEntry> profiles;
  for (const std::string& dir : order) {
    // 只认数据目录下面的一层（目录名来自文件，不让它指到别处）。
    const base::FilePath name = base::FilePath::FromUTF8Unsafe(dir);
    if (name.empty() || name.BaseName() != name ||
        name.value() == base::FilePath::kCurrentDirectory ||
        name.value() == base::FilePath::kParentDirectory) {
      continue;
    }
    const std::string* title = cache->FindDict(dir)->FindString("name");
    profiles.push_back(
        {user_data_dir.Append(name),
         base::UTF8ToUTF16(title && !title->empty() ? *title : dir)});
  }
  return profiles;
}

ImportSource MakeSource(const ChromiumBrowser& browser,
                        const base::FilePath& user_data_dir,
                        const base::FilePath& profile_dir,
                        std::u16string profile,
                        uint16_t items) {
  ImportSource source;
  source.id = base::StrCat({"chromium:", browser.id, "/",
                            profile_dir == user_data_dir
                                ? ""
                                : profile_dir.BaseName().AsUTF8Unsafe()});
  source.kind = ImportSource::Kind::kChromium;
  source.browser = base::UTF8ToUTF16(browser.name);
  source.profile = std::move(profile);
  source.items = items;
  source.last_used = LastUsed(profile_dir);
  source.chromium = &browser;
  source.user_data_dir = user_data_dir;
  source.profile_dir = profile_dir;
  return source;
}

}  // namespace

base::span<const ChromiumBrowser> ChromiumBrowsers() {
  return kBrowsers;
}

std::optional<base::FilePath> ImportHomeForTesting() {
#if defined(STARRY_DEVTOOLS)
  return devtools::ImportHomeOverride();
#else
  return std::nullopt;
#endif
}

std::vector<ImportSource> DetectChromiumSources() {
  const std::optional<base::FilePath> home = ImportHomeForTesting();
  std::vector<ImportSource> sources;
  for (const ChromiumBrowser& browser : kBrowsers) {
    const std::optional<base::FilePath> user_data_dir =
        UserDataDir(browser, home);
    if (!user_data_dir || !base::DirectoryExists(*user_data_dir)) {
      continue;
    }
    std::vector<ImportSource> found;
    for (ProfileEntry& entry : LocalStateProfiles(*user_data_dir)) {
      if (const uint16_t items = ItemsIn(entry.dir, browser)) {
        found.push_back(MakeSource(browser, *user_data_dir, entry.dir,
                                   std::move(entry.name), items));
      }
    }
    if (found.empty()) {
      // 没有用户资料表（或者表里的目录都是空的）：Default，或者数据直接放在数据目录
      // 里（Opera）。
      for (const base::FilePath& dir :
           {user_data_dir->Append(FILE_PATH_LITERAL("Default")),
            *user_data_dir}) {
        if (const uint16_t items = ItemsIn(dir, browser)) {
          found.push_back(MakeSource(browser, *user_data_dir, dir, u"", items));
          break;
        }
      }
    }
    // 只有一个用户资料时不显示它的名字（多半是「用户 1」「Default」）。
    if (found.size() == 1) {
      found.front().profile.clear();
    }
    for (ImportSource& source : found) {
      sources.push_back(std::move(source));
    }
  }
  return sources;
}

}  // namespace starry
