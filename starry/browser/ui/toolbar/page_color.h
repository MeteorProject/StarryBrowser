// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TOOLBAR_PAGE_COLOR_H_
#define STARRY_BROWSER_UI_TOOLBAR_PAGE_COLOR_H_

#include <optional>

#include "base/functional/callback.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/ui/design/theme.h"
#include "third_party/skia/include/core/SkColor.h"

class SkBitmap;

namespace content {
class WebContents;
}

namespace starry {

// 网页顶边采样色：供紧凑地址栏与网页融为一体。按标签缓存，切回标签时保留；顶边为
// 图片、渐变或细色带无法取色时为空。
class PageTopColor : public content::WebContentsUserData<PageTopColor> {
 public:
  ~PageTopColor() override;

  static std::optional<SkColor> Get(content::WebContents* contents);
  static void Set(content::WebContents* contents, std::optional<SkColor> color);

 private:
  friend class content::WebContentsUserData<PageTopColor>;
  explicit PageTopColor(content::WebContents* contents);

  std::optional<SkColor> color_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

// 截取网页视口顶部区域采样顶边主色：沿顶边与下方 12px 处各采样 7 个点，顶边至少
// 6 个、下方至少 5 个点颜色接近时返回顶边平均色，否则为空。无法截图时（页面未显示）
// 不调用 done。
void SamplePageTopColor(content::WebContents* contents,
                        base::OnceCallback<void(std::optional<SkColor>)> done);
std::optional<SkColor> UniformTopColor(const SkBitmap& bitmap);

// 紧凑地址栏配色：底色依次回退取网页顶边采样色、theme-color、页面底色；文字与图标
// 按底色明暗独立生成主题（与窗口深浅无关）。起始页、内部页面、错误页、无痕窗口以及
// 关闭「跟随网页的主题色」时沿用内容卡片底色与窗口主题。
struct CompactToolbarStyle {
  SkColor background = SK_ColorTRANSPARENT;
  Theme theme;

  static CompactToolbarStyle For(content::WebContents* contents,
                                 const Theme& window_theme,
                                 bool follows_page);
  static CompactToolbarStyle Lerp(const CompactToolbarStyle& from,
                                  const CompactToolbarStyle& to,
                                  double t);
  bool operator==(const CompactToolbarStyle&) const = default;
};

// 网页能不能给界面取色：普通网页（不是空白页、浏览器自己的页面、崩溃或出错的网页）。
bool CanTintFromPage(content::WebContents* contents);
// sRGB 的相对亮度（0 黑 1 白）。
double RelativeLuminance(SkColor color);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TOOLBAR_PAGE_COLOR_H_
