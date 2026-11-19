// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/starry_pages.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/extensions/extension_icon_source.h"
#include "chrome/browser/ui/webui/favicon_source.h"
#include "chrome/browser/ui/webui/fileicon_source.h"
#include "chrome/common/webui_url_constants.h"
#include "components/favicon_base/favicon_url_parser.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/webui_config.h"
#include "content/public/browser/webui_config_map.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "starry/browser/assistant/assistant_images.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/resources/starry_page_resources.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/webui/archive_handler.h"
#include "starry/browser/ui/webui/assistant_handler.h"
#include "starry/browser/ui/webui/assistant_settings_handler.h"
#include "starry/browser/ui/webui/bookmarks_handler.h"
#include "starry/browser/ui/webui/content_blocking_handler.h"
#include "starry/browser/ui/webui/downloads_handler.h"
#include "starry/browser/ui/webui/extensions_handler.h"
#include "starry/browser/ui/webui/history_handler.h"
#include "starry/browser/ui/webui/import_handler.h"
#include "starry/browser/ui/webui/page_handler.h"
#include "starry/browser/ui/webui/settings_handler.h"
#include "starry/common/internal_urls.h"
#include "url/gurl.h"

namespace starry {

namespace {

struct PageInfo {
  StarryPage page;
  const char* host;
  // 页面文件所在的目录（starry/browser/resources/pages/<dir>）。
  const char* dir;
};

constexpr PageInfo kPages[] = {
    {StarryPage::kHistory, chrome::kChromeUIHistoryHost, "history"},
    {StarryPage::kDownloads, chrome::kChromeUIDownloadsHost, "downloads"},
    {StarryPage::kBookmarks, chrome::kChromeUIBookmarksHost, "bookmarks"},
    {StarryPage::kSettings, kStarrySettingsHost, "settings"},
    {StarryPage::kAssistant, kStarryAssistantHost, "assistant"},
    {StarryPage::kArchive, kStarryArchiveHost, "archive"},
};

const PageInfo& InfoFor(StarryPage page) {
  for (const PageInfo& info : kPages) {
    if (info.page == page) {
      return info;
    }
  }
  NOTREACHED();
}

const PageResource* FindResource(std::string_view path) {
  for (const PageResource& resource : GetPageResources()) {
    if (resource.path == path) {
      return &resource;
    }
  }
  return nullptr;
}

std::string_view StripQuery(std::string_view path) {
  const size_t end = path.find_first_of("?#");
  return end == std::string_view::npos ? path : path.substr(0, end);
}

// 已归档页的缩略图：thumb/<id>.jpg。不是的时候是空的。
std::string_view ArchiveThumbnailId(const char* dir, std::string_view path) {
  if (std::string_view(dir) != "archive") {
    return {};
  }
  path = StripQuery(path);
  if (!path.starts_with("thumb/") || !path.ends_with(".jpg") ||
      path.size() <= 10) {
    return {};
  }
  return path.substr(6, path.size() - 10);
}

// 请求的路径对应哪个文件：common/… 是共用的，没有扩展名的（/、chrome://history/xxx 这类
// 页面里的路由）是入口，其余在页面自己的目录里。
const PageResource* ResolveResource(const char* dir, std::string_view raw_path) {
  const std::string_view path = StripQuery(raw_path);
  if (path.starts_with("common/")) {
    return FindResource(path);
  }
  if (path.find('.') == std::string_view::npos) {
    return FindResource(base::StrCat({dir, "/index.html"}));
  }
  return FindResource(base::StrCat({dir, "/", path}));
}

std::string CssColor(SkColor color) {
  return base::StringPrintf("rgb(%d %d %d / %.3f)", SkColorGetR(color),
                            SkColorGetG(color), SkColorGetB(color),
                            SkColorGetA(color) / 255.0);
}

std::string ThemeVariables(const Theme& theme) {
  const std::pair<const char*, SkColor> colors[] = {
      {"primary", theme.primary},
      {"primary-container", theme.primary_container},
      {"on-primary", theme.on_primary},
      {"secondary", theme.secondary},
      {"surface", theme.surface},
      {"surface-alt", theme.surface_alt},
      {"surface-panel", theme.surface_panel},
      {"surface-bright", theme.surface_bright},
      {"on-surface", theme.on_surface},
      {"on-surface-variant", theme.on_surface_variant},
      {"outline", theme.outline},
      {"outline-variant", theme.outline_variant},
      {"accent", theme.accent},
      {"warning", theme.warning()},
      {"danger", theme.danger()},
  };
  std::string css;
  for (const auto& [name, color] : colors) {
    base::StrAppend(&css, {"  --", name, ": ", CssColor(color), ";\n"});
  }
  base::StrAppend(&css, {"  --is-dark: ", theme.is_dark ? "1" : "0", ";\n",
                         "  color-scheme: ", theme.is_dark ? "dark" : "light",
                         ";\n"});
  return css;
}

// 主题的颜色（同 Views 的界面）：浅色、深色各一套，跟着窗口的深浅（prefers-color-scheme）；
// 无痕窗口固定是紫色那套。
std::string ThemeCss(bool incognito) {
  if (incognito) {
    return base::StrCat(
        {":root {\n", ThemeVariables(Theme::PrivateBrowsing()), "}\n"});
  }
  return base::StrCat(
      {":root {\n",
       ThemeVariables(Theme::Make(std::nullopt, /*dark=*/false,
                                  /*tint_surfaces=*/false)),
       "}\n@media (prefers-color-scheme: dark) {\n:root {\n",
       ThemeVariables(Theme::Make(std::nullopt, /*dark=*/true,
                                  /*tint_surfaces=*/false)),
       "}\n}\n"});
}

// 页面入口里的 $L{键}（标题）换成界面文字，$LANG 换成界面语言：页面脚本跑起来之前，
// 标签页的标题就是对的。
std::string LocalizeHtml(std::string_view html) {
  std::string result;
  size_t pos = 0;
  while (pos < html.size()) {
    const size_t mark = html.find('$', pos);
    if (mark == std::string_view::npos) {
      break;
    }
    result.append(html.substr(pos, mark - pos));
    const std::string_view rest = html.substr(mark);
    if (rest.starts_with("$LANG")) {
      result.append(l10n::LanguageCode(l10n::CurrentLanguage()));
      pos = mark + 5;
    } else if (const size_t close = rest.find('}');
               rest.starts_with("$L{") && close != std::string_view::npos) {
      result.append(base::EscapeForHTML(
          base::UTF16ToUTF8(L(rest.substr(3, close - 3)))));
      pos = mark + close + 1;
    } else {
      result.push_back('$');
      pos = mark + 1;
    }
  }
  result.append(html.substr(std::min(pos, html.size())));
  return result;
}

bool ShouldHandle(const char* dir, const std::string& path) {
  // 助手对话里的图片（assistant_images.h）。
  if (std::string_view(dir) == "assistant" &&
      assistant::IsImagePath(StripQuery(path))) {
    return true;
  }
  if (!ArchiveThumbnailId(dir, path).empty()) {
    return true;
  }
  return StripQuery(path) == "theme.css" ||
         StripQuery(path) == "strings.js" || ResolveResource(dir, path);
}

void HandleRequest(const char* dir,
                   Profile* profile,
                   const std::string& path,
                   content::WebUIDataSource::GotDataCallback callback) {
  const bool incognito = profile->IsOffTheRecord();
  if (std::string_view(dir) == "assistant" &&
      assistant::IsImagePath(StripQuery(path))) {
    std::move(callback).Run(
        assistant::FindImageForPath(std::string(StripQuery(path))));
    return;
  }
  // 数据源跟着用户资料走，这里的 profile 一定还在。
  if (const std::string_view id = ArchiveThumbnailId(dir, path); !id.empty()) {
    tab_organizer::TabArchive::ForProfile(profile)->ReadThumbnail(
        std::string(id), std::move(callback));
    return;
  }
  if (StripQuery(path) == "theme.css") {
    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(ThemeCss(incognito)));
    return;
  }
  // 界面文字（同 Views 的界面一份语言文件），common/l10n.js 读它。
  if (StripQuery(path) == "strings.js") {
    std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
        base::StrCat({"export default ", l10n::PageStringsJson(), ";\n"})));
    return;
  }
  const PageResource* resource = ResolveResource(dir, path);
  CHECK(resource);
  if (resource->path.ends_with(".html")) {
    std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
        LocalizeHtml(std::string_view(
            reinterpret_cast<const char*>(resource->data.data()),
            resource->data.size()))));
    return;
  }
  std::move(callback).Run(
      base::MakeRefCounted<base::RefCountedStaticMemory>(resource->data));
}

