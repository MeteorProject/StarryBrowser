// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_POPOVER_CONTENT_BLOCKING_POPOVER_H_
#define STARRY_BROWSER_UI_POPOVER_CONTENT_BLOCKING_POPOVER_H_

#include <memory>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/permission_controller.h"
#include "starry/browser/content_blocking/content_blocking_service.h"
#include "starry/browser/content_blocking/content_filter_tab_helper.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "ui/menus/simple_menu_model.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace content {
class RenderFrameHost;
class WebContents;
}

namespace views {
class Label;
}

namespace starry {

class LocationRow;
class PillButton;
class PrivacySettingsRow;
class SwitchButton;

// 地址栏的盾牌点开的卡片：盾牌图块、「隐私保护」和网站；这个网页拦下了几个请求（或者
// 「此网站已停用拦截」）；「拦截广告和跟踪器」开关（改了以后出现「重新载入以生效」）；
// 「位置」一行（定位权限或当前位置来源，点开是菜单：跟随隐私设置、选择模拟位置…、
// 使用真实位置）；「隐私设置…」。数字跟着网页实时变。
class ContentBlockingPopover
    : public PopoverContent,
      public content_blocking::ContentBlockingService::Observer,
      public content_blocking::ContentFilterTabHelper::Observer,
      public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(ContentBlockingPopover, PopoverContent)

 public:
  ContentBlockingPopover(BrowserWindowInterface* browser,
                         PopoverHost* host,
                         content::WebContents* contents);
  ContentBlockingPopover(const ContentBlockingPopover&) = delete;
  ContentBlockingPopover& operator=(const ContentBlockingPopover&) = delete;
  ~ContentBlockingPopover() override;

  // 调试步骤：按开关、重新载入，看文字。
  void ToggleForTesting();
  void ToggleFingerprintForTesting();
  void ReloadForTesting() { Reload(); }
  std::u16string DescribeForTesting() const;

  // PopoverContent:
  gfx::Size ContentSize() const override;
  void SetTheme(const Theme& theme) override;

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;

  // ContentBlockingService::Observer:
  void OnContentBlockingChanged() override;
  // ContentFilterTabHelper::Observer:
  void OnBlockedCountChanged(content::WebContents* contents) override;

  // ui::SimpleMenuModel::Delegate（「位置」的菜单）:
  bool IsCommandIdChecked(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  void ShowLocationMenu();
  void OnLocationSimulationChanged();
  void ObserveLocationPermission();
  void UpdateLocationValue();
  content::RenderFrameHost* LocationFrame() const;
  std::u16string LocationValue() const;
  void Update();
  void SiteToggled(bool block);
  void FingerprintToggled(bool protect);
  void OnFingerprintProtectionChanged();
  void Reload();
  void OpenSettings();
  bool Allowlisted() const;

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<PopoverHost> host_;
  base::WeakPtr<content::WebContents> contents_;
  GURL url_;
  Theme theme_ = Theme::DarkBase();
  // 改过这个网站的开关：出现「重新载入以生效」。
  bool changed_ = false;
  PrefChangeRegistrar fingerprint_prefs_;

  raw_ptr<views::Label> title_ = nullptr;
  raw_ptr<views::Label> site_ = nullptr;
  raw_ptr<views::Label> status_ = nullptr;
  raw_ptr<views::Label> toggle_label_ = nullptr;
  raw_ptr<SwitchButton> switch_ = nullptr;
  raw_ptr<views::Label> fingerprint_label_ = nullptr;
  raw_ptr<SwitchButton> fingerprint_switch_ = nullptr;
  raw_ptr<PillButton> reload_ = nullptr;
  raw_ptr<LocationRow> location_ = nullptr;
  raw_ptr<PrivacySettingsRow> settings_ = nullptr;
  std::unique_ptr<ui::SimpleMenuModel> location_menu_;
  base::CallbackListSubscription location_subscription_;
  std::unique_ptr<content::PermissionController::PermissionSubscription>
      location_permission_subscription_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_POPOVER_CONTENT_BLOCKING_POPOVER_H_
