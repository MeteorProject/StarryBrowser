// Copyright 2026 The Starry Browser Authors

#include "starry/browser/l10n/l10n.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/containers/flat_map.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "starry/browser/resources/starry_l10n_resources.h"

namespace starry {

namespace l10n {

namespace {

// 语言文件里的一条：普通文字，或者按数量变化的写法（{"one": …, "other": …}）。
struct Entry {
  std::u16string other;
  std::optional<std::u16string> one;
};

using Table = base::flat_map<std::string, Entry, std::less<>>;

// 缺了某条文字时依次去找的语言：繁体先找简体，日文、韩文先找英文，最后都落到简体中文
// （源语言，总是齐全的）。
std::vector<Language> Fallbacks(Language language) {
  switch (language) {
    case Language::kSimplifiedChinese:
      return {Language::kSimplifiedChinese};
    case Language::kTraditionalChinese:
      return {Language::kTraditionalChinese, Language::kSimplifiedChinese};
    case Language::kEnglish:
      return {Language::kEnglish, Language::kSimplifiedChinese};
    case Language::kJapanese:
    case Language::kKorean:
      return {language, Language::kEnglish, Language::kSimplifiedChinese};
  }
}

// 读一种语言的文件。以 // 开头的键是给编辑看的说明，跳过。
std::map<std::string, Entry> LoadFile(Language language) {
  std::map<std::string, Entry> entries;
  const std::string file = base::StrCat({LanguageCode(language), ".json"});
  for (const L10nResource& resource : GetL10nResources()) {
    if (resource.path != file) {
      continue;
    }
    std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
        std::string_view(reinterpret_cast<const char*>(resource.data.data()),
                         resource.data.size()),
        base::JSON_PARSE_RFC);
    if (!dict) {
      LOG(ERROR) << "语言文件格式不对：" << file;
      return entries;
    }
    for (const auto [key, value] : *dict) {
      if (key.starts_with("//")) {
        continue;
      }
      if (const std::string* text = value.GetIfString()) {
        entries[key] = Entry{base::UTF8ToUTF16(*text), std::nullopt};
      } else if (const base::DictValue* forms = value.GetIfDict()) {
        const std::string* other = forms->FindString("other");
        if (!other) {
          continue;
        }
        Entry entry{base::UTF8ToUTF16(*other), std::nullopt};
        if (const std::string* one = forms->FindString("one")) {
          entry.one = base::UTF8ToUTF16(*one);
        }
        entries[key] = std::move(entry);
      }
    }
    break;
  }
  return entries;
}

class Catalog {
 public:
  static Catalog& Get() {
    static base::NoDestructor<Catalog> catalog;
    return *catalog;
  }

  Catalog() {
    const std::string app_locale =
        g_browser_process ? g_browser_process->GetApplicationLocale()
                          : std::string();
    language_ = LanguageMatching(app_locale).value_or(Language::kEnglish);
    format_locale_ = LanguageMatching(app_locale) == language_
                         ? app_locale
                         : std::string(LanguageCode(language_));
    // 从最后的后备语言叠起，前面的盖住后面的。
    std::map<std::string, Entry> merged;
    const std::vector<Language> chain = Fallbacks(language_);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      for (auto& [key, entry] : LoadFile(*it)) {
        merged[key] = std::move(entry);
      }
    }
    table_ = Table(std::make_move_iterator(merged.begin()),
                   std::make_move_iterator(merged.end()));
  }

  Language language() const { return language_; }
  const std::string& format_locale() const { return format_locale_; }
  const Table& table() const { return table_; }

  const Entry* Find(std::string_view key) const {
    auto it = table_.find(key);
    return it == table_.end() ? nullptr : &it->second;
  }

  // 没有的键：返回键本身，存起来让引用一直有效。
  const std::u16string& Missing(std::string_view key) {
    base::AutoLock lock(lock_);
    auto [it, inserted] =
        missing_.try_emplace(std::string(key), base::UTF8ToUTF16(key));
    if (inserted) {
      DLOG(WARNING) << "语言文件里没有「" << key << "」";
    }
    return it->second;
  }

 private:
  Language language_ = Language::kSimplifiedChinese;
  std::string format_locale_;
  Table table_;
  base::Lock lock_;
  std::map<std::string, std::u16string, std::less<>> missing_;
};

std::u16string Format(std::u16string_view text,
                      std::initializer_list<Arg> args) {
  if (text.find(u'{') == std::u16string_view::npos) {
    return std::u16string(text);
  }
  std::u16string result;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t open = text.find(u'{', pos);
    if (open == std::u16string_view::npos) {
      break;
    }
    result.append(text.substr(pos, open - pos));
    const size_t close = text.find(u'}', open + 1);
    const Arg* match = nullptr;
    if (close != std::u16string_view::npos) {
      const std::string name =
          base::UTF16ToUTF8(text.substr(open + 1, close - open - 1));
      for (const Arg& arg : args) {
        if (arg.name == name) {
          match = &arg;
          break;
        }
      }
    }
    if (match) {
      result.append(match->value);
      pos = close + 1;
    } else {
      result.push_back(u'{');
      pos = open + 1;
    }
  }
  result.append(text.substr(std::min(pos, text.size())));
  return result;
}

}  // namespace

