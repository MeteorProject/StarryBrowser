// Copyright 2026 The Starry Browser Authors

#include "starry/browser/l10n/interface_language.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "starry/browser/starry_hooks.h"

#if BUILDFLAG(IS_MAC)
#include <CoreFoundation/CoreFoundation.h>

#include <array>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/strings/sys_string_conversions.h"
#elif BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/strings/utf_string_conversions.h"
#endif

namespace starry {

namespace l10n {

namespace {

#if BUILDFLAG(IS_MAC)

// 某个域里的 AppleLanguages（不往全局域找）。
std::vector<std::string> AppleLanguagesIn(CFStringRef application) {
  base::apple::ScopedCFTypeRef<CFPropertyListRef> value(CFPreferencesCopyValue(
      CFSTR("AppleLanguages"), application, kCFPreferencesCurrentUser,
      kCFPreferencesAnyHost));
  std::vector<std::string> languages;
  CFArrayRef array = base::apple::CFCast<CFArrayRef>(value.get());
  if (!array) {
    return languages;
  }
  for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
    if (CFStringRef item = base::apple::CFCast<CFStringRef>(
            CFArrayGetValueAtIndex(array, i))) {
      languages.push_back(base::SysCFStringRefToUTF8(item));
    }
  }
  return languages;
}

#elif !BUILDFLAG(IS_WIN)

// Linux：启动时 WillInitResourceBundle 可能改了 LANGUAGE，系统原来的值存在这里。
std::optional<std::string>& OriginalLanguageVariable() {
  static base::NoDestructor<std::optional<std::string>> value;
  return *value;
}

#endif

// 系统的首选语言，按顺序。
std::vector<std::string> SystemLanguages() {
#if BUILDFLAG(IS_MAC)
  return AppleLanguagesIn(kCFPreferencesAnyApplication);
#elif BUILDFLAG(IS_WIN)
  ULONG count = 0;
  ULONG size = 0;
  if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr,
                                     &size) ||
      size == 0) {
    return {};
  }
  std::wstring buffer(size, L'\0');
  if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(),
                                     &size)) {
    return {};
  }
  // 以 \0 分隔、两个 \0 结尾的列表。
  std::vector<std::string> languages;
  size_t start = 0;
  while (start < buffer.size() && buffer[start] != L'\0') {
    size_t end = buffer.find(L'\0', start);
    if (end == std::wstring::npos) {
      end = buffer.size();
    }
    languages.push_back(base::WideToUTF8(buffer.substr(start, end - start)));
    start = end + 1;
  }
  return languages;
#else
  std::vector<std::string> languages;
  for (const char* name : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
    std::string value;
    if (std::string_view(name) == "LANGUAGE" && OriginalLanguageVariable()) {
      value = *OriginalLanguageVariable();
    } else if (const char* env = std::getenv(name)) {
      value = env;
    }
    // LANG=zh_CN.UTF-8、LANGUAGE=ja:en 这些写法。
    for (const std::string& item : base::SplitString(
             value, ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      languages.push_back(item.substr(0, item.find_first_of(".@")));
    }
  }
  return languages;
#endif
}

}  // namespace

std::optional<Language> ChosenLanguage() {
#if BUILDFLAG(IS_MAC)
  const std::vector<std::string> languages =
      AppleLanguagesIn(kCFPreferencesCurrentApplication);
  if (languages.empty()) {
    return std::nullopt;
  }
  return LanguageMatching(languages.front());
#else
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    return std::nullopt;
  }
  const std::string& locale =
      local_state->GetString(language::prefs::kApplicationLocale);
  if (locale.empty()) {
    return std::nullopt;
  }
  return LanguageMatching(locale);
#endif
}

void SetChosenLanguage(std::optional<Language> language) {
#if BUILDFLAG(IS_MAC)
  if (language) {
    base::apple::ScopedCFTypeRef<CFStringRef> code =
        base::SysUTF8ToCFStringRef(LanguageCode(*language));
    std::array<const void*, 1> values = {code.get()};
    base::apple::ScopedCFTypeRef<CFArrayRef> array(CFArrayCreate(
        nullptr, values.data(), values.size(), &kCFTypeArrayCallBacks));
    CFPreferencesSetValue(CFSTR("AppleLanguages"), array.get(),
                          kCFPreferencesCurrentApplication,
                          kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
  } else {
    CFPreferencesSetValue(CFSTR("AppleLanguages"), nullptr,
                          kCFPreferencesCurrentApplication,
                          kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
  }
  CFPreferencesSynchronize(kCFPreferencesCurrentApplication,
                           kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
#else
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    return;
  }
  if (language) {
    local_state->SetString(language::prefs::kApplicationLocale,
                           std::string(ChromeLocale(*language)));
  } else {
    local_state->ClearPref(language::prefs::kApplicationLocale);
  }
#endif
}

Language SystemLanguage() {
  const std::vector<std::string> languages = SystemLanguages();
  if (languages.empty()) {
    return Language::kEnglish;
  }
  return LanguageMatching(languages.front()).value_or(Language::kEnglish);
}

}  // namespace l10n

// 上游的钩子（starry_hooks.h）。
void WillInitResourceBundle(const std::string& preferred_locale) {
#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN)
  // Chrome 在 Linux 上不看 intl.app_locale，只看 LANGUAGE、LC_*、LANG：选了界面语言时
  // 把它放进 LANGUAGE。子进程继承这个变量，GTK 的对话框也跟着变。
  if (preferred_locale.empty()) {
    return;
  }
  const char* original = std::getenv("LANGUAGE");
  l10n::OriginalLanguageVariable() = original ? original : "";
  setenv("LANGUAGE", preferred_locale.c_str(), /*overwrite=*/1);
#endif
}

}  // namespace starry
