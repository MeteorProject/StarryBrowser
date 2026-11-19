// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/task_manager/task_manager_view.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/window_drag.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "starry/browser/ui/task_manager/usage_chart.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/scroll_view.h"

namespace starry {

namespace task_manager_ui {

namespace {

constexpr int kPadding = 16;
constexpr int kTileSpacing = 12;
constexpr int kHeaderBottom = 6;
constexpr int kHeaderTrailing = 20;
// 没有和红绿灯同一行的标题栏时（Windows、Linux），标题和汇总单独一行这么高。
constexpr int kStandaloneHeaderHeight = 36;
constexpr int kOverviewBottom = 12;
constexpr int kColumnHeaderHeight = 32;
constexpr int kListPadding = 4;
constexpr int kRowPaddingX = 12;
constexpr int kRowPaddingY = 3;
constexpr int kRowMainHeight = 40;
constexpr int kSubtaskHeight = 24;
constexpr int kSubtaskInset = 32;
constexpr int kColumnSpacing = 12;
constexpr int kMemoryWidth = 88;
constexpr int kCpuWidth = 60;
constexpr int kGpuWidth = 56;
constexpr int kPidWidth = 64;
constexpr int kIconSize = 20;
constexpr int kSubtaskIconSize = 14;
constexpr int kFooterHeight = 56;
constexpr int kFooterPaddingX = 24;
// 底栏窄于这个宽度时不放选中进程的小曲线，把地方留给名字和按钮。
constexpr int kTrendsMinWidth = 680;
constexpr int kDetailButtonHeight = 28;
constexpr int kChevronSize = 10;
// 占用到这么多时数字用警示色。
constexpr double kBusyPercent = 50;
constexpr float kSortPressScale = 0.97f;
// 打开时的行入场延迟与位移。
constexpr double kRowRevealBase = 0.1;
constexpr float kRowRevealDistance = 4;
// 新插入行从上方该偏移处淡入。
constexpr float kInsertOffset = -4;

constexpr double kCpuFloor = 10;
constexpr double kGpuFloor = 10;
constexpr double kMemoryFloor = 256.0 * 1024 * 1024;
constexpr double kProcessMemoryFloor = 64.0 * 1024 * 1024;

enum MenuCommand {
  kSwitchToTab = 1,
  kManageExtension,
  kCopyPid,
  kEndProcess,
};

SkColor WithOpacity(SkColor color, float opacity) {
  return SkColorSetA(
      color,
      static_cast<U8CPU>(SkColorGetA(color) * std::clamp(opacity, 0.0f, 1.0f)));
}

SkColor Hairline(const Theme& theme) {
  return theme.OnSurface(theme.is_dark ? 0.07f : 0.06f);
}

void PaintIcon(gfx::Canvas* canvas,
               const gfx::VectorIcon& icon,
               int size,
               SkColor color,
               const gfx::Rect& box) {
  canvas->Save();
  canvas->Translate(gfx::Vector2d(box.x() + (box.width() - size) / 2,
                                  box.y() + (box.height() - size) / 2));
  gfx::PaintVectorIcon(canvas, icon, size, color);
  canvas->Restore();
}

// 一项任务的图标：优先绘制网站或扩展图标，否则按 0.62 倍尺寸绘制默认矢量符号。
void PaintTaskIcon(gfx::Canvas* canvas,
                   const TaskEntry& entry,
                   const gfx::Rect& box,
                   const Theme& theme) {
  if (entry.icon.IsImage()) {
    gfx::ImageSkia image = entry.icon.GetImage().AsImageSkia();
    if (entry.themify_icon) {
      image = gfx::ImageSkiaOperations::CreateColorMask(
          image, theme.on_surface_variant);
    }
    canvas->DrawImageInt(image, 0, 0, image.width(), image.height(), box.x(),
                         box.y(), box.width(), box.height(), /*filter=*/true);
    return;
  }
  if (entry.icon.IsVectorIcon() && entry.icon.GetVectorIcon().vector_icon()) {
    PaintIcon(canvas, *entry.icon.GetVectorIcon().vector_icon(),
              box.width() * 4 / 5, theme.on_surface_variant, box);
    return;
  }
  if (entry.symbol) {
    PaintIcon(canvas, *entry.symbol,
              static_cast<int>(std::lround(box.width() * 0.62)),
              theme.on_surface_variant, box);
  }
}

// 名字后面的小标签：10 半粗，左右 4、上下 1.5，14% 浅底。返回宽度。
int SoftTagWidth(const std::u16string& text) {
  return gfx::GetStringWidth(text, UiFont(10, gfx::Font::Weight::SEMIBOLD)) + 8;
}

void PaintSoftTag(gfx::Canvas* canvas,
                  const std::u16string& text,
                  SkColor color,
                  int x,
                  int center_y) {
  const gfx::FontList& font = UiFont(10, gfx::Font::Weight::SEMIBOLD);
  const int width = SoftTagWidth(text);
  const int height = font.GetHeight() + 3;
  const gfx::Rect rect(x, center_y - height / 2, width, height);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(WithOpacity(color, 0.14f));
  canvas->DrawRoundRect(gfx::RectF(rect), 3, flags);
  canvas->DrawStringRectWithFlags(
      text, font, color, gfx::Rect(x + 4, rect.y(), width - 8, height),
      gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
}

// 表格右边的几列：x 是每列的左边（行或表头的坐标），right 是内容的右边。
struct Columns {
  int memory;
  int cpu;
  int gpu;
  int pid;
};

Columns ColumnsFor(int right) {
  Columns columns;
  columns.pid = right - kPidWidth;
  columns.gpu = columns.pid - kColumnSpacing - kGpuWidth;
  columns.cpu = columns.gpu - kColumnSpacing - kCpuWidth;
  columns.memory = columns.cpu - kColumnSpacing - kMemoryWidth;
  return columns;
}

}  // namespace

// 打开时的淡入与上移入场动画（Motion.reveal，可配置延迟）。开启「减弱动态效果」时仅淡入。
class Reveal {
 public:
  explicit Reveal(views::View* view) : view_(view) {}
  Reveal(const Reveal&) = delete;
  Reveal& operator=(const Reveal&) = delete;
  ~Reveal() = default;

  void Run(double delay, float distance) {
    distance_ = motion::ReduceMotion() ? 0 : distance;
    ui::Layer* layer = view_->layer();
    motion_.SetOpacity(layer, 0);
    if (distance_ != 0) {
      LayerPose pose;
      pose.offset = gfx::Vector2dF(0, distance_);
      motion_.SetTransform(layer, pose);
    }
    timer_.Start(FROM_HERE, base::Seconds(std::max(delay, 0.0)),
                 base::BindOnce(&Reveal::Start, base::Unretained(this)));
  }

  LayerMotion& motion() { return motion_; }

 private:
  void Start() {
    const MotionCurve curve = MotionCurve::Of(motion::kReveal);
    ui::Layer* layer = view_->layer();
    motion_.AnimateOpacityFrom(layer, 0, 1, curve);
    if (distance_ != 0) {
      LayerPose from;
      from.offset = gfx::Vector2dF(0, distance_);
      motion_.AnimateTransformFrom(layer, from, LayerPose(), curve);
    }
  }

  const raw_ptr<views::View> view_;
  float distance_ = 0;
  LayerMotion motion_;
  base::OneShotTimer timer_;
};

// 标题行里的一段文字（标题、汇总），自己的图层：打开时各自淡入。
class HeaderText : public views::View {
  METADATA_HEADER(HeaderText, views::View)

 public:
  HeaderText(const gfx::FontList& font, bool trailing)
      : font_(font), trailing_(trailing) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void Set(const std::u16string& text, SkColor color) {
    if (text_ == text && color_ == color) {
      return;
    }
    text_ = text;
    color_ = color;
    SchedulePaint();
  }
  const std::u16string& text() const { return text_; }
  int TextWidth() const { return gfx::GetStringWidth(text_, font_); }

