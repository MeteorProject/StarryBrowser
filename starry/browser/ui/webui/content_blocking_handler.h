// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_CONTENT_BLOCKING_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_CONTENT_BLOCKING_HANDLER_H_

#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "starry/browser/content_blocking/content_blocking_service.h"

namespace starry {

// 设置 › 隐私的「广告与跟踪拦截」「跟踪保护」（settings.js 的 privacy_），数据来自
// ContentBlockingService。消息：
//   getContentBlocking                  → 状态
//   setContentBlocking [enabled|stripTrackingParameters, 布尔值]
//   setFilterListEnabled [ID, 布尔值]
//   addFilterList [网址]                 → {error}（成功时 error 是空的）
//   removeFilterList [ID]
//   updateFilterLists                   立即更新
//   getCustomRules                      → 文字；setCustomRules [文字]
//   setAllowlisted [网站, 布尔值]        加上或去掉「不拦截的网站」
// 有变化时发 content-blocking-changed。
class ContentBlockingHandler
    : public content::WebUIMessageHandler,
      public content_blocking::ContentBlockingService::Observer {
 public:
  ContentBlockingHandler();
  ContentBlockingHandler(const ContentBlockingHandler&) = delete;
  ContentBlockingHandler& operator=(const ContentBlockingHandler&) = delete;
  ~ContentBlockingHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // ContentBlockingService::Observer:
  void OnContentBlockingChanged() override;

 private:
  base::DictValue State() const;
  void HandleGet(const base::ListValue& args);
  void HandleSet(const base::ListValue& args);
  void HandleSetListEnabled(const base::ListValue& args);
  void HandleAddList(const base::ListValue& args);
  void HandleRemoveList(const base::ListValue& args);
  void HandleUpdate(const base::ListValue& args);
  void HandleGetCustomRules(const base::ListValue& args);
  void HandleSetCustomRules(const base::ListValue& args);
  void HandleSetAllowlisted(const base::ListValue& args);
  void OnListAdded(std::string callback_id, std::string error);

  base::ScopedObservation<content_blocking::ContentBlockingService,
                          content_blocking::ContentBlockingService::Observer>
      observation_{this};
  base::WeakPtrFactory<ContentBlockingHandler> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_CONTENT_BLOCKING_HANDLER_H_
