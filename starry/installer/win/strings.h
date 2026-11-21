// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_INSTALLER_WIN_STRINGS_H_
#define STARRY_INSTALLER_WIN_STRINGS_H_

#include <string>

namespace starry::installer {

void InitLanguage(const std::wstring& override_tag);

// BCP 47 locale for DirectWrite font fallback, e.g. L"zh-CN".
const wchar_t* LocaleName();

// Named L() so scripts/l10n.py tracks key usage.
std::wstring L(const char* key);

std::wstring Fill(std::wstring text,
                  const char* name,
                  const std::wstring& value);

}  // namespace starry::installer

#endif  // STARRY_INSTALLER_WIN_STRINGS_H_