  void OnPaint(gfx::Canvas* canvas) override {
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(text_, font_, width(), gfx::ELIDE_TAIL), font_, color_,
        gfx::Rect(0, (height() - font_.GetHeight()) / 2, width(),
                  font_.GetHeight()),
        (trailing_ ? gfx::Canvas::TEXT_ALIGN_RIGHT
                   : gfx::Canvas::TEXT_ALIGN_LEFT) |
            gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  const gfx::FontList font_;
  const bool trailing_;
  std::u16string text_;
  SkColor color_ = SK_ColorTRANSPARENT;
};

BEGIN_METADATA(HeaderText)
END_METADATA

// 表头排序列按钮：11 半粗标题加向下箭头。激活列高亮并显示箭头，升序时旋转 180°；按下缩到 0.97。
class SortButton : public views::Button {
  METADATA_HEADER(SortButton, views::Button)

 public:
  SortButton(PressedCallback callback, std::u16string title, bool trailing)
      : views::Button(std::move(callback)),
        title_(std::move(title)),
        trailing_(trailing),
        active_(0,
                base::BindRepeating(&SortButton::SchedulePaint,
                                    base::Unretained(this))),
        flipped_(0,
                 base::BindRepeating(&SortButton::SchedulePaint,
                                     base::Unretained(this))) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetTooltipText(L("taskManager.sortBy.help", {{"column", title_}}));
    GetViewAccessibility().SetName(title_);
    SetWindowDraggable(this, false);
  }

  void SetState(bool active, bool ascending, bool animate) {
    const double active_target = active ? 1 : 0;
    const double flipped_target = active && ascending ? 1 : 0;
    if (animate && !motion::ReduceMotion()) {
      if (active_.target() != active_target) {
        active_.AnimateTo(active_target, motion::Hover());
      }
      if (flipped_.target() != flipped_target) {
        flipped_.AnimateTo(flipped_target, motion::Hover());
      }
    } else {
      active_.SetValue(active_target);
      flipped_.SetValue(flipped_target);
      SchedulePaint();
    }
    GetViewAccessibility().SetIsSelected(active);
  }

  const std::u16string& title() const { return title_; }

