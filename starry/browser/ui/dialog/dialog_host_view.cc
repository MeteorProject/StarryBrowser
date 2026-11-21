// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/dialog/dialog_host_view.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/focus/focus_manager.h"

namespace starry {

namespace {

constexpr int kPadding = 24;
constexpr int kTopPadding = 22;
constexpr int kIconSize = 40;
constexpr int kIconGlyph = 17;
constexpr int kIconRadius = 10;
constexpr int kIconGap = 12;
constexpr int kFieldHeight = 34;
constexpr int kFieldPadding = 14;
constexpr int kButtonGap = 8;
constexpr int kBottomPadding = 20;
// PillButton 外面留给键盘选中外圈的边距。
constexpr int kButtonOutset = 3;
constexpr int kButtonHeight = 32 + 2 * kButtonOutset;

SkColor WithAlpha(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(255 * opacity));
}

}  // namespace

DialogSpec::DialogSpec() = default;
DialogSpec::DialogSpec(const DialogSpec&) = default;
DialogSpec& DialogSpec::operator=(const DialogSpec&) = default;
DialogSpec::~DialogSpec() = default;

std::unique_ptr<views::View> DialogRequest::CreateContentView() {
  return nullptr;
}

namespace {

class CallbackDialogRequest : public DialogRequest {
 public:
  CallbackDialogRequest(DialogSpec spec, DialogAnswerCallback callback)
      : spec_(std::move(spec)), callback_(std::move(callback)) {}

  const DialogSpec& spec() const override { return spec_; }

  void Answer(size_t button, const std::u16string& input) override {
    if (callback_) {
      std::move(callback_).Run(button, input);
    }
  }

  void Dismissed() override {
    if (callback_) {
      std::move(callback_).Run(std::nullopt, std::u16string());
    }
  }

 private:
  const DialogSpec spec_;
  DialogAnswerCallback callback_;
};

}  // namespace

std::unique_ptr<DialogRequest> MakeDialogRequest(DialogSpec spec,
                                                 DialogAnswerCallback callback) {
  return std::make_unique<CallbackDialogRequest>(std::move(spec),
                                                 std::move(callback));
}

// 卡片上的面板色、细描边、图标块、标题和输入框的胶囊底。
class DialogCardPanel : public views::View {
  METADATA_HEADER(DialogCardPanel, views::View)

 public:
  DialogCardPanel() = default;

  void Set(const DialogSpec& spec, const Theme& theme) {
    spec_ = spec;
    theme_ = theme;
    SchedulePaint();
  }

  void SetFieldRect(const gfx::Rect& rect, bool focused) {
    field_rect_ = rect;
    field_focused_ = focused;
    SchedulePaint();
  }

  int title_x() const { return spec_.icon ? kPadding + kIconSize + kIconGap
                                          : kPadding; }