class StarryPageUIConfig : public content::WebUIConfig {
 public:
  explicit StarryPageUIConfig(const PageInfo& info)
      : content::WebUIConfig(content::kChromeUIScheme, info.host),
        page_(info.page) {}

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override {
    return std::make_unique<StarryPageUI>(web_ui, page_);
  }

 private:
  const StarryPage page_;
};

}  // namespace

StarryPageUI::StarryPageUI(content::WebUI* web_ui, StarryPage page)
    : content::WebUIController(web_ui) {
  const PageInfo& info = InfoFor(page);
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, info.host);
  source->SetRequestFilter(
      base::BindRepeating(&ShouldHandle, info.dir),
      base::BindRepeating(&HandleRequest, info.dir, profile));
  // 页面用 chrome://resources 的 Lit，模板不经过 Trusted Types。
  source->DisableTrustedTypesCSP();
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ImgSrc,
      "img-src chrome://favicon2 chrome://fileicon chrome://extension-icon "
      "chrome://resources 'self' data:;");
  // 网站图标（chrome://favicon2/?pageUrl=…）和文件图标（chrome://fileicon/?path=…）。
  content::URLDataSource::Add(
      profile, std::make_unique<FaviconSource>(
                   profile, chrome::FaviconUrlFormat::kFavicon2));
  content::URLDataSource::Add(profile, std::make_unique<FileIconSource>());
  // 扩展的图标（chrome://extension-icon/<id>/<大小>/<匹配>）。
  content::URLDataSource::Add(
      profile, std::make_unique<extensions::ExtensionIconSource>(profile));

  web_ui->AddMessageHandler(std::make_unique<StarryPageHandler>());
  switch (page) {
    case StarryPage::kHistory:
      web_ui->AddMessageHandler(std::make_unique<HistoryHandler>(profile));
      break;
    case StarryPage::kDownloads:
      web_ui->AddMessageHandler(std::make_unique<DownloadsHandler>(profile));
      break;
    case StarryPage::kBookmarks:
      web_ui->AddMessageHandler(std::make_unique<BookmarksHandler>(profile));
      break;
    case StarryPage::kSettings:
      web_ui->AddMessageHandler(std::make_unique<SettingsHandler>(profile));
      // 「扩展」分类（chrome://extensions 也到这里）。
      web_ui->AddMessageHandler(std::make_unique<ExtensionsHandler>(profile));
      // 「浏览历史」一组：条数、清除。
      web_ui->AddMessageHandler(std::make_unique<HistoryHandler>(profile));
      // 「AI 智能体」分类。
      web_ui->AddMessageHandler(
          std::make_unique<AssistantSettingsHandler>(profile));
      // 隐私 › 广告与跟踪拦截。
      web_ui->AddMessageHandler(std::make_unique<ContentBlockingHandler>());
      // 通用 › 导入浏览器数据。
      web_ui->AddMessageHandler(std::make_unique<ImportHandler>(profile));
      break;
    case StarryPage::kAssistant:
      web_ui->AddMessageHandler(std::make_unique<AssistantHandler>());
      break;
    case StarryPage::kArchive:
      web_ui->AddMessageHandler(std::make_unique<ArchiveHandler>(profile));
      break;
  }
}