  int ContentWidth() const {
    return gfx::GetStringWidth(title_, Font()) + 3 + kChevronSize;
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(ContentWidth(), kColumnHeaderHeight);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const float active =
        static_cast<float>(std::clamp(active_.value(), 0.0, 1.0));
    const SkColor color =
        Theme::LerpColor(theme_.on_surface_variant, theme_.on_surface, active);
    const gfx::FontList& font = Font();
    const int text_width = gfx::GetStringWidth(title_, font);
    const int content = text_width + 3 + kChevronSize;
    const int x = trailing_ ? width() - content : 0;
    canvas->DrawStringRectWithFlags(
        title_, font, color,
        gfx::Rect(x, (height() - font.GetHeight()) / 2, text_width + 1,
                  font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    if (active < 0.01f) {
      return;
    }
    const gfx::PointF center(x + text_width + 3 + kChevronSize / 2.0f,
                             height() / 2.0f);
    canvas->Save();
    canvas->Translate(gfx::Vector2d(static_cast<int>(center.x()),
                                    static_cast<int>(center.y())));
    canvas->sk_canvas()->rotate(
        static_cast<float>(std::clamp(flipped_.value(), 0.0, 1.0)) * 180);
    canvas->Translate(gfx::Vector2d(-kChevronSize / 2, -kChevronSize / 2));
    gfx::PaintVectorIcon(canvas, icons::kChevronDown, kChevronSize,
                         WithOpacity(color, active));
    canvas->Restore();
  }

  void StateChanged(ButtonState old_state) override {
    views::Button::StateChanged(old_state);
    const bool pressed = GetState() == STATE_PRESSED;
    if (pressed == pressed_) {
      return;
    }
    pressed_ = pressed;
    LayerPose pose;
    pose.scale = pressed ? kSortPressScale : 1;
    press_motion_.AnimateTransform(
        layer(), pose, motion::Press(),
        gfx::PointF(width() / 2.0f, height() / 2.0f));
  }

 private:
  static const gfx::FontList& Font() {
    return UiFont(11, gfx::Font::Weight::SEMIBOLD);
  }

  const std::u16string title_;
  const bool trailing_;
  Theme theme_ = Theme::DarkBase();
  ValueMotion active_;
  ValueMotion flipped_;
  bool pressed_ = false;
  LayerMotion press_motion_;
};

BEGIN_METADATA(SortButton)
END_METADATA

// 底栏操作胶囊按钮：12 图标、12 中等字重，高 28，左右内边距 11；悬停加深，按下缩到 0.96；
// 禁用时 45% 不透明度。
class DetailButton : public views::Button {
  METADATA_HEADER(DetailButton, views::Button)

 public:
  DetailButton(PressedCallback callback,
               std::u16string title,
               const gfx::VectorIcon& icon,
               bool destructive)
      : views::Button(std::move(callback)),
        title_(std::move(title)),
        icon_(icon),
        destructive_(destructive) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetAnimationDuration(base::Milliseconds(180));
    GetViewAccessibility().SetName(title_);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  LayerMotion& motion() { return motion_; }
  // 正在淡出（底栏换了选中的进程）：排版不算它。
  bool leaving() const { return leaving_; }
  void set_leaving(bool leaving) { leaving_ = leaving; }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(
        11 + 12 + 5 +
            gfx::GetStringWidth(title_, UiFont(12, gfx::Font::Weight::MEDIUM)) +
            11,
        kDetailButtonHeight);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const bool enabled = GetEnabled();
    const float hover =
        enabled ? static_cast<float>(hover_animation().GetCurrentValue()) : 0;
    const float alpha = enabled ? 1 : 0.45f;
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(WithOpacity(theme_.primary, (0.08f + 0.04f * hover) * alpha));
    canvas->DrawRoundRect(bounds, bounds.height() / 2, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(WithOpacity(theme_.primary, 0.06f * alpha));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, stroke.height() / 2, border);

    const SkColor color =
        WithOpacity(destructive_ ? theme_.danger() : theme_.on_surface, alpha);
    PaintIcon(canvas, *icon_, 12, color,
              gfx::Rect(11, (height() - 12) / 2, 12, 12));
    const gfx::FontList& font = UiFont(12, gfx::Font::Weight::MEDIUM);
    canvas->DrawStringRectWithFlags(
        title_, font, color,
        gfx::Rect(11 + 12 + 5, (height() - font.GetHeight()) / 2,
                  std::max(0, width() - 11 - 12 - 5 - 11 + 1),
                  font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void StateChanged(ButtonState old_state) override {
    views::Button::StateChanged(old_state);
    const bool pressed = GetState() == STATE_PRESSED;
    if (pressed == pressed_) {
      return;
    }
    pressed_ = pressed;
    LayerPose pose;
    pose.scale = pressed ? motion::kPressScale : 1;
    press_motion_.AnimateTransform(
        layer(), pose, motion::Press(),
        gfx::PointF(width() / 2.0f, height() / 2.0f));
  }

  void OnEnabledChanged() override {
    views::Button::OnEnabledChanged();
    SchedulePaint();
  }

 private:
  const std::u16string title_;
  const raw_ref<const gfx::VectorIcon> icon_;
  const bool destructive_;
  Theme theme_ = Theme::DarkBase();
  bool pressed_ = false;
  bool leaving_ = false;
  LayerMotion press_motion_;
  // 出现、消失（底栏换了选中的进程）。
  LayerMotion motion_;
};

BEGIN_METADATA(DetailButton)
END_METADATA

// 单个进程行：主任务的图标、标题、标签和说明，右侧为内存、CPU、GPU、进程 ID；同进程内的
// 其他子任务列于下方。使用独立图层支持位置重排 FLIP 动画与淡入。
class ProcessRow : public views::View, public views::ContextMenuController {
  METADATA_HEADER(ProcessRow, views::View)

 public:
  ProcessRow(TaskManagerView* host, base::ProcessId pid)
      : host_(host),
        pid_(pid),
        hover_(0,
               base::BindRepeating(&ProcessRow::SchedulePaint,
                                   base::Unretained(this))) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    set_context_menu_controller(this);
    GetViewAccessibility().SetRole(ax::mojom::Role::kRow);
  }

  base::ProcessId pid() const { return pid_; }
  const TaskProcess& process() const { return process_; }
  LayerMotion& motion() { return motion_; }
  bool leaving() const { return leaving_; }

  static int HeightFor(const TaskProcess& process) {
    return 2 * kRowPaddingY + kRowMainHeight +
           kSubtaskHeight *
               static_cast<int>(std::max<size_t>(process.tasks.size(), 1) - 1);
  }

  void SetProcess(const TaskProcess& process) {
    process_ = process;
    GetViewAccessibility().SetName(process_.title());
    SchedulePaint();
  }

  void SetSelected(bool selected) {
    if (selected_ == selected) {
      return;
    }
    selected_ = selected;
    GetViewAccessibility().SetIsSelected(selected);
    SchedulePaint();
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  // 没了：原地淡出（Motion.listEdit），放完了删掉。
  void Leave(base::OnceClosure done) {
    leaving_ = true;
    SetCanProcessEventsWithinSubtree(false);
    motion_.AnimateOpacity(layer(), 0,
                           motion::ReduceMotion()
                               ? motion::Hover()
                               : MotionCurve::Of(motion::kListEdit),
                           std::move(done));
  }

  // 新的一行：从上面 4 淡入（Motion.listEdit；减弱动态效果时只淡入）。
  void Insert() {
    if (motion::ReduceMotion()) {
      motion_.AnimateOpacityFrom(layer(), 0, 1, motion::Hover());
      return;
    }
    const MotionCurve curve = MotionCurve::Of(motion::kListEdit);
    motion_.AnimateOpacityFrom(layer(), 0, 1, curve);
    LayerPose from;
    from.offset = gfx::Vector2dF(0, kInsertOffset);
    motion_.AnimateTransformFrom(layer(), from, LayerPose(), curve);
  }

  // 打开窗口后的第一批行：逐行淡入并上移 4 DIP。
  void RevealIn(double delay) {
    reveal_ = std::make_unique<Reveal>(this);
    reveal_->Run(delay, kRowRevealDistance);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const float hover =
        static_cast<float>(std::clamp(hover_.value(), 0.0, 1.0));
    if (!selected_ && hover > 0.01f) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(0.04f * hover));
      canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kRow, flags);
    }
    if (process_.tasks.empty()) {
      return;
    }
    const TaskEntry& primary = process_.tasks.front();
    const int top = kRowPaddingY;
    const int right = width() - kRowPaddingX;
    const Columns columns = ColumnsFor(right);
    PaintTaskIcon(
        canvas, primary,
        gfx::Rect(kRowPaddingX, top + (kRowMainHeight - kIconSize) / 2,
                  kIconSize, kIconSize),
        theme_);

    // 名字（13 中等）和标签，下面一行说明（11）。
    const int text_x = kRowPaddingX + kIconSize + kColumnSpacing;
    const int text_right = columns.memory - kColumnSpacing;
    const int text_width = std::max(0, text_right - text_x);
    const gfx::FontList& title_font = UiFont(13, gfx::Font::Weight::MEDIUM);
    const gfx::FontList& detail_font = UiFont(11);
    const int block = title_font.GetHeight() + 1 + detail_font.GetHeight();
    const int title_y = top + (kRowMainHeight - block) / 2;
    std::vector<std::pair<std::u16string, SkColor>> tags;
    if (primary.is_private) {
      tags.emplace_back(L("taskManager.tag.private"),
                        theme_.on_surface_variant);
    }
    if (process_.unresponsive) {
      tags.emplace_back(L("taskManager.tag.unresponsive"), theme_.warning());
    }
    int tags_width = 0;
    for (const auto& tag : tags) {
      tags_width += 6 + SoftTagWidth(tag.first);
    }
    const int title_space = std::max(0, text_width - tags_width);
    const std::u16string title =
        gfx::ElideText(primary.title, title_font, title_space, gfx::ELIDE_TAIL);
    const int title_width =
        std::min(title_space, gfx::GetStringWidth(title, title_font));
    canvas->DrawStringRectWithFlags(
        title, title_font, theme_.on_surface,
        gfx::Rect(text_x, title_y, title_width + 1, title_font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    int tag_x = text_x + title_width;
    for (const auto& [text, color] : tags) {
      tag_x += 6;
      PaintSoftTag(canvas, text, color, tag_x,
                   title_y + title_font.GetHeight() / 2);
      tag_x += SoftTagWidth(text);
    }
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(primary.detail, detail_font, text_width,
                       gfx::ELIDE_TAIL),
        detail_font, theme_.on_surface_variant,
        gfx::Rect(text_x, title_y + title_font.GetHeight() + 1, text_width,
                  detail_font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);

    // 数字（12）：每次刷新直接换上，不做过渡。
    const gfx::FontList& metric_font = UiFont(12);
    const int metric_y = top + (kRowMainHeight - metric_font.GetHeight()) / 2;
    const auto metric = [&](const std::u16string& text, SkColor color, int x,
                            int column_width) {
      canvas->DrawStringRectWithFlags(
          text, metric_font, color,
          gfx::Rect(x, metric_y, column_width, metric_font.GetHeight()),
          gfx::Canvas::TEXT_ALIGN_RIGHT | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    };
    metric(process_.memory ? FormatMemory(static_cast<double>(*process_.memory))
                           : u"—",
           theme_.on_surface, columns.memory, kMemoryWidth);
    metric(FormatOptional(process_.cpu, &FormatPercent),
           process_.cpu.value_or(0) >= kBusyPercent ? theme_.warning()
                                                    : theme_.on_surface,
           columns.cpu, kCpuWidth);
    metric(FormatOptional(process_.gpu, &FormatPercent),
           process_.gpu.value_or(0) >= kBusyPercent ? theme_.warning()
                                                    : theme_.on_surface,
           columns.gpu, kGpuWidth);
    metric(base::NumberToString16(process_.pid), theme_.on_surface_variant,
           columns.pid, kPidWidth);

    // 同一进程里的其他页面：14 的图标、12 的名字、11 的说明。
    const gfx::FontList& sub_title = UiFont(12);
    const int sub_x = kRowPaddingX + kSubtaskInset;
    for (size_t i = 1; i < process_.tasks.size(); ++i) {
      const TaskEntry& task = process_.tasks[i];
      const int y =
          top + kRowMainHeight + kSubtaskHeight * static_cast<int>(i - 1);
      PaintTaskIcon(
          canvas, task,
          gfx::Rect(sub_x, y + (kSubtaskHeight - kSubtaskIconSize) / 2,
                    kSubtaskIconSize, kSubtaskIconSize),
          theme_);
      const int name_x = sub_x + kSubtaskIconSize + 8;
      const int available = std::max(0, right - name_x);
      const std::u16string name = gfx::ElideText(
          task.title, sub_title, available * 3 / 5, gfx::ELIDE_TAIL);
      const int name_width = gfx::GetStringWidth(name, sub_title);
      canvas->DrawStringRectWithFlags(
          name, sub_title, theme_.on_surface,
          gfx::Rect(name_x, y + (kSubtaskHeight - sub_title.GetHeight()) / 2,
                    name_width + 1, sub_title.GetHeight()),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
      const int detail_x = name_x + name_width + 8;
      canvas->DrawStringRectWithFlags(
          gfx::ElideText(task.detail, detail_font,
                         std::max(0, right - detail_x), gfx::ELIDE_TAIL),
          detail_font, theme_.on_surface_variant,
          gfx::Rect(detail_x,
                    y + (kSubtaskHeight - detail_font.GetHeight()) / 2,
                    std::max(0, right - detail_x), detail_font.GetHeight()),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }
  }

  void OnMouseEntered(const ui::MouseEvent& event) override {
    SetHovered(true);
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    SetHovered(false);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    // 右键交给 ShowContextMenuForViewImpl，不改变当前选中行。
    if (!event.IsLeftMouseButton()) {
      return event.IsRightMouseButton();
    }
    host_->FocusTable();
    host_->Select(pid_);
    if (event.GetClickCount() == 2 ||
        (event.flags() & ui::EF_IS_DOUBLE_CLICK)) {
      host_->Open(pid_);
    }
    return true;
  }

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& screen_point,
      ui::mojom::MenuSourceType source_type) override {
    gfx::Point point = screen_point;
    views::View::ConvertPointFromScreen(host_, &point);
    host_->ShowProcessMenu(pid_, point);
  }

 private:
  void SetHovered(bool hovered) {
    const double target = hovered ? 1 : 0;
    if (hover_.target() == target) {
      return;
    }
    hover_.AnimateTo(target, motion::Hover());
  }

  const raw_ptr<TaskManagerView> host_;
  const base::ProcessId pid_;
  TaskProcess process_;
  Theme theme_ = Theme::DarkBase();
  bool selected_ = false;
  bool leaving_ = false;
  ValueMotion hover_;
  LayerMotion motion_;
  std::unique_ptr<Reveal> reveal_;
};

BEGIN_METADATA(ProcessRow)
END_METADATA

// 选中行高亮背景层：位于行下方，切换选中行时平滑滑动并调整高度。
class SelectionFill : public views::View {
  METADATA_HEADER(SelectionFill, views::View)

 public:
  SelectionFill() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetTheme(const Theme& theme) {
    color_ = theme.OnSurface(theme.is_dark ? 0.1f : 0.07f);
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(color_);
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kRow, flags);
  }

 private:
  SkColor color_ = SK_ColorTRANSPARENT;
};

BEGIN_METADATA(SelectionFill)
END_METADATA

// 表格里滚动的那一块：选中的底色和每一行。
class ProcessList : public views::View {
  METADATA_HEADER(ProcessList, views::View)

 public:
  explicit ProcessList(TaskManagerView* host)
      : host_(host),
        selection_height_(
            0,
            base::BindRepeating(&ProcessList::ApplySelectionHeight,
                                base::Unretained(this))) {
    selection_ = AddChildView(std::make_unique<SelectionFill>());
  }

  // 按模型更新行：animate 时换位置的滑过去、新的淡入、没了的淡出（Motion.listEdit）。
  // 打开窗口后的第一批行逐行出现，reveal_delay 是第一行晚多久。
  void Sync(const TaskManagerModel& model,
            bool animate,
            std::optional<double> reveal_delay) {
    std::map<base::ProcessId, ProcessRow*> existing;
    for (ProcessRow* row : rows_) {
      existing[row->pid()] = row;
    }
    std::vector<raw_ptr<ProcessRow, VectorExperimental>> next;
    std::vector<ProcessRow*> added;
    for (const TaskProcess& process : model.processes()) {
      ProcessRow* row = nullptr;
      if (auto it = existing.find(process.pid); it != existing.end()) {
        row = it->second;
        existing.erase(it);
      } else {
        row = AddChildView(std::make_unique<ProcessRow>(host_, process.pid));
        row->SetTheme(host_->theme());
        added.push_back(row);
      }
      row->SetProcess(process);
      row->SetSelected(model.selection() == process.pid);
      next.push_back(row);
    }
    for (auto& [pid, row] : existing) {
      if (animate) {
        row->Leave(base::BindOnce(&ProcessList::RemoveRow,
                                  weak_factory_.GetWeakPtr(),
                                  base::Unretained(row)));
      } else {
        RemoveChildViewT(row);
      }
    }
    rows_ = std::move(next);
    Place(animate, added);
    for (size_t i = 0; i < rows_.size() && reveal_delay; ++i) {
      if (std::find(added.begin(), added.end(), rows_[i].get()) !=
          added.end()) {
        rows_[i]->RevealIn(*reveal_delay +
                           std::min<size_t>(i, motion::kStaggerLimit) *
                               motion::kStaggerStep);
      }
    }
    if (animate && !reveal_delay) {
      for (ProcessRow* row : added) {
        row->Insert();
      }
    }
    UpdateSelection(model, /*selection_changed=*/false, animate);
  }

  // 选中的行变化或随列表重排、高度变化时，高亮底色平滑过渡位置和高度。
  // 位置采用合成器图层位移，高度采用主线程数值动画（保持顶边对齐且圆角不变形），共用同一曲线。
  void UpdateSelection(const TaskManagerModel& model,
                       bool selection_changed,
                       bool animate) {
    ProcessRow* selected = nullptr;
    for (ProcessRow* row : rows_) {
      const bool is_selected = model.selection() == row->pid();
      row->SetSelected(is_selected);
      if (is_selected) {
        selected = row;
      }
    }
    ui::Layer* layer = selection_->layer();
    if (!selected) {
      if (shown_) {
        selection_motion_.AnimateOpacity(layer, 0, motion::Hover());
      }
      shown_ = false;
      selected_pid_.reset();
      return;
    }
    const gfx::Rect target = selected->bounds();
    const bool same_row = selected_pid_ == selected->pid();
    if (!shown_ || !animate || motion::ReduceMotion()) {
      selection_->SetBoundsRect(target);
      selection_motion_.SetTransform(layer, LayerPose());
      selection_height_.SetValue(target.height());
      if (!shown_) {
        selection_motion_.AnimateOpacity(layer, 1, motion::Hover());
      }
    } else {
      // 换了行：Motion.selection；同一行跟着挪、变高：用行的曲线（Motion.listEdit）。
      const MotionCurve curve = same_row && !selection_changed
                                    ? MotionCurve::Of(motion::kListEdit)
                                    : MotionCurve::Of(motion::kSelection);
      const gfx::Rect old = selection_->bounds();
      // 高度先留着现在的（可能正在过渡），由 selection_height_ 动过去。
      selection_->SetBounds(target.x(), target.y(), target.width(),
                            old.height());
      if (old.origin() != target.origin()) {
        selection_motion_.ShiftAndAnimate(layer, old.origin() - target.origin(),
                                          LayerPose(), curve);
      }
      if (selection_height_.target() != target.height()) {
        selection_height_.AnimateTo(target.height(), curve);
      }
      if (layer->GetTargetOpacity() < 1) {
        selection_motion_.AnimateOpacity(layer, 1, motion::Hover());
      }
    }
    shown_ = true;
    selected_pid_ = selected->pid();
  }

  ProcessRow* RowFor(base::ProcessId pid) const {
    for (ProcessRow* row : rows_) {
      if (row->pid() == pid) {
        return row;
      }
    }
    return nullptr;
  }

  const std::vector<raw_ptr<ProcessRow, VectorExperimental>>& rows() const {
    return rows_;
  }

  void SetTheme(const Theme& theme) {
    selection_->SetTheme(theme);
    for (views::View* child : children()) {
      if (auto* row = views::AsViewClass<ProcessRow>(child)) {
        row->SetTheme(theme);
      }
    }
  }

  void SetListWidth(int width) {
    if (width == list_width_) {
      return;
    }
    list_width_ = width;
    Place(/*animate=*/false, {});
  }

  int ContentHeight() const {
    int height = 2 * kListPadding;
    for (ProcessRow* row : rows_) {
      height += ProcessRow::HeightFor(row->process());
    }
    return height;
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(list_width_, ContentHeight());
  }

  void Layout(PassKey) override {
    if (placing_) {
      return;
    }
    Place(/*animate=*/false, {});
    // 宽度变了：选中的底色跟着行变宽（不动变换和高度，正在过渡的接着过渡）。
    if (shown_ && selected_pid_) {
      if (ProcessRow* row = RowFor(*selected_pid_)) {
        const gfx::Rect bounds = row->bounds();
        selection_->SetBounds(bounds.x(), bounds.y(), bounds.width(),
                              selection_height_.is_animating()
                                  ? selection_->height()
                                  : bounds.height());
      }
    }
  }

 private:
  void Place(bool animate, const std::vector<ProcessRow*>& added) {
    const int content_height = ContentHeight();
    const gfx::Size size(list_width_, content_height);
    if (this->size() != size) {
      placing_ = true;
      SetSize(size);
      placing_ = false;
      PreferredSizeChanged();
    }
    const bool slide = animate && !motion::ReduceMotion();
    int y = kListPadding;
    for (ProcessRow* row : rows_) {
      const gfx::Rect old = row->bounds();
      const gfx::Rect target(kListPadding, y,
                             std::max(0, list_width_ - 2 * kListPadding),
                             ProcessRow::HeightFor(row->process()));
      row->SetBoundsRect(target);
      const bool is_new =
          std::find(added.begin(), added.end(), row) != added.end();
      if (!is_new && old.origin() != target.origin()) {
        if (slide && !old.IsEmpty()) {
          row->motion().ShiftAndAnimate(
              row->layer(), old.origin() - target.origin(), LayerPose(),
              MotionCurve::Of(motion::kListEdit));
        } else if (!animate) {
          row->motion().SetTransform(row->layer(), LayerPose());
        }
      }
      y += target.height();
    }
  }

  // 高度动画的每一帧：顶边不动，只改高度（弹簧略微冲过头时也不小于 0）。
  void ApplySelectionHeight() {
    const int height =
        std::max(0, static_cast<int>(std::lround(selection_height_.value())));
    selection_->SetSize(gfx::Size(selection_->width(), height));
  }

  void RemoveRow(ProcessRow* row) {
    // 只比较指针，确认它还是这里的子视图再删。
    for (views::View* child : children()) {
      if (child == row) {
        RemoveChildViewT(row);
        return;
      }
    }
  }

  const raw_ptr<TaskManagerView> host_;
  raw_ptr<SelectionFill> selection_ = nullptr;
  std::vector<raw_ptr<ProcessRow, VectorExperimental>> rows_;
  LayerMotion selection_motion_;
  // 选中底色的高度（主线程上过渡）。
  ValueMotion selection_height_;
  bool shown_ = false;
  std::optional<base::ProcessId> selected_pid_;
  int list_width_ = 0;
  bool placing_ = false;
  base::WeakPtrFactory<ProcessList> weak_factory_{this};
};

BEGIN_METADATA(ProcessList)
END_METADATA

// 表格卡片的细描边：覆盖在滚动列表上方。
class CardBorder : public views::View {
  METADATA_HEADER(CardBorder, views::View)

 public:
  CardBorder() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetTheme(const Theme& theme) {
    color_ = Hairline(theme);
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1);
    flags.setColor(color_);
    gfx::RectF rect(GetLocalBounds());
    rect.Inset(0.5f);
    canvas->DrawRoundRect(rect, radius::kCard - 0.5f, flags);
  }

 private:
  SkColor color_ = SK_ColorTRANSPARENT;
};

BEGIN_METADATA(CardBorder)
END_METADATA

// 进程表格卡片：面板色圆角卡片，上方为表头与分隔线，下方为可滚动列表。
// 键盘支持 ↑↓ 选择、回车打开、删除键结束进程。
class TableCard : public views::View {
  METADATA_HEADER(TableCard, views::View)

 public:
  explicit TableCard(TaskManagerView* host) : host_(host) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(radius::kCard));
    layer()->SetIsFastRoundedCorner(true);
    layer()->SetMasksToBounds(true);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    GetViewAccessibility().SetRole(ax::mojom::Role::kTable);
    GetViewAccessibility().SetName(L("taskManager.title"));
    SetWindowDraggable(this, false);

    const auto add_sort = [&](TaskManagerModel::Column column,
                              const std::u16string& title, bool trailing) {
      SortButton* button = AddChildView(std::make_unique<SortButton>(
          base::BindRepeating(
              [](TaskManagerView* host, TaskManagerModel::Column column) {
                host->SortForTesting(column);
              },
              base::Unretained(host), column),
          title, trailing));
      sort_buttons_[static_cast<size_t>(column)] = button;
    };
    add_sort(TaskManagerModel::Column::kTask, L("taskManager.column.task"),
             false);
    add_sort(TaskManagerModel::Column::kMemory, L("taskManager.memory"), true);
    add_sort(TaskManagerModel::Column::kCpu, u"CPU", true);
    add_sort(TaskManagerModel::Column::kGpu, u"GPU", true);
    add_sort(TaskManagerModel::Column::kPid, L("taskManager.column.pid"), true);

    scroll_ = AddChildView(std::make_unique<views::ScrollView>(
        views::ScrollView::ScrollWithLayers::kEnabled));
    scroll_->SetBackgroundColor(std::nullopt);
    scroll_->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    scroll_->SetDrawOverflowIndicator(false);
    list_ = scroll_->SetContents(std::make_unique<ProcessList>(host));
    border_ = AddChildView(std::make_unique<CardBorder>());
  }

  ProcessList* list() { return list_; }
  views::ScrollView* scroll() { return scroll_; }
  SortButton* sort_button(TaskManagerModel::Column column) {
    return sort_buttons_[static_cast<size_t>(column)];
  }

  void SetSort(TaskManagerModel::Column column, bool ascending, bool animate) {
    for (size_t i = 0; i < sort_buttons_.size(); ++i) {
      sort_buttons_[i]->SetState(static_cast<size_t>(column) == i, ascending,
                                 animate);
    }
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    for (SortButton* button : sort_buttons_) {
      button->SetTheme(theme);
    }
    list_->SetTheme(theme);
    border_->SetTheme(theme);
    SchedulePaint();
  }

  void Layout(PassKey) override {
    scroll_->SetBounds(0, kColumnHeaderHeight + 1, width(),
                       std::max(0, height() - kColumnHeaderHeight - 1));
    const int list_width = scroll_->GetVisibleRect().width() > 0
                               ? scroll_->GetVisibleRect().width()
                               : width();
    list_->SetListWidth(list_width);
    // 表头和行的列对齐：行在列表里左右各让 4，内容再让 12。
    const int right = list_width - kListPadding - kRowPaddingX;
    const Columns columns = ColumnsFor(right);
    const auto place = [&](TaskManagerModel::Column column, int x,
                           int column_width, bool trailing) {
      SortButton* button = sort_button(column);
      const int content = std::min(column_width, button->ContentWidth());
      button->SetBounds(trailing ? x + column_width - content : x, 0, content,
                        kColumnHeaderHeight);
    };
    const int task_x = kListPadding + kRowPaddingX;
    place(TaskManagerModel::Column::kTask, task_x,
          std::max(0, columns.memory - kColumnSpacing - task_x), false);
    place(TaskManagerModel::Column::kMemory, columns.memory, kMemoryWidth,
          true);
    place(TaskManagerModel::Column::kCpu, columns.cpu, kCpuWidth, true);
    place(TaskManagerModel::Column::kGpu, columns.gpu, kGpuWidth, true);
    place(TaskManagerModel::Column::kPid, columns.pid, kPidWidth, true);
    border_->SetBoundsRect(GetLocalBounds());
  }

  void OnPaint(gfx::Canvas* canvas) override {
    canvas->DrawColor(theme_.surface_panel);
    canvas->FillRect(gfx::Rect(0, kColumnHeaderHeight, width(), 1),
                     Hairline(theme_));
  }

  bool OnKeyPressed(const ui::KeyEvent& event) override {
    return host_->HandleKey(event);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    RequestFocus();
    return false;
  }

 private:
  const raw_ptr<TaskManagerView> host_;
  Theme theme_ = Theme::DarkBase();
  std::array<raw_ptr<SortButton>, 5> sort_buttons_{};
  raw_ptr<views::ScrollView> scroll_ = nullptr;
  raw_ptr<ProcessList> list_ = nullptr;
  raw_ptr<CardBorder> border_ = nullptr;
};

BEGIN_METADATA(TableCard)
END_METADATA

// 底栏：显示选中进程名称与 PID（未选中时显示提示），宽度充足时附加进程迷你趋势图，
// 右侧提供「切换到标签页」/「管理扩展」和「结束进程」按钮，切换选中行时平滑淡入淡出。
class Footer : public views::View {
  METADATA_HEADER(Footer, views::View)

