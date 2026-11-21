// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_INTERNAL_PAGES_H_
#define STARRY_BROWSER_UI_INTERNAL_PAGES_H_

class GURL;

namespace gfx {
struct VectorIcon;
}

namespace starry {

// 浏览器自己的整页界面，接受公开的 starry:// 地址和底层的 WebUI 地址。
enum class InternalPage {
  kNone,
  kHistory,
  kBookmarks,
  kDownloads,
  kExtensions,
  kSettings,
  // 已归档的标签页（starry://archive）。
  kArchive,
  // 设置向导（starry://welcome）。
  kWelcome,
};

InternalPage InternalPageForUrl(const GURL& url);

// 标签和地址栏里代替网站图标的图标；kNone 时为空。
const gfx::VectorIcon* InternalPageIcon(InternalPage page);

// 空白的标签：还没有网址、about:blank、新标签页。地址栏显示提示文字，标题是「新标签页」，
// 不能重新载入、复制、固定。
bool IsBlankTabUrl(const GURL& url);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_INTERNAL_PAGES_H_
