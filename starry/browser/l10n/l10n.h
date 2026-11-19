// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_L10N_L10N_H_
#define STARRY_BROWSER_L10N_L10N_H_

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// 界面文字的多语言模块。语言文件位于 starry/browser/resources/l10n/<语言>.json。
//
//   label->SetText(L("downloads.page.title"));
//   L("downloads.quit.paused", {{"count", n}})
//
// 键一律写成字面量，scripts/l10n.py check 才能核对。整页界面（网页）用的是同一份文字，
// 见 resources/pages/common/l10n.js。
namespace starry {

namespace l10n {

// 浏览器界面支持的语言。
enum class Language {
  kSimplifiedChinese,
  kTraditionalChinese,
  kEnglish,
  kJapanese,
  kKorean,
};

inline constexpr auto kAllLanguages = std::to_array<Language>(
    {Language::kSimplifiedChinese, Language::kTraditionalChinese,
     Language::kEnglish, Language::kJapanese, Language::kKorean});

// 界面语言：跟着 Chrome 的界面语言（macOS 上是这个 App 的语言，Windows 上是 Chrome 的
// 「界面语言」设置，Linux 上是 LANGUAGE / LANG），Starry 不支持的语言用英文。
// 启动后不变，改了要重新启动。
Language CurrentLanguage();

// 语言文件的名字，也是 BCP 47 的写法：zh-Hans、zh-Hant、en、ja、ko。
std::string_view LanguageCode(Language language);

// 用这种语言自己写的名字（语言菜单里显示，不翻译）。
std::u16string NativeName(Language language);

// Chrome 认的语言代码（zh-CN、zh-TW、en-US、ja、ko）：写进 Chrome 的界面语言设置用。
std::string_view ChromeLocale(Language language);

// BCP 47 写法的语言（zh-TW、zh-Hant-HK、en-GB、ja-JP……）对应的界面语言，不支持的是
// nullopt。没写文字的中文按地区分：台湾、香港、澳门是繁体，其他是简体。
std::optional<Language> LanguageMatching(std::string_view identifier);

// 界面上日期、数字的格式用的区域：Chrome 的界面语言和 Starry 的一致时就是它（带地区，
// 比如 en-GB），否则是 Starry 的界面语言。
const std::string& FormatLocale();

// L() 的一个参数：语言文件里的 {name} 换成 value。整数参数 count 还用来选复数写法。
struct Arg {
  Arg(std::string_view name, std::u16string_view value);
  // UTF-8 的文字。
  Arg(std::string_view name, std::string_view utf8);
  template <typename T>
    requires std::is_integral_v<T>
  Arg(std::string_view name, T number)
      : Arg(name, static_cast<int64_t>(number), /*is_number=*/true) {}
  Arg(const Arg&);
  ~Arg();

  std::string_view name;
  std::u16string value;
  std::optional<int64_t> number;

 private:
  Arg(std::string_view name, int64_t number, bool is_number);
};

// 整页界面用的文字表：{"language", "locale", "platform", "strings"} 的 JSON，后备语言
// 已经并进去了。common/l10n.js 读它（/strings.js）。
const std::string& PageStringsJson();

// 把几项连成一个列表（「A、B」/「A, B」）。
std::u16string JoinList(const std::vector<std::u16string>& items);

// 「在访达中显示」（macOS）、「在资源管理器中显示」（Windows）、「在文件夹中显示」
// （Linux）。
const std::u16string& ShowInFolderLabel();

}  // namespace l10n

// 键对应的文字。哪种语言都没有这个键时是键本身（scripts/l10n.py check 会报出来）。
// 返回的引用一直有效。
const std::u16string& L(std::string_view key);

// 带参数的文字：{name} 换成参数，没有对应参数的花括号原样保留。
std::u16string L(std::string_view key, std::initializer_list<l10n::Arg> args);

}  // namespace starry

#endif  // STARRY_BROWSER_L10N_L10N_H_