StarryPageUI::~StarryPageUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(StarryPageUI)

void RegisterStarryPages() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  content::WebUIConfigMap& map = content::WebUIConfigMap::GetInstance();
  for (const PageInfo& info : kPages) {
    // Chrome 自己的同名页面拿掉，换成 Starry 的。
    map.RemoveConfig(GURL(base::StrCat(
        {content::kChromeUIScheme, url::kStandardSchemeSeparator, info.host})));
    map.AddWebUIConfig(std::make_unique<StarryPageUIConfig>(info));
  }
}

GURL StarrySettingsUrl(std::string_view pane) {
  return ToStarryURL(GURL(
      base::StrCat({content::kChromeUIScheme, url::kStandardSchemeSeparator,
                    kStarrySettingsHost, "/", pane})));
}

void OpenStarrySettings(BrowserWindowInterface* browser,
                        std::string_view pane) {
  const GURL url = StarrySettingsUrl(pane);
  Profile* profile = browser->GetProfile();
  if (profile->IsOffTheRecord()) {
    ShowSingletonTabOverwritingNTP(profile->GetOriginalProfile(), url,
                                   NavigateParams::IGNORE_AND_NAVIGATE);
    return;
  }
  // 已经开着设置页时切过去（指定了分类时换到那个分类），否则开一个。
  TabStripModel* model = browser->GetTabStripModel();
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    const GURL& current = contents->GetLastCommittedURL();
    if (!current.SchemeIs(content::kChromeUIScheme) ||
        current.host() != kStarrySettingsHost) {
      continue;
    }
    if (!pane.empty() && current.path() != url.path()) {
      contents->GetController().LoadURL(url, content::Referrer(),
                                        ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                                        std::string());
    }
    model->ActivateTabAt(i);
    return;
  }
  ShowSingletonTabOverwritingNTP(browser, url);
}

