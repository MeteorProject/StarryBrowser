// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_L10N_INTERFACE_LANGUAGE_H_
#define STARRY_BROWSER_L10N_INTERFACE_LANGUAGE_H_

#include <optional>
#include <string>
#include <vector>

#include "starry/browser/l10n/l10n.h"

// 设置里的「界面语言」。界面语言就是 Chrome 的界面语言（Chrome 自己的文字和 Starry 的
// 一致），所以设置写在 Chrome 读它的地方，重新启动后生效：
// - macOS：这个 App 自己的 AppleLanguages（同「系统设置 › 通用 › 语言与地区 › App」）；
// - Windows：Chrome 的 intl.app_locale（本地状态，同 Chrome 设置里的界面语言）；
// - Linux：同样存在 intl.app_locale，启动时由 WillInitResourceBundle 换成 LANGUAGE 环境
//   变量（Chrome 在 Linux 上只看环境变量）。
namespace starry::l10n {

// 选了的界面语言，跟随系统时是 nullopt。
std::optional<Language> ChosenLanguage();

// 改界面语言，重新启动后生效。nullopt 是跟随系统。
void SetChosenLanguage(std::optional<Language> language);

// 跟随系统时会用的语言：系统的首选语言里的第一个，Starry 不支持时是英文。
Language SystemLanguage();

std::vector<std::string> SystemLanguages();

}  // namespace starry::l10n

#endif  // STARRY_BROWSER_L10N_INTERFACE_LANGUAGE_H_