  // 标题占的高度：一行时和图标块一样高（文字居中），放不下时折成几行（英文、韩文
  // 常常比中文长一倍）。card_width 是卡片的宽度。
  int TitleHeight(int card_width) const {
    const int line_box = spec_.icon ? kIconSize : 22;
    const int text_height = WrappedTitleHeight(card_width);
    return text_height > TitleFont().GetHeight() ? std::max(line_box, text_height)
                                                 : line_box;
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(WithAlpha(theme_.surface_panel, theme_.is_dark ? 0.55f : 0.65f));
    canvas->DrawRoundRect(bounds, radius::kLarge, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius::kLarge - 0.5f, border);

    if (spec_.icon) {
      const SkColor tint = spec_.tone == DialogSpec::Tone::kDanger
                               ? theme_.danger()
                           : spec_.tone == DialogSpec::Tone::kWarning
                               ? theme_.warning()
                               : theme_.accent;
      const gfx::Rect tile(kPadding, kTopPadding, kIconSize, kIconSize);
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(WithAlpha(tint, theme_.is_dark ? 0.16f : 0.1f));
      canvas->DrawRoundRect(gfx::RectF(tile), kIconRadius, flags);
      canvas->Save();
      canvas->Translate(gfx::Vector2d(tile.x() + (kIconSize - kIconGlyph) / 2,
                                      tile.y() + (kIconSize - kIconGlyph) / 2));
      gfx::PaintVectorIcon(canvas, *spec_.icon, kIconGlyph, tint);
      canvas->Restore();
    }
    const int text_width = std::max(0, width() - title_x() - kPadding);
    const int wrapped = WrappedTitleHeight(width());
    if (wrapped > TitleFont().GetHeight()) {
      canvas->DrawStringRectWithFlags(
          spec_.title, TitleFont(), theme_.on_surface,
          gfx::Rect(title_x(), kTopPadding, text_width, wrapped),
          gfx::Canvas::MULTI_LINE | gfx::Canvas::NO_SUBPIXEL_RENDERING);
    } else {
      canvas->DrawStringRectWithFlags(
          spec_.title, TitleFont(), theme_.on_surface,
          gfx::Rect(title_x(), kTopPadding, text_width,
                    spec_.icon ? kIconSize : 22),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }

    if (!field_rect_.IsEmpty()) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(field_focused_ ? 0.08f : 0.06f));
      canvas->DrawRoundRect(gfx::RectF(field_rect_), kFieldHeight / 2.0f,
                            flags);
      if (field_focused_) {
        cc::PaintFlags ring;
        ring.setAntiAlias(true);
        ring.setStyle(cc::PaintFlags::kStroke_Style);
        ring.setStrokeWidth(1);
        ring.setColor(theme_.OnSurface(0.16f));
        gfx::RectF outline(field_rect_);
        outline.Inset(0.5f);
        canvas->DrawRoundRect(outline, outline.height() / 2, ring);
      }
    }
  }

 private:
  static const gfx::FontList& TitleFont() {
    return UiFont(16, gfx::Font::Weight::SEMIBOLD);
  }

  int WrappedTitleHeight(int card_width) const {
    int text_width = std::max(1, card_width - title_x() - kPadding);
    int text_height = 0;
    gfx::Canvas::SizeStringInt(spec_.title, TitleFont(), &text_width,
                               &text_height, 0, gfx::Canvas::MULTI_LINE);
    return text_height;
  }

  DialogSpec spec_;
  Theme theme_ = Theme::DarkBase();
  gfx::Rect field_rect_;
  bool field_focused_ = false;
};

BEGIN_METADATA(DialogCardPanel)
END_METADATA

// 一张对话框卡片（玻璃）。
class DialogCard : public GlassView {
  METADATA_HEADER(DialogCard, GlassView)

 public:
  DialogCard(const DialogSpec& spec,
             const Theme& theme,
             DialogHostView* host,
             std::unique_ptr<views::View> content)
      : spec_(spec) {
    SetCornerRadius(radius::kLarge, true);
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);

    panel_ = AddChildView(std::make_unique<DialogCardPanel>());
    panel_->Set(spec, theme);

    if (content) {
      // 圆角裁掉内容（网页也是图层，裁得到）。
      content_ = panel_->AddChildView(std::move(content));
      content_->SetPaintToLayer();
      content_->layer()->SetFillsBoundsOpaquely(false);
      content_->layer()->SetRoundedCornerRadius(
          gfx::RoundedCornersF(radius::kLarge));
      content_->layer()->SetIsFastRoundedCorner(true);
      GetViewAccessibility().SetRole(ax::mojom::Role::kDialog);
      return;
    }