GURL StarryArchiveUrl(std::u16string_view query) {
  std::string url = base::StrCat(
      {kUIScheme, url::kStandardSchemeSeparator, kStarryArchiveHost, "/"});
  if (!query.empty()) {
    base::StrAppend(
        &url, {"?q=", base::EscapeQueryParamValue(base::UTF16ToUTF8(query),
                                                  /*use_plus=*/false)});
  }
  return GURL(url);
}

void OpenStarryArchive(BrowserWindowInterface* browser,
                       std::u16string_view query) {
  const GURL url = StarryArchiveUrl(query);
  Profile* profile = browser->GetProfile();
  if (profile->IsOffTheRecord()) {
    ShowSingletonTabOverwritingNTP(profile->GetOriginalProfile(), url,
                                   NavigateParams::IGNORE_AND_NAVIGATE);
    return;
  }
  // 已经开着时切过去（带着搜索词时换成这次的），否则开一个。
  TabStripModel* model = browser->GetTabStripModel();
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    const GURL current = ToStarryURL(contents->GetLastCommittedURL());
    if (!current.SchemeIs(kUIScheme) || current.host() != kStarryArchiveHost) {
      continue;
    }
    if (!query.empty()) {
      contents->GetController().LoadURL(url, content::Referrer(),
                                        ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                                        std::string());
    }
    model->ActivateTabAt(i);
    return;
  }
  ShowSingletonTabOverwritingNTP(browser, url,
                                 NavigateParams::IGNORE_AND_NAVIGATE);
}

bool HasPageResourceForTesting(std::string_view path) {
  return FindResource(path) != nullptr;
}

}  // namespace starry
