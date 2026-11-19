// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_POPOVER_ZOOM_INDICATOR_H_
#define STARRY_BROWSER_UI_POPOVER_ZOOM_INDICATOR_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "starry/browser/ui/popover/popover_host.h"

class BrowserWindowInterface;

namespace views {
class Label;
}

namespace starry {

class IconButton;
class PillButton;

// 缩放网页（⌘+ ⌘- ⌘0、触控板捏合）时内容卡片右上角的小条，换掉 Chrome 挂在地址栏上的
// 缩放气泡：放大镜、百分比、减小、放大，不是 100% 时有「重置」。1.5 秒后自己收起，鼠标
// 停在上面时不收（外观与查找栏一致）。
class ZoomIndicator : public PopoverContent {
  METADATA_HEADER(ZoomIndicator, PopoverContent)

 public:
  explicit ZoomIndicator(BrowserWindowInterface* browser);
  ZoomIndicator(const ZoomIndicator&) = delete;
  ZoomIndicator& operator=(const ZoomIndicator&) = delete;
  ~ZoomIndicator() override;

  // 按当前标签的缩放比例刷新。
  void Update();
  std::u16string TextForTesting() const;

  // PopoverContent:
  gfx::Size ContentSize() const override;
  void SetTheme(const Theme& theme) override;

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;

 private:
  void Run(int command);

  const raw_ptr<BrowserWindowInterface> browser_;
  Theme theme_ = Theme::DarkBase();
  bool at_default_ = true;
  raw_ptr<views::Label> percent_ = nullptr;
  raw_ptr<IconButton> minus_ = nullptr;
  raw_ptr<IconButton> plus_ = nullptr;
  raw_ptr<PillButton> reset_ = nullptr;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_POPOVER_ZOOM_INDICATOR_H_