 public:
  explicit Footer(TaskManagerView* host) : host_(host) {
    const auto open = base::BindRepeating(
        [](TaskManagerView* host) {
          if (const TaskProcess* process = host->model().selected_process()) {
            host->Open(process->pid);
          }
        },
        base::Unretained(host));
    switch_tab_ = AddChildView(std::make_unique<DetailButton>(
        open, L("taskManager.switchToTab"), icons::kArrowUpRight, false));
    manage_extension_ = AddChildView(std::make_unique<DetailButton>(
        open, L("taskManager.manageExtension"), icons::kPuzzlepiece, false));
    end_ = AddChildView(std::make_unique<DetailButton>(
        base::BindRepeating(
            [](TaskManagerView* host) {
              if (const TaskProcess* process =
                      host->model().selected_process()) {
                host->RequestEnd(process->pid);
              }
            },
            base::Unretained(host)),
        L("taskManager.endProcess"), icons::kXmarkOctagon, true));
    trends_ = AddChildView(std::make_unique<views::View>());
    trends_->SetPaintToLayer();
    trends_->layer()->SetFillsBoundsOpaquely(false);
    cpu_trend_ = trends_->AddChildView(
        std::make_unique<MiniTrend>(u"CPU", kCpuFloor, &FormatPercent));
    memory_trend_ = trends_->AddChildView(std::make_unique<MiniTrend>(
        L("taskManager.memory"), kProcessMemoryFloor, &FormatMemory));
    switch_tab_->SetVisible(false);
    manage_extension_->SetVisible(false);
    trends_->SetVisible(false);
  }

