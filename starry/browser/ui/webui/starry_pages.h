// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_STARRY_PAGES_H_
#define STARRY_BROWSER_UI_WEBUI_STARRY_PAGES_H_

#include <string_view>

#include "content/public/browser/web_ui_controller.h"

class BrowserWindowInterface;
class GURL;

namespace starry {

// Starry 的整页界面，用网页技术写，放在内容卡片里当标签页：
//   starry://history    历史记录
//   starry://downloads  下载
//   starry://bookmarks  书签
//   starry://settings   设置
//   starry://archive    已归档的标签页（标签管理）
// 底层注册在 Chrome 的 WebUI 源，保留 Chrome 自己的链接和扩展的兼容性；
// 公开地址的转换见 starry/common/internal_urls.h。原生补充设置在
// starry://browser-settings。
//
// 页面的文件编在程序里（starry/browser/resources/），按路径取：/ 是 <页面>/index.html，
// /common/… 是共用的，/theme.css 是按窗口的主题生成的颜色（深浅两套，无痕窗口是紫色那套）。
// 页面用 chrome.send 和浏览器说话：共用的菜单、对话框、打开网址在 StarryPageHandler，
// 每页的数据在各自的 handler 里。
enum class StarryPage {
  kHistory,
  kDownloads,
  kBookmarks,
  kSettings,
  // AI 助手的岛里的网页（不是标签页）。
  kAssistant,
  kArchive,
  kWelcome,
};

inline constexpr char kStarrySettingsHost[] = "starry-settings";
// 公开地址是 starry://welcome（不用 Chrome 自己的 chrome://welcome）。
inline constexpr char kStarryWelcomeHost[] = "starry-welcome";
// Chrome 没有这个页面，公开地址就是 starry://archive。
inline constexpr char kStarryArchiveHost[] = "archive";
inline constexpr char kStarryAssistantHost[] = "starry-assistant";

// 设置页的地址；pane 是分类（general、appearance、performance、downloads、privacy、
// extensions、developer），空的是上次看的那个。
GURL StarrySettingsUrl(std::string_view pane = {});

// 打开设置页（已经开着的标签就切过去）。无痕窗口里在普通窗口打开（同 Chrome 的设置）。
void OpenStarrySettings(BrowserWindowInterface* browser,
                        std::string_view pane = {});

// 整页的「已归档」（已经开着的就切过去）。query 不是空的时候带上搜索词。无痕窗口里
// 在普通窗口打开。
GURL StarryArchiveUrl(std::u16string_view query = {});
void OpenStarryArchive(BrowserWindowInterface* browser,
                       std::u16string_view query = {});

class StarryPageUI : public content::WebUIController {
 public:
  StarryPageUI(content::WebUI* web_ui, StarryPage page);
  StarryPageUI(const StarryPageUI&) = delete;
  StarryPageUI& operator=(const StarryPageUI&) = delete;
  ~StarryPageUI() override;

 private:
  WEB_UI_CONTROLLER_TYPE_DECL();
};

// 换掉 Chrome 同一地址的页面。在 Chrome 注册完自己的页面之后、打开第一个页面之前调用
// （StarryBrowserMainExtraParts::PostProfileInit）。
void RegisterStarryPages();

// 测试用：页面文件里有没有这个路径。
bool HasPageResourceForTesting(std::string_view path);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_STARRY_PAGES_H_