Arg::Arg(std::string_view name, std::u16string_view value)
    : name(name), value(value) {}

Arg::Arg(std::string_view name, std::string_view utf8)
    : name(name), value(base::UTF8ToUTF16(utf8)) {}

Arg::Arg(std::string_view name, int64_t number, bool is_number)
    : name(name), value(base::NumberToString16(number)), number(number) {}

Arg::Arg(const Arg&) = default;

Arg::~Arg() = default;

Language CurrentLanguage() {
  return Catalog::Get().language();
}

std::string_view LanguageCode(Language language) {
  switch (language) {
    case Language::kSimplifiedChinese:
      return "zh-Hans";
    case Language::kTraditionalChinese:
      return "zh-Hant";
    case Language::kEnglish:
      return "en";
    case Language::kJapanese:
      return "ja";
    case Language::kKorean:
      return "ko";
  }
}

std::u16string NativeName(Language language) {
  switch (language) {
    case Language::kSimplifiedChinese:
      return u"简体中文";
    case Language::kTraditionalChinese:
      return u"繁體中文";
    case Language::kEnglish:
      return u"English";
    case Language::kJapanese:
      return u"日本語";
    case Language::kKorean:
      return u"한국어";
  }
}

std::string_view ChromeLocale(Language language) {
  switch (language) {
    case Language::kSimplifiedChinese:
      return "zh-CN";
    case Language::kTraditionalChinese:
      return "zh-TW";
    case Language::kEnglish:
      return "en-US";
    case Language::kJapanese:
      return "ja";
    case Language::kKorean:
      return "ko";
  }
}

std::optional<Language> LanguageMatching(std::string_view identifier) {
  std::string normalized(identifier);
  base::ReplaceChars(normalized, "_", "-", &normalized);
  const std::vector<std::string> parts = base::SplitString(
      normalized, "-", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.empty()) {
    return std::nullopt;
  }
  const std::string code = base::ToLowerASCII(parts[0]);
  if (code == "zh") {
    for (size_t i = 1; i < parts.size(); ++i) {
      const std::string part = base::ToLowerASCII(parts[i]);
      if (part.size() == 4) {
        return part == "hant" ? Language::kTraditionalChinese
                              : Language::kSimplifiedChinese;
      }
    }
    for (size_t i = 1; i < parts.size(); ++i) {
      const std::string region = base::ToUpperASCII(parts[i]);
      if (region == "TW" || region == "HK" || region == "MO") {
        return Language::kTraditionalChinese;
      }
    }
    return Language::kSimplifiedChinese;
  }
  if (code == "en") {
    return Language::kEnglish;
  }
  if (code == "ja") {
    return Language::kJapanese;
  }
  if (code == "ko") {
    return Language::kKorean;
  }
  return std::nullopt;
}

const std::string& FormatLocale() {
  return Catalog::Get().format_locale();
}

const std::string& PageStringsJson() {
  static const base::NoDestructor<std::string> json([] {
    const Catalog& catalog = Catalog::Get();
    base::DictValue strings;
    for (const auto& [key, entry] : catalog.table()) {
      if (entry.one) {
        strings.Set(key, base::DictValue()
                             .Set("one", *entry.one)
                             .Set("other", entry.other));
      } else {
        strings.Set(key, entry.other);
      }
    }
#if BUILDFLAG(IS_MAC)
    constexpr char kPlatform[] = "mac";
#elif BUILDFLAG(IS_WIN)
    constexpr char kPlatform[] = "win";
#else
    constexpr char kPlatform[] = "linux";
#endif
    base::DictValue root;
    root.Set("language", LanguageCode(catalog.language()));
    root.Set("locale", catalog.format_locale());
    root.Set("platform", kPlatform);
    root.Set("strings", std::move(strings));
    return base::WriteJson(root).value_or("{}");
  }());
  return *json;
}

std::u16string JoinList(const std::vector<std::u16string>& items) {
  return base::JoinString(items, L("common.listSeparator"));
}

const std::u16string& ShowInFolderLabel() {
#if BUILDFLAG(IS_MAC)
  return L("common.showInFinder");
#elif BUILDFLAG(IS_WIN)
  return L("common.showInExplorer");
#else
  return L("common.showInFolder");
#endif
}

}  // namespace l10n

const std::u16string& L(std::string_view key) {
  l10n::Catalog& catalog = l10n::Catalog::Get();
  if (const l10n::Entry* entry = catalog.Find(key)) {
    return entry->other;
  }
  return catalog.Missing(key);
}

std::u16string L(std::string_view key, std::initializer_list<l10n::Arg> args) {
  l10n::Catalog& catalog = l10n::Catalog::Get();
  const l10n::Entry* entry = catalog.Find(key);
  if (!entry) {
    return catalog.Missing(key);
  }
  std::u16string_view text = entry->other;
  if (entry->one && catalog.language() == l10n::Language::kEnglish) {
    for (const l10n::Arg& arg : args) {
      if (arg.name == "count" && arg.number == 1) {
        text = *entry->one;
      }
    }
  }
  return l10n::Format(text, args);
}

}  // namespace starry
