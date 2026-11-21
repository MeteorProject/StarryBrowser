// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_NEW_TAB_NEW_TAB_H_
#define STARRY_BROWSER_NEW_TAB_NEW_TAB_H_

#include <optional>
#include <string_view>

#include "base/functional/callback.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"

class GURL;
class Profile;

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

// 新标签页的三种风格（无痕窗口固定为极简）：
// - 极简：⌘T 只弹出命令栏，空白标签为 about:blank。
// - 卡片：⌘T 打开 starry://newtab，命令栏停靠在页面搜索框上。
// - 全屏浮层：⌘T 打开带 CommandBarHome 的模糊浮层，空白标签仍为 starry://newtab。
//
// chrome://newtab 内部改写为 chrome://starry-newtab，但 virtual URL 还原为
// chrome://newtab，以保留 Chrome 对新标签页的默认聚焦与会话恢复行为。
namespace starry::new_tab {

enum class Style {
  kMinimal,
  kCard,
  kOverlay,
};

inline constexpr char kHost[] = "starry-newtab";

Style GetStyle(Profile* profile);

// 不看显示地址（极简空白页也显示 chrome://newtab），仅在已载入或正在载入页面时为真。
bool IsNewTabPage(content::WebContents* contents);

// BrowserURLHandler 一对：按风格改写为新标签页或 about:blank，反向还原为 chrome://newtab。
bool RewriteURL(GURL* url, content::BrowserContext* context);
bool ReverseURL(GURL* url, content::BrowserContext* context);

// 页面上报前的默认搜索框位置（CSS 像素，与 newtab.css 布局公式一致）。
gfx::Rect DefaultSearchRect(const gfx::Size& page);

bool IsWidgetId(std::string_view id);
bool WidgetEnabled(Profile* profile, std::string_view id);

// 每个新标签页一份：记录页面上报的搜索框位置及命令栏停靠状态。
class NewTabState : public content::WebContentsUserData<NewTabState> {
 public:
  ~NewTabState() override;

  const std::optional<gfx::RectF>& search_rect() const { return search_rect_; }
  void set_search_rect(const gfx::RectF& rect) { search_rect_ = rect; }

  bool search_docked() const { return search_docked_; }
  void SetSearchDocked(bool docked);
  void set_docked_listener(base::RepeatingCallback<void(bool)> listener) {
    docked_listener_ = std::move(listener);
  }

 private:
  friend class content::WebContentsUserData<NewTabState>;
  explicit NewTabState(content::WebContents* contents);

  std::optional<gfx::RectF> search_rect_;
  bool search_docked_ = false;
  base::RepeatingCallback<void(bool)> docked_listener_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace starry::new_tab

#endif  // STARRY_BROWSER_NEW_TAB_NEW_TAB_H_