    if (!spec.message.empty()) {
      scroll_ = panel_->AddChildView(std::make_unique<views::ScrollView>(
          views::ScrollView::ScrollWithLayers::kEnabled));
      scroll_->SetBackgroundColor(std::nullopt);
      scroll_->SetHorizontalScrollBarMode(
          views::ScrollView::ScrollBarMode::kDisabled);
      scroll_->SetDrawOverflowIndicator(false);
      auto label = std::make_unique<views::Label>(
          spec.message, views::Label::CustomFont{UiFont(13)});
      label->SetMultiLine(true);
      label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      label->SetAutoColorReadabilityEnabled(false);
      label->SetEnabledColor(theme.OnSurface(0.8f));
      label->SetSelectable(true);
      message_ = scroll_->SetContents(std::move(label));
    }
    if (spec.field) {
      field_ = panel_->AddChildView(std::make_unique<views::Textfield>());
      field_->SetController(host);
      field_->SetBackgroundEnabled(false);
      field_->SetBorder(nullptr);
      field_->SetFontList(UiFont(13));
      field_->SetColor(theme.on_surface);
      field_->SetText(*spec.field);
    }
    for (size_t i = 0; i < spec.buttons.size(); ++i) {
      const DialogSpec::Button& button = spec.buttons[i];
      auto* pill = panel_->AddChildView(std::make_unique<PillButton>(
          base::BindRepeating(&DialogHostView::PressButton,
                              base::Unretained(host), i),
          button.title, button.variant));
      pill->SetTheme(theme);
      buttons_.push_back(pill);
    }
    GetViewAccessibility().SetRole(ax::mojom::Role::kAlertDialog);
    GetViewAccessibility().SetName(spec.title);
  }

  views::Textfield* field() { return field_; }
  views::View* content() { return content_; }
  const std::vector<raw_ptr<PillButton, VectorExperimental>>& buttons() const {
    return buttons_;
  }
  LayerMotion& motion() { return motion_; }

  // 给定最大宽高，卡片多大；同时排好里面的东西。
  gfx::Size Arrange(int width, int max_height) {
    if (content_) {
      const gfx::Size size(width,
                           std::min(spec_.content_size.height(), max_height));
      content_->SetBoundsRect(gfx::Rect(size));
      return size;
    }
    const bool has_body = message_ || field_;
    int y = kTopPadding + panel_->TitleHeight(width) +
            (has_body ? 14 : kBottomPadding);
    const int inner = std::max(0, width - 2 * kPadding);

    // 按钮一行（固定在底部）。
    const int buttons_height =
        kButtonHeight - kButtonOutset + (has_body ? 18 - kButtonOutset : 0) +
        kBottomPadding;
    const int field_height = field_ ? kFieldHeight + (message_ ? 12 : 0) : 0;
    int body_height = 0;
    if (message_) {
      const int wanted = message_->GetHeightForWidth(inner) + 8;
      const int room = max_height - y - field_height - buttons_height;
      body_height = std::clamp(wanted, std::min(40, wanted), std::max(room, 0));
      scroll_->SetBounds(kPadding, y, inner, body_height);
      message_->SetBounds(0, 4, inner, message_->GetHeightForWidth(inner));
      scroll_->ClipHeightTo(0, body_height);
      y += body_height;
    }
    gfx::Rect field_rect;
    if (field_) {
      y += message_ ? 12 : 0;
      field_rect = gfx::Rect(kPadding, y, inner, kFieldHeight);
      field_->SetBounds(field_rect.x() + kFieldPadding, field_rect.y() + 5,
                        std::max(0, field_rect.width() - 2 * kFieldPadding),
                        kFieldHeight - 10);
      y += kFieldHeight;
    }
    panel_->SetFieldRect(field_rect, field_ && field_->HasFocus());

    y += has_body ? 18 - kButtonOutset : -kButtonOutset;
    int right = width - kPadding + kButtonOutset;
    for (size_t i = buttons_.size(); i-- > 0;) {
      const gfx::Size size = buttons_[i]->GetPreferredSize();
      right -= size.width();
      buttons_[i]->SetBounds(right, y, size.width(), size.height());
      right -= kButtonGap - 2 * kButtonOutset;
    }
    y += kButtonHeight - kButtonOutset + kBottomPadding;
    return gfx::Size(width, y);
  }

  void PlaceAt(const gfx::Rect& rect) {
    SetGlassBounds(rect);
    panel_->SetBoundsRect(glass_rect());
  }

  void UpdateFieldFocus() {
    if (field_) {
      panel_->SetFieldRect(
          gfx::Rect(kPadding, field_->y() - 5, field_->width() + 2 * kFieldPadding,
                    kFieldHeight),
          field_->HasFocus());
    }
  }

 private:
  const DialogSpec spec_;
  raw_ptr<DialogCardPanel> panel_ = nullptr;
  raw_ptr<views::ScrollView> scroll_ = nullptr;
  raw_ptr<views::Label> message_ = nullptr;
  raw_ptr<views::Textfield> field_ = nullptr;
  raw_ptr<views::View> content_ = nullptr;
  std::vector<raw_ptr<PillButton, VectorExperimental>> buttons_;
  LayerMotion motion_;
};