  const std::u16string& text() const { return text_; }

  void Update(const TaskManagerModel& model, bool animate) {
    const TaskProcess* process = model.selected_process();
    text_ = process ? L("taskManager.footer.selected",
                        {{"name", process->title()},
                         {"pid", base::NumberToString16(process->pid)}})
                    : L("taskManager.footer.hint");
    const TaskEntry::Source source = process && !process->tasks.empty()
                                         ? process->tasks.front().source
                                         : TaskEntry::Source::kOther;
    const ProcessHistory* history =
        process ? model.HistoryFor(process->pid) : nullptr;
    trends_wanted_ = history != nullptr;
    if (history) {
      cpu_trend_->Update(history->cpu);
      memory_trend_->Update(history->memory);
    }
    end_->SetEnabled(process && process->can_end);
    end_->SetTooltipText(process && process->kind == TaskProcess::Kind::kBrowser
                             ? L("taskManager.end.browser.help")
                             : L("taskManager.end.selected.help"));
    animate_ = animate;
    Show(switch_tab_, source == TaskEntry::Source::kTab);
    Show(manage_extension_, source == TaskEntry::Source::kExtension);
    Place();
    SchedulePaint();
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    for (DetailButton* button :
         {switch_tab_.get(), manage_extension_.get(), end_.get()}) {
      button->SetTheme(theme);
    }
    cpu_trend_->SetTheme(theme);
    memory_trend_->SetTheme(theme);
    SchedulePaint();
  }

