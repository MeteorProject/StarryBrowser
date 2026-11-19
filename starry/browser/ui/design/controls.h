// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DESIGN_CONTROLS_H_
#define STARRY_BROWSER_UI_DESIGN_CONTROLS_H_

#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/button/button.h"

namespace gfx {
struct VectorIcon;
}

namespace starry {

// 按钮样式变体。
enum class ButtonVariant { kFilled, kSecondary, kTertiary, kGhost, kDanger };

// 图标按钮右上角的状态角标：kActivity 为智能体渐变（助手运行中），
// kAttention 为警示色（等待用户回应）。
enum class IconBadge { kNone, kActivity, kAttention };

// 图标按钮：悬停时底色淡入，按下时在合成器线程缩放到 0.96。默认 28 × 28、图标 14、ghost。
class IconButton : public views::Button {
  METADATA_HEADER(IconButton, views::Button)

 public:
  IconButton(PressedCallback callback,
             const gfx::VectorIcon& icon,
             const std::u16string& tooltip,
             int size = 28,
             int icon_size = 14);
  IconButton(const IconButton&) = delete;
  IconButton& operator=(const IconButton&) = delete;
  ~IconButton() override;

  void SetIcon(const gfx::VectorIcon& icon);
  void SetTheme(const Theme& theme);
  void SetVariant(ButtonVariant variant);
  // 激活状态：底色常驻，图标使用主题色。
  void SetActive(bool active);
  void SetTint(std::optional<SkColor> tint);
  // 右上角的小点（直径 6，离右边、上边 5）：出现、消失时缩放淡入淡出（Motion.lift）。
  void SetBadge(IconBadge badge);
  IconBadge badge() const { return badge_; }

  // views::Button:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void PaintButtonContents(gfx::Canvas* canvas) override;
  void StateChanged(ButtonState old_state) override;
  void OnEnabledChanged() override;

 protected:
  // 底色上面的图标（下载按钮在这里加进度圈）。
  virtual void PaintIcon(gfx::Canvas* canvas, SkColor color);
  const gfx::VectorIcon& icon() const { return *icon_; }
  int icon_size() const { return icon_size_; }
  const Theme& theme() const { return theme_; }

 private:
  SkColor FillColor(float hover) const;
  SkColor IconColor() const;

  raw_ref<const gfx::VectorIcon> icon_;
  int size_;
  int icon_size_;
  Theme theme_ = Theme::DarkBase();
  ButtonVariant variant_ = ButtonVariant::kGhost;
  bool active_ = false;
  std::optional<SkColor> tint_;
  bool pressed_scale_ = false;
  LayerMotion press_motion_;
  IconBadge badge_ = IconBadge::kNone;
  // 小点正在消失时还用原来的颜色画。
  IconBadge badge_shown_ = IconBadge::kNone;
  // 小点出来了多少（0 到 1，弹簧）。
  ValueMotion badge_presence_;
};

// 胶囊按钮：13 中等字重，高 32（SetHeight 可修改），左右内边距 16，
// 可带 12 的前置图标（间距 8）。对话框里取消用 tertiary、确认用 filled、
// 危险操作用 danger。键盘选中时绘制外圈 2 的强调色边框。
class PillButton : public views::Button {
  METADATA_HEADER(PillButton, views::Button)

 public:
  PillButton(PressedCallback callback,
             const std::u16string& title,
             ButtonVariant variant = ButtonVariant::kSecondary);
  PillButton(const PillButton&) = delete;
  PillButton& operator=(const PillButton&) = delete;
  ~PillButton() override;

  void SetTheme(const Theme& theme);
  void SetTitle(const std::u16string& title);
  void SetVariant(ButtonVariant variant);
  // 文字前置图标，nullptr 表示无图标。
  void SetIcon(const gfx::VectorIcon* icon);
  // 胶囊的高度（不含键盘选中的外圈）。
  void SetHeight(int height);
  // 键盘选中（对话框里 Tab、←→ 移动的那个）。
  void SetKeyboardSelected(bool selected);
  const std::u16string& title() const { return title_; }
  // 外圈占的边距：按钮比胶囊本身大这么多（每边）。
  static int FocusOutset();

  // views::Button:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void PaintButtonContents(gfx::Canvas* canvas) override;
  void StateChanged(ButtonState old_state) override;

 private:
  std::u16string title_;
  ButtonVariant variant_;
  raw_ptr<const gfx::VectorIcon> icon_ = nullptr;
  int height_;
  Theme theme_ = Theme::DarkBase();
  bool keyboard_selected_ = false;
  bool pressed_scale_ = false;
  LayerMotion press_motion_;
};

// 标签里的小按钮（关闭、静音）：只有图标，平时没有底；show_fill 时垫一层浅底（标签悬停时）。
// 透明度由标签控制（关闭按钮悬停时淡入）。
class GlyphButton : public views::Button {
  METADATA_HEADER(GlyphButton, views::Button)

 public:
  GlyphButton(PressedCallback callback,
              const gfx::VectorIcon& icon,
              int glyph_size,
              int button_size);
  GlyphButton(const GlyphButton&) = delete;
  GlyphButton& operator=(const GlyphButton&) = delete;
  ~GlyphButton() override;

  void SetIcon(const gfx::VectorIcon& icon);
  void SetColors(SkColor tint, SkColor fill);
  void SetShowsFill(bool shows_fill);
  void SetFillRadius(float radius);
  void SetAlpha(float alpha);
  float alpha() const { return alpha_; }

  // views::Button:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void PaintButtonContents(gfx::Canvas* canvas) override;

 private:
  raw_ref<const gfx::VectorIcon> icon_;
  int glyph_size_;
  int button_size_;
  SkColor tint_ = SK_ColorGRAY;
  SkColor fill_ = SK_ColorTRANSPARENT;
  bool shows_fill_ = false;
  float fill_radius_ = 5;
  float alpha_ = 1;
};

// 开关按钮：34 × 20 胶囊轨道，开启时高亮强调色并平滑滑动圆钮。点击切换并触发 changed。
class SwitchButton : public views::Button {
  METADATA_HEADER(SwitchButton, views::Button)

 public:
  explicit SwitchButton(base::RepeatingCallback<void(bool on)> changed);
  SwitchButton(const SwitchButton&) = delete;
  SwitchButton& operator=(const SwitchButton&) = delete;
  ~SwitchButton() override;

  bool on() const { return on_; }
  void SetOn(bool on, bool animate);
  void SetTheme(const Theme& theme);

  // views::Button:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void PaintButtonContents(gfx::Canvas* canvas) override;

 private:
  void Toggle();

  base::RepeatingCallback<void(bool)> changed_;
  bool on_ = false;
  Theme theme_ = Theme::DarkBase();
  ValueMotion knob_;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DESIGN_CONTROLS_H_