BEGIN_METADATA(DialogCard)
END_METADATA

DialogHostView::DialogHostView() {
  // 遮罩把鼠标交给本视图。Aura 用命中视图的图层判断点击是否该发给下面的网页。
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  SetVisible(false);
  SetFocusBehavior(FocusBehavior::ALWAYS);
  backdrop_ = AddChildView(std::make_unique<views::View>());
  backdrop_->SetPaintToLayer();
  backdrop_->layer()->SetFillsBoundsOpaquely(false);
  backdrop_->layer()->SetOpacity(0);
  backdrop_->SetCanProcessEventsWithinSubtree(false);
  SetTheme(theme_);
}

DialogHostView::~DialogHostView() {
  // 窗口关掉：还没回答的都算被拿掉。
  if (current_) {
    current_->Dismissed();
  }
  for (auto& request : queue_) {
    request->Dismissed();
  }
}

void DialogHostView::Present(std::unique_ptr<DialogRequest> request) {
  queue_.push_back(std::move(request));
  if (!current_) {
    ShowNext();
  }
}

void DialogHostView::Remove(DialogRequest* request) {
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->get() == request) {
      base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
          FROM_HERE, std::move(*it));
      queue_.erase(it);
      return;
    }
  }
  if (current_.get() == request) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(current_));
    HideCard(/*then_next=*/true);
  }
}

std::u16string DialogHostView::CurrentInput() const {
  if (card_ && card_->field()) {
    return std::u16string(card_->field()->GetText());
  }
  return std::u16string();
}

void DialogHostView::SetTheme(const Theme& theme) {
  theme_ = theme;
  // 遮罩：深色 42%、浅色 20% 的黑。
  backdrop_->SetBackground(views::CreateSolidBackground(
      SkColorSetA(SK_ColorBLACK, theme.is_dark ? 0x6B : 0x33)));
}

void DialogHostView::PressButton(size_t index) {
  Respond(index);
}

void DialogHostView::TypeForTesting(const std::u16string& text) {
  if (card_ && card_->field()) {
    card_->field()->SetText(text);
  }
}

void DialogHostView::Layout(PassKey) {
  backdrop_->SetBoundsRect(GetLocalBounds());
  if (!card_) {
    return;
  }
  const DialogSpec& spec = current_ ? current_->spec() : DialogSpec();
  const bool content = card_->content();
  const int width =
      std::min(content ? spec.content_size.width() : metrics::kDialogWidth,
               std::max(0, this->width() - 2 * metrics::kDialogMargin));
  const int max_height =
      std::min(content ? spec.content_size.height() : metrics::kDialogMaxHeight,
               std::max(0, height() - 2 * metrics::kDialogMargin));
  const gfx::Size size = card_->Arrange(width, max_height);
  card_->PlaceAt(gfx::Rect((this->width() - size.width()) / 2,
                           (height() - size.height()) / 2, size.width(),
                           size.height()));
}

bool DialogHostView::OnMousePressed(const ui::MouseEvent& event) {
  // 点在遮罩上：卡片动一下，告诉用户要先回答。
  if (card_ && !card_->bounds().Contains(event.location())) {
    Nudge();
  }
  return true;
}

bool DialogHostView::SkipDefaultKeyEventProcessing(const ui::KeyEvent& event) {
  return is_presenting() &&
         (event.key_code() == ui::VKEY_TAB ||
          event.key_code() == ui::VKEY_RETURN ||
          event.key_code() == ui::VKEY_ESCAPE);
}

