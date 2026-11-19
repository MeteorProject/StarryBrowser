// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/toolbar/content_blocking_button.h"

#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/typography.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace starry {

namespace {

constexpr int kGlyph = 11;
constexpr int kHeight = 16;
constexpr int kGap = 3;

const gfx::FontList& CountFont() {
  return UiFont(10, gfx::Font::Weight::SEMIBOLD);
}

}  // namespace

ContentBlockingButton::ContentBlockingButton(PressedCallback callback)
    : views::Button(std::move(callback)) {
  GetViewAccessibility().SetName(L("toolbar.privacy.help"));
}

ContentBlockingButton::~ContentBlockingButton() = default;

void ContentBlockingButton::SetState(int count,
                                     bool allowlisted,
                                     bool fingerprint_protected) {
  if (count == count_ && allowlisted == allowlisted_ &&
      fingerprint_protected == fingerprint_protected_) {
    return;
  }
  const std::u16string old_text = CountText();
  count_ = allowlisted ? 0 : count;
  allowlisted_ = allowlisted;
  fingerprint_protected_ = fingerprint_protected;
  GetViewAccessibility().SetName(GetRenderedTooltipText(gfx::Point()));
  if (CountText() != old_text) {
    PreferredSizeChanged();
  }
  SchedulePaint();
}

void ContentBlockingButton::SetTheme(const Theme& theme) {
  theme_ = theme;
  SchedulePaint();
}

void ContentBlockingButton::SetAlpha(float alpha) {
  if (alpha == alpha_) {
    return;
  }
  alpha_ = alpha;
  SchedulePaint();
}

std::u16string ContentBlockingButton::CountText() const {
  if (count_ <= 0) {
    return std::u16string();
  }
  return count_ < 100 ? base::NumberToString16(count_) : u"99+";
}

gfx::Size ContentBlockingButton::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  int width = kHeight;
  if (const std::u16string text = CountText(); !text.empty()) {
    width = kGlyph + kGap + gfx::GetStringWidth(text, CountFont()) + 2;
  }
  return gfx::Size(width, kHeight);
}

void ContentBlockingButton::PaintButtonContents(gfx::Canvas* canvas) {
  if (alpha_ <= 0) {
    return;
  }
  const SkColor color =
      count_ > 0 ? theme_.accent : theme_.on_surface_variant;
  const SkColor tinted = SkColorSetA(
      color, static_cast<U8CPU>(SkColorGetA(color) * alpha_));
  const std::u16string text = CountText();
  // 没有数字时符号在按钮正中（和书签星号一样），有数字时靠左。
  const int x = text.empty() ? (width() - kGlyph) / 2 : 0;
  canvas->Save();
  canvas->Translate(gfx::Vector2d(x, (height() - kGlyph) / 2));
  gfx::PaintVectorIcon(canvas,
                       allowlisted_ && !fingerprint_protected_
                           ? icons::kShieldSlash
                           : icons::kShieldLefthalfFilled,
                       kGlyph, tinted);
  canvas->Restore();
  if (!text.empty()) {
    canvas->DrawStringRectWithFlags(
        text, CountFont(), tinted,
        gfx::Rect(kGlyph + kGap, 0, width() - kGlyph - kGap, height()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }
}

std::u16string ContentBlockingButton::GetRenderedTooltipText(
    const gfx::Point& p) const {
  if (allowlisted_) {
    return L(fingerprint_protected_ ? "toolbar.privacy.fingerprinting"
                                    : "toolbar.privacy.allowlisted");
  }
  std::u16string text = L("toolbar.privacy.blockedCount", {{"count", count_}});
  if (fingerprint_protected_) {
    text += u" · " + L("toolbar.privacy.fingerprinting");
  }
  return text;
}

BEGIN_METADATA(ContentBlockingButton)
END_METADATA

}  // namespace starry
