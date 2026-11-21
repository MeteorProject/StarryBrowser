// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_LOCATION_LOCATION_PICKER_H_
#define STARRY_BROWSER_UI_LOCATION_LOCATION_PICKER_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"

class BrowserWindowInterface;

namespace content {
class WebContents;
}

namespace starry {

class StarryBrowserView;

// 位置保存后回调 true，取消或无法打开时回调 false。
using LocationPickedCallback = base::OnceCallback<void(bool)>;

// 在窗口的对话框层里打开选点页（starry://location-picker，一张 760×540 的卡片，
// 窗口小时缩小）。不传 site 是全局的模拟位置，否则是那个网站（主站点）的。规则存在这个
// 窗口的用户资料里：无痕窗口里改的网站设置只放在内存里。
void ShowLocationPicker(BrowserWindowInterface* browser,
                        std::optional<std::string> site,
                        LocationPickedCallback callback = {});

// 选点页由谁打开（LocationPickerHandler 用）。单独开在标签里的选点页没有。
class LocationPickerOwner {
 public:
  static LocationPickerOwner* FromWebContents(content::WebContents* contents);

  virtual const std::string& site() const = 0;
  virtual StarryBrowserView* browser_view() const = 0;
  // 选好了或取消了：关掉卡片，回调 ShowLocationPicker 的 callback。
  virtual void Finish(bool saved) = 0;

 protected:
  virtual ~LocationPickerOwner() = default;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_LOCATION_LOCATION_PICKER_H_