bool DialogHostView::OnKeyPressed(const ui::KeyEvent& event) {
  if (!current_) {
    return false;
  }
  switch (event.key_code()) {
    case ui::VKEY_RETURN:
    case ui::VKEY_SPACE:
      if (event.key_code() == ui::VKEY_SPACE && !keyboard_selection_) {
        return true;
      }
      RespondDefault();
      return true;
    case ui::VKEY_ESCAPE:
      RespondCancel();
      return true;
    case ui::VKEY_TAB:
      MoveKeyboardSelection(event.IsShiftDown() ? -1 : 1);
      return true;
    case ui::VKEY_LEFT:
      MoveKeyboardSelection(-1);
      return true;
    case ui::VKEY_RIGHT:
      MoveKeyboardSelection(1);
      return true;
    default:
      return true;
  }
}

bool DialogHostView::HandleKeyEvent(views::Textfield* sender,
                                    const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed ||
      sender->IsIMEComposing()) {
    return false;
  }
  switch (key_event.key_code()) {
    case ui::VKEY_RETURN:
      RespondDefault();
      return true;
    case ui::VKEY_ESCAPE:
      RespondCancel();
      return true;
    default:
      return false;
  }
}

void DialogHostView::ShowNext() {
  if (queue_.empty()) {
    return;
  }
  const bool first = !GetVisible() || !current_;
  current_ = std::move(queue_.front());
  queue_.pop_front();
  keyboard_selection_.reset();
  shown_at_ = base::TimeTicks::Now();
  ++generation_;

  if (!GetVisible()) {
    views::FocusManager* focus_manager = GetFocusManager();
    previous_focus_.SetView(focus_manager ? focus_manager->GetFocusedView()
                                          : nullptr);
    SetVisible(true);
  }
  SetCanProcessEventsWithinSubtree(true);
  if (first) {
    backdrop_motion_.AnimateOpacity(backdrop_->layer(), 1,
                                    motion::DialogBackdrop());
  }

  card_ = AddChildView(std::make_unique<DialogCard>(
      current_->spec(), theme_, this, current_->CreateContentView()));
  InvalidateLayout();
  DeprecatedLayoutImmediately();

  // 从 0.95 放大、上移 10 并淡入（motion::kDialogIn）。自己画内容的只淡入。
  const gfx::Rect rect = card_->glass_rect();
  const MotionCurve curve = MotionCurve::Of(motion::kDialogIn);
  if (!card_->content()) {
    LayerPose from;
    if (!motion::ReduceMotion()) {
      from.scale = 0.95f;
      from.offset = gfx::Vector2dF(0, 10);
    }
    card_->motion().AnimateTransformFrom(card_->layer(), from, LayerPose(),
                                         curve,
                                         gfx::PointF(rect.CenterPoint()));
  }
  card_->motion().AnimateOpacityFrom(card_->layer(), 0, 1, curve);

  if (card_->content()) {
    card_->content()->RequestFocus();
  } else if (card_->field()) {
    card_->field()->RequestFocus();
    card_->field()->SelectAll(false);
    card_->UpdateFieldFocus();
  } else {
    RequestFocus();
  }
}

void DialogHostView::Respond(size_t button) {
  if (!current_ || button >= current_->spec().buttons.size()) {
    return;
  }
  const DialogSpec& spec = current_->spec();
  if (!spec.buttons[button].cancel && spec.input_protection.is_positive() &&
      base::TimeTicks::Now() - shown_at_ < spec.input_protection) {
    Nudge();
    return;
  }
  std::unique_ptr<DialogRequest> request = std::move(current_);
  const std::u16string input = CurrentInput();
  HideCard(/*then_next=*/true);
  request->Answer(button, input);
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, std::move(request));
}

void DialogHostView::RespondDefault() {
  if (!current_ || current_->spec().buttons.empty()) {
    return;
  }
  if (!keyboard_selection_ && current_->spec().no_default_button) {
    Nudge();
    return;
  }
  Respond(keyboard_selection_.value_or(current_->spec().buttons.size() - 1));
}

