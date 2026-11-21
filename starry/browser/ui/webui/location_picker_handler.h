// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_LOCATION_PICKER_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_LOCATION_PICKER_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace network {
class SimpleURLLoader;
}

namespace starry {

// 对话框由 LocationPickerOwner 指定网站，独立标签页从查询参数读取。
class LocationPickerHandler : public content::WebUIMessageHandler {
 public:
  explicit LocationPickerHandler(Profile* profile);
  LocationPickerHandler(const LocationPickerHandler&) = delete;
  LocationPickerHandler& operator=(const LocationPickerHandler&) = delete;
  ~LocationPickerHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptDisallowed() override;

 private:
  // 空字符串是全局位置，nullopt 表示网址指定了无效的网站。
  std::optional<std::string> Site();
  void Close(bool saved);
  void HandleGet(const base::ListValue& args);
  void HandlePick(const base::ListValue& args);
  void HandleCancel(const base::ListValue& args);
  void HandleFetch(const base::ListValue& args);
  void Fetched(std::string callback_id,
               network::SimpleURLLoader* loader,
               std::optional<std::string> body);

  // 选点页的用户资料（无痕窗口里是无痕的那个）。
  const raw_ptr<Profile> profile_;
  // 正在取的瓦片（地图一次要好几张）。
  std::map<network::SimpleURLLoader*, std::unique_ptr<network::SimpleURLLoader>>
      loaders_;
  base::WeakPtrFactory<LocationPickerHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_LOCATION_PICKER_HANDLER_H_
