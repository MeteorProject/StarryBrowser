// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TOOLBAR_CONTENT_BLOCKING_BUTTON_H_
#define STARRY_BROWSER_UI_TOOLBAR_CONTENT_BLOCKING_BUTTON_H_

#include <string>

#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/button/button.h"

namespace starry {

// 地址栏内的内容拦截盾牌按钮：盾牌后显示当前页拦截计数（超过 99 显示 99+），有计数
// 时以强调色高亮；对该站点停用拦截时显示带斜线的盾牌。透明度由地址栏控制（有拦截
// 计数或停用拦截时常显，其余仅悬停时显示）。
class ContentBlockingButton : public views::Button {
  METADATA_HEADER(ContentBlockingButton, views::Button)

 public:
  explicit ContentBlockingButton(PressedCallback callback);
  ContentBlockingButton(const ContentBlockingButton&) = delete;
  ContentBlockingButton& operator=(const ContentBlockingButton&) = delete;
  ~ContentBlockingButton() override;

  void SetState(int count, bool allowlisted, bool fingerprint_protected);
  void SetTheme(const Theme& theme);
  void SetAlpha(float alpha);
  int count() const { return count_; }
  bool allowlisted() const { return allowlisted_; }
  // 一直显示（有数字、关了拦截）。
  bool sticky() const { return count_ > 0 || allowlisted_; }

  // views::Button:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void PaintButtonContents(gfx::Canvas* canvas) override;
  std::u16string GetRenderedTooltipText(const gfx::Point& p) const override;

 private:
  std::u16string CountText() const;

  int count_ = 0;
  bool allowlisted_ = false;
  bool fingerprint_protected_ = false;
  float alpha_ = 0;
  Theme theme_ = Theme::DarkBase();
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TOOLBAR_CONTENT_BLOCKING_BUTTON_H_