void DialogHostView::RespondCancel() {
  if (!current_) {
    return;
  }
  // 自己画内容的（焦点不在内容里时才到这里）：Esc 当作没回答就关掉。
  if (card_ && card_->content()) {
    std::unique_ptr<DialogRequest> request = std::move(current_);
    HideCard(/*then_next=*/true);
    request->Dismissed();
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(request));
    return;
  }
  const auto& buttons = current_->spec().buttons;
  for (size_t i = 0; i < buttons.size(); ++i) {
    if (buttons[i].cancel) {
      Respond(i);
      return;
    }
  }
  // 只有一个按钮时 Esc 也是它。
  if (buttons.size() == 1) {
    Respond(0);
  }
}

void DialogHostView::MoveKeyboardSelection(int delta) {
  if (!card_ || card_->buttons().empty()) {
    return;
  }
  const size_t count = card_->buttons().size();
  size_t index = keyboard_selection_.value_or(delta > 0 ? count - 1 : 0);
  index = (index + count + (delta > 0 ? 1 : count - 1)) % count;
  keyboard_selection_ = index;
  for (size_t i = 0; i < count; ++i) {
    card_->buttons()[i]->SetKeyboardSelected(i == index);
  }
  RequestFocus();
}

void DialogHostView::HideCard(bool then_next) {
  DialogCard* card = card_;
  card_ = nullptr;
  const bool more = then_next && !queue_.empty();
  if (card) {
    card->SetCanProcessEventsWithinSubtree(false);
    // 缩到 0.98 淡出（motion::DialogOut()）。自己画内容的只淡出。
    if (!card->content()) {
      LayerPose to;
      if (!motion::ReduceMotion()) {
        to.scale = 0.98f;
      }
      const gfx::Rect rect = card->glass_rect();
      card->motion().AnimateTransform(card->layer(), to, motion::DialogOut(),
                                      gfx::PointF(rect.CenterPoint()));
    }
    card->motion().AnimateOpacity(
        card->layer(), 0, motion::DialogOut(),
        base::BindOnce(
            [](base::WeakPtr<DialogHostView> host, DialogCard* card) {
              if (!host) {
                return;
              }
              for (views::View* child : host->children()) {
                if (child == card) {
                  host->RemoveChildViewT(card);
                  break;
                }
              }
            },
            weak_factory_.GetWeakPtr(), base::Unretained(card)));
  }
  if (more) {
    // 排队的下一个直接接上（遮罩不闪）。
    ShowNext();
    return;
  }
  // 都答完了：遮罩淡出，焦点还给原来的视图。
  SetCanProcessEventsWithinSubtree(false);
  const int generation = ++generation_;
  backdrop_motion_.AnimateOpacity(
      backdrop_->layer(), 0, motion::DialogOut(),
      base::BindOnce(
          [](base::WeakPtr<DialogHostView> host, int generation) {
            if (host && host->generation_ == generation && !host->current_) {
              host->SetVisible(false);
            }
          },
          weak_factory_.GetWeakPtr(), generation));
  if (previous_focus_.view()) {
    previous_focus_.view()->RequestFocus();
  }
  previous_focus_.SetView(nullptr);
}

void DialogHostView::Nudge() {
  // 网页内容不能缩放（见 CreateContentView）。
  if (!card_ || card_->content() || motion::ReduceMotion()) {
    return;
  }
  const gfx::Rect rect = card_->glass_rect();
  card_->motion().AnimateTransformFrom(
      card_->layer(), LayerPose(), LayerPose(),
      MotionCurve::Keyframes(
          motion::kDialogNudgeOut + motion::kDialogNudgeBack,
          {1, motion::kDialogNudgeScale, 0.996f, 1},
          {0,
           static_cast<float>(motion::kDialogNudgeOut /
                              (motion::kDialogNudgeOut +
                               motion::kDialogNudgeBack)),
           0.75f, 1}),
      gfx::PointF(rect.CenterPoint()));
}

BEGIN_METADATA(DialogHostView)
END_METADATA

}  // namespace starry
