// Copyright 2026 The Starry Browser Authors

#include "starry/installer/win/strings.h"

#include <windows.h>

#include <array>
#include <string_view>

#include "starry/installer/win/installer_strings.h"

namespace starry::installer {

namespace {

// Same order as LANGUAGES in generate_strings.py.
constexpr std::array<const wchar_t*, 5> kLanguages = {L"zh-Hans", L"zh-Hant",
                                                      L"en", L"ja", L"ko"};
constexpr std::array<const wchar_t*, 5> kLocales = {
    L"zh-CN", L"zh-TW", L"en-US", L"ja-JP", L"ko-KR"};
constexpr size_t kEnglish = 2;

size_t g_language = kEnglish;

size_t LanguageFromLangId(LANGID id) {
  switch (PRIMARYLANGID(id)) {
    case LANG_CHINESE:
      switch (SUBLANGID(id)) {
        case SUBLANG_CHINESE_TRADITIONAL:
        case SUBLANG_CHINESE_HONGKONG:
        case SUBLANG_CHINESE_MACAU:
          return 1;
        default:
          return 0;
      }
    case LANG_JAPANESE:
      return 3;
    case LANG_KOREAN:
      return 4;
    default:
      return kEnglish;
  }
}

}  // namespace

void InitLanguage(const std::wstring& override_tag) {
  if (!override_tag.empty()) {
    for (size_t i = 0; i < kLanguages.size(); ++i) {
      if (CompareStringOrdinal(kLanguages[i], -1, override_tag.c_str(), -1,
                               TRUE) == CSTR_EQUAL) {
        g_language = i;
        return;
      }
    }
  }
  g_language = LanguageFromLangId(GetUserDefaultUILanguage());
}

const wchar_t* LocaleName() {
  return kLocales[g_language];
}

std::wstring L(const char* key) {
  const std::string_view wanted(key);
  for (const InstallerString& entry : kInstallerStrings) {
    if (entry.key == wanted) {
      const std::wstring_view text(entry.text[g_language]);
      return std::wstring(text.empty() ? std::wstring_view(entry.text[kEnglish])
                                       : text);
    }
  }
  return std::wstring(wanted.begin(), wanted.end());
}

std::wstring Fill(std::wstring text,
                  const char* name,
                  const std::wstring& value) {
  const std::string_view name_view(name);
  std::wstring placeholder = L"{";
  placeholder.append(name_view.begin(), name_view.end());
  placeholder += L"}";
  const size_t at = text.find(placeholder);
  if (at != std::wstring::npos) {
    text.replace(at, placeholder.size(), value);
  }
  return text;
}

}  // namespace starry::installer
