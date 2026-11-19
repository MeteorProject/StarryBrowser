// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/assistant_mini_toolbar.h"

#include <memory>
#include <string>
#include <utility>

#include "base/memory/raw_ref.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view_targeter.h"

namespace starry {

namespace {

// 胶囊高 34，按钮 30 × 28、圆角 14，从左边 4 起一个挨一个。
constexpr int kHeight = 34;
constexpr int kInset = 4;
constexpr int kButtonWidth = 30;
constexpr int kButtonHeight = 28;
constexpr float kButtonRadius = 14;
constexpr int kButtonCount = 4;

}  // namespace

// 一个按钮：只有图标（次要的文字色），悬停时一层浅底淡入。
class AssistantMiniButton : public views::Button {
  METADATA_HEADER(AssistantMiniButton, views::Button)

 public:
  AssistantMiniButton(base::RepeatingClosure callback,
                      const gfx::VectorIcon& icon,
                      int icon_size,
                      const std::u16string& tooltip)
      : views::Button(std::move(callback)), icon_(icon), icon_size_(icon_size) {
    SetTooltipText(tooltip);
    GetViewAccessibility().SetName(tooltip);
    SetAnimationDuration(base::Milliseconds(180));
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  // views::Button:
  void PaintButtonContents(gfx::Canvas* canvas) override {
    const float hover = static_cast<float>(hover_animation().GetCurrentValue());
    if (hover > 0) {
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(theme_.OnSurface(0.08f * hover));
      canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), kButtonRadius, fill);
    }
    canvas->Save();
    canvas->Translate(
        gfx::Vector2d((width() - icon_size_) / 2, (height() - icon_size_) / 2));
    gfx::PaintVectorIcon(canvas, *icon_, icon_size_, theme_.on_surface_variant);
    canvas->Restore();
  }

 private:
  const raw_ref<const gfx::VectorIcon> icon_;
  const int icon_size_;
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(AssistantMiniButton)
END_METADATA

// 玻璃上面一层主题的面板色（同胶囊、岛只有输入框时：46%）和一圈细边；按钮在它上面。
class AssistantMiniToolbarBody : public views::View {
  METADATA_HEADER(AssistantMiniToolbarBody, views::View)

 public:
  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    const float radius = bounds.height() / 2;
    const SkColor base =
        theme_.is_dark ? theme_.surface_panel : theme_.surface_bright;
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(base, 0.46f * 255));
    canvas->DrawRoundRect(bounds, radius, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius - 0.5f, border);
  }

 private:
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(AssistantMiniToolbarBody)
END_METADATA

AssistantMiniToolbar::Callbacks::Callbacks() = default;
AssistantMiniToolbar::Callbacks::Callbacks(Callbacks&&) = default;
AssistantMiniToolbar::Callbacks& AssistantMiniToolbar::Callbacks::operator=(
    Callbacks&&) = default;
AssistantMiniToolbar::Callbacks::~Callbacks() = default;

AssistantMiniToolbar::AssistantMiniToolbar(Callbacks callbacks) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  SetCornerRadius(kHeight / 2.0f, true);
  body_ = AddChildView(std::make_unique<AssistantMiniToolbarBody>());
  const auto add = [this](base::RepeatingClosure callback,
                          const gfx::VectorIcon& icon, int icon_size,
                          const std::u16string& tooltip) {
    buttons_.push_back(
        body_->AddChildView(std::make_unique<AssistantMiniButton>(
            std::move(callback), icon, icon_size, tooltip)));
  };
  // 新对话、聊天记录、换成侧栏、关闭（图标同页面标题行里的那几个）。
  add(std::move(callbacks.new_chat), icons::kSquareAndPencil, 14,
      L("assistant.newChat.help"));
  add(std::move(callbacks.history), icons::kClock, 14,
      L("assistant.history.title"));
  add(std::move(callbacks.dock), icons::kSidebarRight, 14,
      L("assistant.panelStyle.dock"));
  add(std::move(callbacks.close), icons::kXmark, 12, L("assistant.close"));
}

AssistantMiniToolbar::~AssistantMiniToolbar() = default;

// static
gfx::Size AssistantMiniToolbar::GlassSize() {
  return gfx::Size(kButtonCount * kButtonWidth + 2 * kInset, kHeight);
}

void AssistantMiniToolbar::SetTheme(const Theme& theme) {
  GlassStyle style;
  style.dark = theme.is_dark;
  style.luma = theme.is_dark ? 0.1f : 0.9f;
  SetStyle(style);
  body_->SetTheme(theme);
  for (AssistantMiniButton* button : buttons_) {
    button->SetTheme(theme);
  }
}

void AssistantMiniToolbar::SetToolbarBounds(const gfx::Rect& rect) {
  SetGlassBounds(rect);
  body_->SetBoundsRect(glass_rect());
  for (size_t i = 0; i < buttons_.size(); ++i) {
    buttons_[i]->SetBounds(kInset + static_cast<int>(i) * kButtonWidth,
                           (kHeight - kButtonHeight) / 2, kButtonWidth,
                           kButtonHeight);
  }
}

bool AssistantMiniToolbar::DoesIntersectRect(const views::View* target,
                                             const gfx::Rect& rect) const {
  return glass_rect().Intersects(rect);
}

BEGIN_METADATA(AssistantMiniToolbar)
END_METADATA

}  // namespace starry
