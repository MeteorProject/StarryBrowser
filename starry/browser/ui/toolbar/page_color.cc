// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/toolbar/page_color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/page_type.h"
#include "starry/browser/ui/internal_pages.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace starry {

namespace {

// 截图的高度（DIP）：顶边一排，最下面一排检查颜色有没有往下延续（排除顶上一条细色带）。
constexpr int kDepth = 12;
// 截图最宽多少像素：颜色只看大块，缩小了也够用。
constexpr int kMaxSnapshotWidth = 320;
// 每排取几个点。
constexpr int kColumns = 7;
// 两个颜色算「接近」的最大距离（sRGB 0–255 的欧氏距离）。
constexpr double kTolerance = 10;
// 底色相对亮度低于该阈值时使用浅色前景文字。
constexpr double kDarkLuminance = 0.2;

struct Rgb {
  double r = 0;
  double g = 0;
  double b = 0;

  double DistanceTo(const Rgb& other) const {
    return std::sqrt((r - other.r) * (r - other.r) +
                     (g - other.g) * (g - other.g) +
                     (b - other.b) * (b - other.b));
  }
};

std::optional<SkColor> Opaque(std::optional<SkColor> color) {
  // 几乎透明的（theme-color 写成 transparent）不算，换下一个。
  if (!color || SkColorGetA(*color) <= 128) {
    return std::nullopt;
  }
  return SkColorSetA(*color, 0xFF);
}

}  // namespace

WEB_CONTENTS_USER_DATA_KEY_IMPL(PageTopColor);

PageTopColor::PageTopColor(content::WebContents* contents)
    : content::WebContentsUserData<PageTopColor>(*contents) {}

PageTopColor::~PageTopColor() = default;

// static
std::optional<SkColor> PageTopColor::Get(content::WebContents* contents) {
  PageTopColor* data = contents ? FromWebContents(contents) : nullptr;
  return data ? data->color_ : std::nullopt;
}

// static
void PageTopColor::Set(content::WebContents* contents,
                       std::optional<SkColor> color) {
  CreateForWebContents(contents);
  FromWebContents(contents)->color_ = color;
}

std::optional<SkColor> UniformTopColor(const SkBitmap& bitmap) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  if (width < kColumns || height < 2) {
    return std::nullopt;
  }
  // 左右各让开一点：右边可能是常显的滚动条。
  const auto row = [&](int y) {
    std::vector<Rgb> points;
    for (int i = 0; i < kColumns; ++i) {
      const double fraction = 0.03 + 0.92 * i / (kColumns - 1);
      const int x = std::min(static_cast<int>(width * fraction), width - 1);
      const SkColor color = bitmap.getColor(x, y);
      // 透明的像素（网页不画底色时）不算。
      if (SkColorGetA(color) < 250) {
        continue;
      }
      points.push_back(Rgb{static_cast<double>(SkColorGetR(color)),
                           static_cast<double>(SkColorGetG(color)),
                           static_cast<double>(SkColorGetB(color))});
    }
    return points;
  };
  const std::vector<Rgb> top = row(0);
  const std::vector<Rgb> below = row(height - 1);
  if (top.empty()) {
    return std::nullopt;
  }
  const auto near = [](const std::vector<Rgb>& points, const Rgb& reference) {
    return std::count_if(points.begin(), points.end(), [&](const Rgb& point) {
      return point.DistanceTo(reference) <= kTolerance;
    });
  };
  // 以和最多点接近的那个为准。
  const Rgb* reference = &top[0];
  long best = 0;
  for (const Rgb& point : top) {
    const long count = near(top, point);
    if (count > best) {
      best = count;
      reference = &point;
    }
  }
  if (near(top, *reference) < kColumns - 1 ||
      near(below, *reference) < kColumns - 2) {
    return std::nullopt;
  }
  Rgb sum;
  int count = 0;
  for (const Rgb& point : top) {
    if (point.DistanceTo(*reference) <= kTolerance) {
      sum.r += point.r;
      sum.g += point.g;
      sum.b += point.b;
      ++count;
    }
  }
  return SkColorSetRGB(static_cast<U8CPU>(std::lround(sum.r / count)),
                       static_cast<U8CPU>(std::lround(sum.g / count)),
                       static_cast<U8CPU>(std::lround(sum.b / count)));
}

void SamplePageTopColor(content::WebContents* contents,
                        base::OnceCallback<void(std::optional<SkColor>)> done) {
  content::RenderWidgetHostView* view =
      contents ? contents->GetRenderWidgetHostView() : nullptr;
  if (!view || !view->IsShowing()) {
    return;
  }
  const gfx::Size size = view->GetViewBounds().size();
  if (size.width() < 40 || size.height() < kDepth) {
    return;
  }
  const int out_width = std::min(size.width(), kMaxSnapshotWidth);
  const int out_height =
      std::max(2, kDepth * out_width / std::max(size.width(), 1));
  view->CopyFromSurface(
      gfx::Rect(0, 0, size.width(), kDepth), gfx::Size(out_width, out_height),
      base::Seconds(1),
      base::BindOnce(
          [](base::OnceCallback<void(std::optional<SkColor>)> done,
             const content::CopyFromSurfaceResult& result) {
            if (!result.has_value() || result->bitmap.drawsNothing()) {
              return;
            }
            std::move(done).Run(UniformTopColor(result->bitmap));
          },
          std::move(done)));
}

bool CanTintFromPage(content::WebContents* contents) {
  if (!contents || contents->IsCrashed()) {
    return false;
  }
  const GURL& url = contents->GetVisibleURL();
  if (IsBlankTabUrl(url) || InternalPageForUrl(url) != InternalPage::kNone) {
    return false;
  }
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  return !entry || entry->GetPageType() != content::PAGE_TYPE_ERROR;
}

double RelativeLuminance(SkColor color) {
  const auto linear = [](U8CPU channel) {
    const double c = channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(SkColorGetR(color)) +
         0.7152 * linear(SkColorGetG(color)) +
         0.0722 * linear(SkColorGetB(color));
}

// static
CompactToolbarStyle CompactToolbarStyle::For(content::WebContents* contents,
                                             const Theme& window_theme,
                                             bool follows_page) {
  CompactToolbarStyle style;
  style.background = window_theme.surface_panel;
  style.theme = window_theme;
  if (!follows_page || !CanTintFromPage(contents)) {
    return style;
  }
  // 顶边的取色最接近紧挨着地址栏的那一排像素，theme-color
  // 是网站声明的品牌色，未必和顶 部一致。
  std::optional<SkColor> color = Opaque(PageTopColor::Get(contents));
  if (!color) {
    color = Opaque(contents->GetThemeColor());
  }
  if (!color) {
    color = Opaque(contents->GetBackgroundColor());
  }
  if (!color) {
    return style;
  }
  style.background = *color;
  style.theme = Theme::Make(*color, RelativeLuminance(*color) < kDarkLuminance,
                            /*tint_surfaces=*/true);
  return style;
}

// static
CompactToolbarStyle CompactToolbarStyle::Lerp(const CompactToolbarStyle& from,
                                              const CompactToolbarStyle& to,
                                              double t) {
  CompactToolbarStyle style;
  style.background = Theme::LerpColor(from.background, to.background, t);
  style.theme = Theme::Lerp(from.theme, to.theme, t);
  return style;
}

}  // namespace starry