  void Layout(PassKey) override {
    animate_ = false;
    Place();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::FontList& font = UiFont(12);
    const int text_width = std::max(0, text_right_ - kFooterPaddingX);
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(text_, font, text_width, gfx::ELIDE_MIDDLE), font,
        theme_.on_surface_variant,
        gfx::Rect(kFooterPaddingX, (height() - font.GetHeight()) / 2,
                  text_width, font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  // 按钮出现（淡入）、消失（原地淡出后藏起来，排版马上不算它）。
  void Show(DetailButton* button, bool show) {
    const bool shown = button->GetVisible() && !button->leaving();
    if (show == shown) {
      return;
    }
    LayerMotion& motion = button->motion();
    ui::Layer* layer = button->layer();
    if (show) {
      const float from = button->leaving() ? motion.CurrentOpacity(layer) : 0;
      button->set_leaving(false);
      button->SetVisible(true);
      if (animate_) {
        motion.AnimateOpacityFrom(layer, from, 1, motion::Hover());
      } else {
        motion.SetOpacity(layer, 1);
      }
      return;
    }
    if (!animate_) {
      button->SetVisible(false);
      return;
    }
    button->set_leaving(true);
    motion.AnimateOpacity(
        layer, 0, motion::Hover(),
        base::BindOnce(
            [](base::WeakPtr<Footer> footer, DetailButton* button) {
              if (footer && button->leaving()) {
                button->set_leaving(false);
                button->SetVisible(false);
              }
            },
            weak_factory_.GetWeakPtr(), base::Unretained(button)));
  }

  void Place() {
    int right = width() - kFooterPaddingX;
    const auto place_button = [&](DetailButton* button) {
      const gfx::Size size = button->GetPreferredSize();
      const gfx::Rect target(right - size.width(),
                             (height() - size.height()) / 2, size.width(),
                             size.height());
      button->SetBoundsRect(target);
      right = target.x() - 8;
    };
    place_button(end_);
    for (DetailButton* button : {switch_tab_.get(), manage_extension_.get()}) {
      if (button->GetVisible() && !button->leaving()) {
        place_button(button);
      }
    }
    // 选中进程的小曲线：底栏够宽、有曲线时才放，和按钮隔 6 + 8。
    const bool show_trends = trends_wanted_ && width() >= kTrendsMinWidth;
    if (show_trends) {
      const gfx::Size cpu = cpu_trend_->GetPreferredSize();
      const gfx::Size memory = memory_trend_->GetPreferredSize();
      const int trends_width = cpu.width() + 14 + memory.width();
      const int trends_height = std::max(cpu.height(), memory.height());
      const gfx::Rect target(right - 6 - trends_width,
                             (height() - trends_height) / 2, trends_width,
                             trends_height);
      const gfx::Rect old = trends_->bounds();
      const bool was_visible = trends_->GetVisible() && trends_shown_;
      trends_->SetBoundsRect(target);
      cpu_trend_->SetBounds(0, (trends_height - cpu.height()) / 2, cpu.width(),
                            cpu.height());
      memory_trend_->SetBounds(cpu.width() + 14,
                               (trends_height - memory.height()) / 2,
                               memory.width(), memory.height());
      if (!was_visible) {
        trends_->SetVisible(true);
        if (animate_) {
          trends_motion_.SetTransform(trends_->layer(), LayerPose());
          trends_motion_.AnimateOpacityFrom(trends_->layer(), 0, 1,
                                            motion::Hover());
        } else {
          trends_motion_.SetOpacity(trends_->layer(), 1);
        }
      } else if (animate_ && old.origin() != target.origin() &&
                 !motion::ReduceMotion()) {
        // 按钮少了一个、多了一个：曲线跟着挪过去。
        trends_motion_.ShiftAndAnimate(trends_->layer(),
                                       old.origin() - target.origin(),
                                       LayerPose(), motion::Hover());
      }
      trends_shown_ = true;
      right = target.x() - 12;
    } else {
      if (trends_shown_ && animate_) {
        trends_motion_.AnimateOpacity(
            trends_->layer(), 0, motion::Hover(),
            base::BindOnce(
                [](base::WeakPtr<Footer> footer) {
                  if (footer && !footer->trends_shown_) {
                    footer->trends_->SetVisible(false);
                  }
                },
                weak_factory_.GetWeakPtr()));
      } else if (!trends_shown_ || !animate_) {
        trends_->SetVisible(false);
      }
      trends_shown_ = false;
      right -= 4;
    }
    text_right_ = right - 8;
  }

  const raw_ptr<TaskManagerView> host_;
  Theme theme_ = Theme::DarkBase();
  std::u16string text_;
  raw_ptr<DetailButton> switch_tab_ = nullptr;
  raw_ptr<DetailButton> manage_extension_ = nullptr;
  raw_ptr<DetailButton> end_ = nullptr;
  raw_ptr<views::View> trends_ = nullptr;
  raw_ptr<MiniTrend> cpu_trend_ = nullptr;
  raw_ptr<MiniTrend> memory_trend_ = nullptr;
  LayerMotion trends_motion_;
  bool trends_wanted_ = false;
  bool trends_shown_ = false;
  bool animate_ = false;
  int text_right_ = 0;
  base::WeakPtrFactory<Footer> weak_factory_{this};
};

BEGIN_METADATA(Footer)
END_METADATA

// 右键菜单的命令交给窗口，晚一步执行：结束进程会拿掉菜单下面的那一行。
class ProcessMenuDelegate : public ui::SimpleMenuModel::Delegate {
 public:
  explicit ProcessMenuDelegate(base::RepeatingCallback<void(int)> execute)
      : execute_(std::move(execute)) {}

  void ExecuteCommand(int command_id, int event_flags) override {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(execute_, command_id));
  }

 private:
  base::RepeatingCallback<void(int)> execute_;
};

}  // namespace task_manager_ui

using task_manager_ui::DetailButton;
using task_manager_ui::Footer;
using task_manager_ui::HeaderText;
using task_manager_ui::ProcessList;
using task_manager_ui::ProcessRow;
using task_manager_ui::Reveal;
using task_manager_ui::TableCard;

TaskManagerView::TaskManagerView() {
  header_ = AddChildView(std::make_unique<views::View>());
  SetWindowDraggable(header_, true);
  title_ = header_->AddChildView(std::make_unique<HeaderText>(
      UiFont(13, gfx::Font::Weight::SEMIBOLD), /*trailing=*/false));
  summary_ = header_->AddChildView(
      std::make_unique<HeaderText>(UiFont(12), /*trailing=*/true));

  overview_ = AddChildView(std::make_unique<views::View>());
  overview_->SetPaintToLayer();
  overview_->layer()->SetFillsBoundsOpaquely(false);
  tiles_[0] = overview_->AddChildView(std::make_unique<UsageTile>(
      u"CPU", task_manager_ui::kCpuFloor, &FormatPercent));
  tiles_[1] = overview_->AddChildView(std::make_unique<UsageTile>(
      L("taskManager.memory"), task_manager_ui::kMemoryFloor, &FormatMemory));
  tiles_[2] = overview_->AddChildView(std::make_unique<UsageTile>(
      u"GPU", task_manager_ui::kGpuFloor, &FormatPercent));

  table_ = AddChildView(std::make_unique<TableCard>(this));
  footer_ = AddChildView(std::make_unique<Footer>(this));
  dialog_host_ = AddChildView(std::make_unique<DialogHostView>());
  menu_host_ = AddChildView(std::make_unique<MenuHostView>());

  reveals_[0] = std::make_unique<Reveal>(title_);
  reveals_[1] = std::make_unique<Reveal>(summary_);
  reveals_[2] = std::make_unique<Reveal>(overview_);
  reveals_[3] = std::make_unique<Reveal>(table_);

  model_ = std::make_unique<TaskManagerModel>(base::BindRepeating(
      &TaskManagerView::OnModelChanged, base::Unretained(this)));
  UpdateHeader();
  UpdateTiles();
  table_->SetSort(model_->sort_column(), model_->ascending(),
                  /*animate=*/false);
  footer_->Update(*model_, /*animate=*/false);
}

TaskManagerView::~TaskManagerView() {
  // 菜单、对话框先拆：它们的回调会找模型。
  if (menu_host_) {
    menu_host_->Close();
  }
  model_.reset();
}

void TaskManagerView::SetTitlebar(int height, int inset) {
  titlebar_height_ = height;
  title_inset_ = inset;
  InvalidateLayout();
}

views::View* TaskManagerView::focus_target() {
  return table_;
}

void TaskManagerView::AddedToWidget() {
  views::View::AddedToWidget();
  if (revealed_) {
    return;
  }
  revealed_ = true;
  appeared_at_ = base::TimeTicks::Now();
  // 标题与汇总仅淡入，卡片和表格同时上移 10 DIP。
  reveals_[0]->Run(0.02, 0);
  reveals_[1]->Run(0.06, 0);
  reveals_[2]->Run(0.07, 10);
  reveals_[3]->Run(0.08, 10);
}

gfx::Size TaskManagerView::GetMinimumSize() const {
  return gfx::Size(560, 440);
}

void TaskManagerView::Layout(PassKey) {
  const int header_height = titlebar_height_ > 0
                                ? titlebar_height_
                                : task_manager_ui::kStandaloneHeaderHeight;
  const int inset = titlebar_height_ > 0 ? title_inset_ : 20;
  header_->SetBounds(0, 0, width(), header_height);
  const int title_width = title_->TextWidth() + 1;
  title_->SetBounds(inset, 0,
                    std::min(title_width, std::max(0, width() - inset)),
                    header_height);
  const int summary_x = inset + title_width + 12;
  summary_->SetBounds(
      summary_x, 0,
      std::max(0, width() - task_manager_ui::kHeaderTrailing - summary_x),
      header_height);

  int y = header_height + task_manager_ui::kHeaderBottom;
  const int content_width =
      std::max(0, width() - 2 * task_manager_ui::kPadding);
  const int tile_height = UsageTile::PreferredHeight();
  overview_->SetBounds(task_manager_ui::kPadding, y, content_width,
                       tile_height);
  const int tile_width =
      (content_width - 2 * task_manager_ui::kTileSpacing) / 3;
  for (size_t i = 0; i < tiles_.size(); ++i) {
    const int x =
        static_cast<int>(i) * (tile_width + task_manager_ui::kTileSpacing);
    tiles_[i]->SetBounds(
        x, 0, i + 1 == tiles_.size() ? content_width - x : tile_width,
        tile_height);
  }
  y += tile_height + task_manager_ui::kOverviewBottom;
  const int footer_y = height() - task_manager_ui::kFooterHeight;
  table_->SetBounds(task_manager_ui::kPadding, y, content_width,
                    std::max(0, footer_y - y));
  footer_->SetBounds(0, footer_y, width(), task_manager_ui::kFooterHeight);
  dialog_host_->SetBoundsRect(GetLocalBounds());
  menu_host_->SetBoundsRect(GetLocalBounds());
}

void TaskManagerView::OnPaint(gfx::Canvas* canvas) {
  canvas->DrawColor(theme_.surface);
}

void TaskManagerView::OnThemeChanged() {
  views::View::OnThemeChanged();
  const ui::ColorProvider* provider = GetColorProvider();
  if (!provider) {
    return;
  }
  // 深浅跟着窗口实际用的配色（系统外观、Chrome 的主题设置），同浏览器窗口。
  const bool dark =
      color_utils::IsDark(provider->GetColor(ui::kColorWindowBackground));
  const Theme theme = Theme::Make(std::nullopt, dark, /*tint_surfaces=*/false);
  if (theme_applied_ && theme == theme_) {
    return;
  }
  theme_ = theme;
  theme_applied_ = true;
  ApplyTheme();
}

void TaskManagerView::ApplyTheme() {
  UpdateHeader();
  for (UsageTile* tile : tiles_) {
    tile->SetTheme(theme_);
  }
  table_->SetTheme(theme_);
  footer_->SetTheme(theme_);
  menu_host_->SetTheme(theme_);
  dialog_host_->SetTheme(theme_);
  SchedulePaint();
}

void TaskManagerView::OnModelChanged() {
  UpdateHeader();
  UpdateTiles();
  // 打开后的第一批行逐行出现：第一行在窗口出现 0.1 秒后开始。
  std::optional<double> reveal_delay;
  const bool first = !rows_synced_;
  if (first && !model_->processes().empty()) {
    rows_synced_ = true;
    const double elapsed = (base::TimeTicks::Now() - appeared_at_).InSecondsF();
    reveal_delay = std::max(0.0, task_manager_ui::kRowRevealBase - elapsed);
  }
  table_->list()->Sync(*model_, /*animate=*/!first, reveal_delay);
  table_->SetSort(model_->sort_column(), model_->ascending(), /*animate=*/true);
  footer_->Update(*model_, /*animate=*/true);
}

void TaskManagerView::FocusTable() {
  table_->RequestFocus();
}

void TaskManagerView::UpdateHeader() {
  title_->Set(L("taskManager.title"), theme_.on_surface);
  summary_->Set(
      model_ && model_->loaded()
          ? L("taskManager.summary", {{"count", model_->processes().size()}})
          : L("taskManager.summary.loading"),
      theme_.on_surface_variant);
}

void TaskManagerView::UpdateTiles() {
  if (!model_) {
    return;
  }
  tiles_[0]->Update(model_->cpu_history(), std::nullopt, std::nullopt);
  tiles_[1]->Update(model_->memory_history(), std::nullopt, std::nullopt);
  std::optional<std::u16string> context;
  if (const std::optional<double> device = model_->device_gpu()) {
    context = L("taskManager.gpu.device", {{"value", FormatPercent(*device)}});
  }
  std::optional<std::u16string> unavailable;
  if (model_->gpu_available() == false) {
    unavailable = L("taskManager.gpu.unavailable");
  }
  tiles_[2]->Update(model_->gpu_history(), std::move(context),
                    std::move(unavailable));
}

void TaskManagerView::Select(std::optional<base::ProcessId> pid) {
  if (model_->selection() == pid) {
    return;
  }
  model_->Select(pid);
  table_->list()->UpdateSelection(*model_, /*selection_changed=*/true,
                                  /*animate=*/true);
  footer_->Update(*model_, /*animate=*/true);
  if (pid) {
    if (ProcessRow* row = table_->list()->RowFor(*pid)) {
      row->ScrollViewToVisible();
    }
  }
}

void TaskManagerView::Open(base::ProcessId pid) {
  model_->Open(pid);
}

void TaskManagerView::RequestEnd(base::ProcessId pid) {
  const TaskProcess* process = model_->Find(pid);
  if (!process || !process->can_end) {
    return;
  }
  const std::optional<TaskManagerModel::Confirmation> confirmation =
      model_->ConfirmationFor(*process);
  if (!confirmation) {
    model_->End(pid);
    return;
  }
  DialogSpec spec;
  spec.tone = DialogSpec::Tone::kDanger;
  spec.icon = &icons::kXmarkOctagonFill;
  spec.title = confirmation->title;
  spec.message = confirmation->message;
  spec.buttons.push_back(
      {L("common.cancel"), ButtonVariant::kTertiary, /*cancel=*/true});
  spec.buttons.push_back(
      {L("taskManager.endProcess"), ButtonVariant::kDanger, /*cancel=*/false});
  dialog_host_->Present(MakeDialogRequest(
      std::move(spec),
      base::BindOnce(
          [](base::WeakPtr<TaskManagerView> view, base::ProcessId pid,
             std::optional<size_t> button, const std::u16string& input) {
            if (view && button == 1u) {
              view->model().End(pid);
            }
          },
          weak_factory_.GetWeakPtr(), pid)));
}

void TaskManagerView::ShowProcessMenu(base::ProcessId pid,
                                      const gfx::Point& point_in_view) {
  const TaskProcess* process = model_->Find(pid);
  if (!process || process->tasks.empty()) {
    return;
  }
  menu_delegate_ = std::make_unique<task_manager_ui::ProcessMenuDelegate>(
      base::BindRepeating(
          [](base::WeakPtr<TaskManagerView> view, base::ProcessId pid,
             int command) {
            if (view) {
              view->ExecuteMenuCommand(pid, command);
            }
          },
          weak_factory_.GetWeakPtr(), pid));
  menu_model_ = std::make_unique<ui::SimpleMenuModel>(menu_delegate_.get());
  const auto add = [&](int command, const std::u16string& title,
                       const gfx::VectorIcon& icon) {
    menu_model_->AddItemWithIcon(
        command, title,
        ui::ImageModel::FromVectorIcon(icon, ui::kColorMenuIcon, 14));
  };
  switch (process->tasks.front().source) {
    case TaskEntry::Source::kTab:
      add(task_manager_ui::kSwitchToTab, L("taskManager.switchToTab"),
          icons::kArrowUpRight);
      break;
    case TaskEntry::Source::kExtension:
      add(task_manager_ui::kManageExtension, L("taskManager.manageExtension"),
          icons::kPuzzlepiece);
      break;
    default:
      break;
  }
  add(task_manager_ui::kCopyPid, L("taskManager.copyPID"), icons::kNumber);
  if (process->can_end) {
    menu_model_->AddSeparator(ui::NORMAL_SEPARATOR);
    add(task_manager_ui::kEndProcess, L("taskManager.endProcess"),
        icons::kXmarkOctagon);
  }
  MenuOptions options;
  options.destructive = base::BindRepeating(
      [](int command) { return command == task_manager_ui::kEndProcess; });
  menu_host_->ShowMenuAt(
      menu_model_.get(), point_in_view,
      base::BindOnce(
          [](base::WeakPtr<TaskManagerView> view) {
            if (view) {
              // 模型留到下一轮（菜单在关的动画里还会读它）。
              base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
                  FROM_HERE, std::move(view->menu_model_));
            }
          },
          weak_factory_.GetWeakPtr()),
      std::move(options));
}

void TaskManagerView::ExecuteMenuCommand(base::ProcessId pid, int command) {
  switch (command) {
    case task_manager_ui::kSwitchToTab:
    case task_manager_ui::kManageExtension:
      Open(pid);
      break;
    case task_manager_ui::kCopyPid:
      ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
          .WriteText(base::NumberToString16(pid));
      break;
    case task_manager_ui::kEndProcess:
      RequestEnd(pid);
      break;
  }
}

bool TaskManagerView::HandleKey(const ui::KeyEvent& event) {
  if (event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  switch (event.key_code()) {
    case ui::VKEY_UP:
    case ui::VKEY_DOWN: {
      std::optional<base::ProcessId> before = model_->selection();
      model_->MoveSelection(event.key_code() == ui::VKEY_UP ? -1 : 1);
      std::optional<base::ProcessId> after = model_->selection();
      model_->Select(before);
      Select(after);
      return true;
    }
    case ui::VKEY_RETURN:
      if (const TaskProcess* process = model_->selected_process()) {
        Open(process->pid);
        return true;
      }
      return false;
    case ui::VKEY_BACK:
    case ui::VKEY_DELETE:
      if (const TaskProcess* process = model_->selected_process();
          process && process->can_end) {
        RequestEnd(process->pid);
        return true;
      }
      return false;
    default:
      return false;
  }
}

void TaskManagerView::SortForTesting(TaskManagerModel::Column column) {
  model_->Sort(column);
}

void TaskManagerView::HoverTileForTesting(size_t tile,
                                          std::optional<size_t> index) {
  if (tile < tiles_.size()) {
    tiles_[tile]->HoverForTesting(index);
  }
}

std::string TaskManagerView::DebugDescription() const {
  std::string text = base::StrCat(
      {"summary=\"", base::UTF16ToUTF8(summary_->text()), "\" sort=",
       base::NumberToString(static_cast<int>(model_->sort_column())),
       model_->ascending() ? " asc" : " desc", " selection=",
       model_->selection() ? base::NumberToString(*model_->selection())
                           : std::string("none"),
       " gpu_available=",
       model_->gpu_available()
           ? (*model_->gpu_available() ? std::string("yes") : std::string("no"))
           : std::string("?"),
       " theme=", theme_.is_dark ? "dark" : "light"});
  constexpr std::array<const char*, 3> names = {"cpu", "memory", "gpu"};
  for (size_t i = 0; i < tiles_.size(); ++i) {
    base::StrAppend(&text,
                    {"\n  tile ", names[i], " value=\"",
                     base::UTF16ToUTF8(tiles_[i]->ValueText()), "\" caption=\"",
                     base::UTF16ToUTF8(tiles_[i]->CaptionText()),
                     "\" bounds=", tiles_[i]->GetBoundsInScreen().ToString()});
  }
  for (ProcessRow* row : table_->list()->rows()) {
    const TaskProcess& process = row->process();
    const gfx::Rect bounds =
        views::View::ConvertRectToTarget(row->parent(), this, row->bounds());
    base::StrAppend(
        &text,
        {"\n  row pid=",
         base::NumberToString(process.pid),
         " kind=",
         base::NumberToString(static_cast<int>(process.kind)),
         " \"",
         base::UTF16ToUTF8(process.title()),
         "\" | ",
         process.tasks.empty() ? std::string()
                               : base::UTF16ToUTF8(process.tasks[0].detail),
         " | mem=",
         process.memory ? base::UTF16ToUTF8(FormatMemory(*process.memory))
                        : std::string("-"),
         " cpu=",
         process.cpu ? base::StringPrintf("%.1f", *process.cpu)
                     : std::string("-"),
         " gpu=",
         process.gpu ? base::StringPrintf("%.1f", *process.gpu)
                     : std::string("-"),
         " tasks=",
         base::NumberToString(process.tasks.size()),
         process.can_end ? " can_end" : "",
         process.unresponsive ? " unresponsive" : "",
         " rect=",
         bounds.ToString()});
    for (size_t i = 1; i < process.tasks.size(); ++i) {
      base::StrAppend(&text,
                      {"\n    + \"", base::UTF16ToUTF8(process.tasks[i].title),
                       "\" | ", base::UTF16ToUTF8(process.tasks[i].detail)});
    }
  }
  base::StrAppend(&text,
                  {"\n  footer=\"", base::UTF16ToUTF8(footer_->text()), "\""});
  return text;
}

BEGIN_METADATA(TaskManagerView)
END_METADATA

}  // namespace starry
