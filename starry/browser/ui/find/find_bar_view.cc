// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/find/find_bar_view.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/find_bar/find_bar_controller.h"
#include "components/find_in_page/find_notification_details.h"
#include "components/find_in_page/find_tab_helper.h"
#include "components/find_in_page/find_types.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
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
#include "ui/gfx/range/range.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

// 上一个、下一个的提示；仅 macOS 附带快捷键文案。
const std::u16string& PreviousTooltip() {
#if BUILDFLAG(IS_MAC)
  return L("page.find.previous.help");
#else
  return L("page.find.previous");
#endif
}

const std::u16string& NextTooltip() {
#if BUILDFLAG(IS_MAC)
  return L("page.find.next.help");
#else
  return L("page.find.next");
#endif
}

constexpr int kMargin = 10;
constexpr int kHeight = 38;
constexpr int kLeading = 12;
constexpr int kTrailing = 6;
constexpr int kSpacing = 6;
constexpr int kButtonSize = 22;
constexpr float kRadius = 10;
// 出现时从上方 12 pt 落下并由 0.96 放大；收起时向上 6 pt 淡出。
constexpr float kDropOffset = -12;
constexpr float kDropScale = 0.96f;
constexpr float kLiftOffset = -6;

SkColor WithAlpha(SkColor color, float opacity) {
  return SkColorSetA(color, static_cast<U8CPU>(255 * opacity));
}

}  // namespace

// 玻璃上的面板色、细描边和放大镜。
class FindBarPanel : public views::View {
  METADATA_HEADER(FindBarPanel, views::View)

 public:
  FindBarPanel() = default;

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(WithAlpha(theme_.surface_panel, theme_.is_dark ? 0.55f : 0.65f));
    canvas->DrawRoundRect(bounds, kRadius, fill);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, kRadius - 0.5f, border);

    canvas->Save();
    canvas->Translate(gfx::Vector2d(kLeading, (height() - 12) / 2));
    gfx::PaintVectorIcon(canvas, icons::kMagnifyingGlass, 12,
                         theme_.on_surface_variant);
    canvas->Restore();
  }

 private:
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(FindBarPanel)
END_METADATA

FindBarView::FindBarView() {
  SetVisible(false);
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  glass_ = AddChildView(std::make_unique<GlassView>());
  glass_->SetCornerRadius(kRadius, true);
  panel_ = glass_->AddChildView(std::make_unique<FindBarPanel>());

  field_ = panel_->AddChildView(std::make_unique<views::Textfield>());
  field_->SetController(this);
  field_->SetBackgroundEnabled(false);
  field_->SetBorder(nullptr);
  field_->SetFontList(UiFont(13));
  field_->SetPlaceholderText(L("page.find.placeholder"));
  field_->GetViewAccessibility().SetName(L("page.find.placeholder"));

  not_found_label_ = panel_->AddChildView(std::make_unique<views::Label>(
      L("page.find.notFound"), views::Label::CustomFont{UiFont(11, gfx::Font::Weight::MEDIUM)}));
  not_found_label_->SetAutoColorReadabilityEnabled(false);
  not_found_label_->SetVisible(false);

  previous_button_ = panel_->AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&FindBarView::FindNext, base::Unretained(this), true),
      icons::kChevronUp, PreviousTooltip(), kButtonSize, 11));
  next_button_ = panel_->AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&FindBarView::FindNext, base::Unretained(this),
                          false),
      icons::kChevronDown, NextTooltip(), kButtonSize, 11));
  close_button_ = panel_->AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&FindBarView::EndSession, base::Unretained(this)),
      icons::kXmark, L("page.find.done.help"), kButtonSize, 11));
  SetTheme(theme_);
}

FindBarView::~FindBarView() = default;

void FindBarView::SetTheme(const Theme& theme) {
  theme_ = theme;
  GlassStyle style;
  style.dark = theme.is_dark;
  style.luma = theme.is_dark ? 0.1f : 0.9f;
  glass_->SetStyle(style);
  panel_->SetTheme(theme);
  field_->SetColor(theme.on_surface);
  not_found_label_->SetEnabledColor(theme.danger());
  for (IconButton* button :
       {previous_button_.get(), next_button_.get(), close_button_.get()}) {
    button->SetTheme(theme);
  }
}

void FindBarView::SetCardBounds(const gfx::Rect& card) {
  if (card_ == card) {
    return;
  }
  card_ = card;
  InvalidateLayout();
}

void FindBarView::Layout(PassKey) {
  const int width = std::min(metrics::kFindBarWidth,
                             std::max(0, card_.width() - 2 * kMargin));
  const gfx::Rect bar(card_.right() - kMargin - width - x(),
                      card_.y() + kMargin - y(), width, kHeight);
  glass_->SetGlassBounds(bar);
  panel_->SetBoundsRect(glass_->glass_rect());

  int right = bar.width() - kTrailing;
  const int button_y = (kHeight - kButtonSize) / 2;
  for (IconButton* button :
       {close_button_.get(), next_button_.get(), previous_button_.get()}) {
    right -= kButtonSize;
    button->SetBounds(right, button_y, kButtonSize, kButtonSize);
    right -= 2;
  }
  right -= kSpacing - 2;
  if (not_found_label_->GetVisible()) {
    const int label = not_found_label_->GetPreferredSize().width();
    right -= label;
    not_found_label_->SetBounds(right, 0, label, kHeight);
    right -= kSpacing;
  }
  const int field_x = kLeading + 12 + kSpacing;
  field_->SetBounds(field_x, (kHeight - 24) / 2,
                    std::max(0, right - field_x), 24);
}

bool FindBarView::DoesIntersectRect(const views::View* target,
                                    const gfx::Rect& rect) const {
  if (!shown_) {
    return false;
  }
  gfx::Rect bar = glass_->glass_rect();
  bar.Offset(glass_->bounds().OffsetFromOrigin());
  return bar.Intersects(rect);
}

gfx::Rect FindBarView::ShownRect() const {
  if (!shown_) {
    return gfx::Rect();
  }
  gfx::Rect bar = glass_->glass_rect();
  bar.Offset(glass_->bounds().OffsetFromOrigin());
  return bar;
}

void FindBarView::ShowBar(bool animate, bool focus) {
  const bool was_shown = shown_;
  shown_ = true;
  ++hide_generation_;
  SetVisible(true);
  InvalidateLayout();
  DeprecatedLayoutImmediately();
  if (!was_shown && shown_changed_) {
    shown_changed_.Run();
  }
  if (!was_shown) {
    const gfx::Rect rect = glass_->glass_rect();
    const gfx::PointF origin(rect.CenterPoint().x(), rect.y());
    const MotionCurve curve = MotionCurve::Of(motion::kFindBar);
    if (animate && !motion::ReduceMotion()) {
      LayerPose from;
      from.offset = gfx::Vector2dF(0, kDropOffset);
      from.scale = kDropScale;
      motion_.AnimateTransformFrom(glass_->layer(), from, LayerPose(), curve,
                                   origin);
      motion_.AnimateOpacityFrom(glass_->layer(), 0, 1, curve);
    } else {
      motion_.SetTransform(glass_->layer(), LayerPose(), origin);
      motion_.SetOpacity(glass_->layer(), 1);
    }
  }
  if (focus) {
    FocusAndSelectAll();
  }
}

void FindBarView::HideBar(bool animate) {
  if (!shown_) {
    return;
  }
  shown_ = false;
  if (shown_changed_) {
    shown_changed_.Run();
  }
  const int generation = ++hide_generation_;
  if (!animate || motion::ReduceMotion()) {
    SetVisible(false);
    return;
  }
  const gfx::Rect rect = glass_->glass_rect();
  LayerPose to;
  to.offset = gfx::Vector2dF(0, kLiftOffset);
  const MotionCurve fade = motion::DialogOut();
  motion_.AnimateTransform(glass_->layer(), to, fade,
                           gfx::PointF(rect.CenterPoint().x(), rect.y()));
  motion_.AnimateOpacity(
      glass_->layer(), 0, fade,
      base::BindOnce(
          [](base::WeakPtr<FindBarView> view, int generation) {
            if (view && view->hide_generation_ == generation) {
              view->SetVisible(false);
            }
          },
          weak_factory_.GetWeakPtr(), generation));
}

void FindBarView::FocusAndSelectAll() {
  field_->RequestFocus();
  field_->SelectAll(true);
}

void FindBarView::SetFindTextAndSelectedRange(const std::u16string& text,
                                              const gfx::Range& range) {
  field_->SetText(text);
  field_->SetSelectedRange(range);
}

std::u16string_view FindBarView::GetFindText() const {
  return field_->GetText();
}

gfx::Range FindBarView::GetSelectedRange() const {
  return field_->GetSelectedRange();
}

void FindBarView::UpdateForResult(
    const find_in_page::FindNotificationDetails& result,
    const std::u16string& find_text) {
  if (find_text.empty() || result.number_of_matches() < 0) {
    match_text_.clear();
    SetNotFound(false);
    return;
  }
  match_text_ = base::NumberToString16(result.active_match_ordinal()) + u"/" +
                base::NumberToString16(result.number_of_matches());
  // 结果是分几次回来的：最后一次才确定找不到。
  if (result.final_update()) {
    SetNotFound(result.number_of_matches() == 0);
  } else if (result.number_of_matches() > 0) {
    SetNotFound(false);
  }
}

void FindBarView::TypeForTesting(const std::u16string& text) {
  field_->SetText(text);
  Find(text);
}

bool FindBarView::FieldHasFocus() const {
  return field_->HasFocus();
}

void FindBarView::ContentsChanged(views::Textfield* sender,
                                  const std::u16string& new_contents) {
  Find(new_contents);
}

bool FindBarView::HandleKeyEvent(views::Textfield* sender,
                                 const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  switch (key_event.key_code()) {
    case ui::VKEY_RETURN:
      // 回车下一个，⇧回车上一个。
      FindNext(key_event.IsShiftDown());
      return true;
    case ui::VKEY_ESCAPE:
      EndSession();
      return true;
    default:
      return false;
  }
}

void FindBarView::Find(const std::u16string& text) {
  FindBarController* controller =
      find_bar_ ? find_bar_->GetFindBarController() : nullptr;
  content::WebContents* contents =
      controller ? controller->web_contents() : nullptr;
  if (!contents) {
    return;
  }
  auto* helper = find_in_page::FindTabHelper::FromWebContents(contents);
  controller->OnUserChangedFindText(text);
  if (text.empty()) {
    helper->StopFinding(find_in_page::SelectionAction::kClear);
    match_text_.clear();
    SetNotFound(false);
    return;
  }
  helper->StartFinding(text, /*forward_direction=*/true,
                       /*case_sensitive=*/false, /*find_match=*/true);
}

void FindBarView::FindNext(bool reverse) {
  FindBarController* controller =
      find_bar_ ? find_bar_->GetFindBarController() : nullptr;
  content::WebContents* contents =
      controller ? controller->web_contents() : nullptr;
  const std::u16string text(field_->GetText());
  if (!contents || text.empty()) {
    return;
  }
  find_in_page::FindTabHelper::FromWebContents(contents)->StartFinding(
      text, /*forward_direction=*/!reverse, /*case_sensitive=*/false,
      /*find_match=*/true);
}

void FindBarView::EndSession() {
  if (find_bar_ && find_bar_->GetFindBarController()) {
    find_bar_->GetFindBarController()->EndFindSession(
        find_in_page::SelectionAction::kKeep,
        find_in_page::ResultAction::kKeep);
  }
}

void FindBarView::SetNotFound(bool not_found) {
  if (not_found_ == not_found) {
    return;
  }
  not_found_ = not_found;
  not_found_label_->SetVisible(not_found);
  previous_button_->SetEnabled(!not_found);
  next_button_->SetEnabled(!not_found);
  InvalidateLayout();
}

BEGIN_METADATA(FindBarView)
END_METADATA

// StarryFindBar --------------------------------------------------------------

StarryFindBar::StarryFindBar(FindBarView* view) {
  view_.SetView(view);
  view->set_find_bar(this);
}

StarryFindBar::~StarryFindBar() {
  if (FindBarView* bar = view()) {
    bar->set_find_bar(nullptr);
  }
}

FindBarView* StarryFindBar::view() const {
  return static_cast<FindBarView*>(const_cast<views::View*>(view_.view()));
}

FindBarController* StarryFindBar::GetFindBarController() const {
  return controller_;
}

void StarryFindBar::SetFindBarController(
    FindBarController* find_bar_controller) {
  controller_ = find_bar_controller;
}

void StarryFindBar::Show(bool animate, bool focus) {
  if (FindBarView* bar = view()) {
    bar->ShowBar(animate, focus);
  }
}

void StarryFindBar::Hide(bool animate) {
  if (FindBarView* bar = view()) {
    bar->HideBar(animate);
  }
}

void StarryFindBar::SetFocusAndSelection() {
  if (FindBarView* bar = view()) {
    bar->FocusAndSelectAll();
  }
}

void StarryFindBar::ClearResults(
    const find_in_page::FindNotificationDetails& results) {
  if (FindBarView* bar = view()) {
    bar->UpdateForResult(results, std::u16string());
  }
}

void StarryFindBar::StopAnimation() {}

void StarryFindBar::MoveWindowIfNecessary() {}

void StarryFindBar::SetFindTextAndSelectedRange(
    const std::u16string& find_text,
    const gfx::Range& selected_range) {
  if (FindBarView* bar = view()) {
    bar->SetFindTextAndSelectedRange(find_text, selected_range);
  }
}

std::u16string_view StarryFindBar::GetFindText() const {
  FindBarView* bar = view();
  return bar ? bar->GetFindText() : std::u16string_view();
}

gfx::Range StarryFindBar::GetSelectedRange() const {
  FindBarView* bar = view();
  return bar ? bar->GetSelectedRange() : gfx::Range();
}

void StarryFindBar::UpdateUIForFindResult(
    const find_in_page::FindNotificationDetails& result,
    const std::u16string& find_text) {
  if (FindBarView* bar = view()) {
    bar->UpdateForResult(result, find_text);
  }
}

void StarryFindBar::AudibleAlert() {
  ++audible_alerts_;
}

bool StarryFindBar::IsFindBarVisible() const {
  FindBarView* bar = view();
  return bar && bar->is_shown();
}

void StarryFindBar::RestoreSavedFocus() {
  if (controller_ && controller_->web_contents()) {
    controller_->web_contents()->Focus();
  }
}

bool StarryFindBar::HasGlobalFindPasteboard() const {
#if BUILDFLAG(IS_MAC)
  return true;
#else
  return false;
#endif
}

void StarryFindBar::UpdateFindBarForChangedWebContents() {}

bool StarryFindBar::CanPopulateFromSelectedText() {
  return true;
}

const FindBarTesting* StarryFindBar::GetFindBarTesting() const {
  return this;
}

bool StarryFindBar::HasFocus() const {
  FindBarView* bar = view();
  return bar && bar->FieldHasFocus();
}

void StarryFindBar::CloseOverlappingBubbles() {}

views::Widget* StarryFindBar::GetHostWidget() {
  FindBarView* bar = view();
  return bar ? bar->GetWidget() : nullptr;
}

bool StarryFindBar::GetFindBarWindowInfo(gfx::Point* position,
                                         bool* fully_visible) const {
  FindBarView* bar = view();
  if (position) {
    *position = bar ? bar->bounds().origin() : gfx::Point();
  }
  if (fully_visible) {
    *fully_visible = bar && bar->is_shown();
  }
  return bar && bar->is_shown();
}

std::u16string_view StarryFindBar::GetFindSelectedText() const {
  return GetFindText();
}

std::u16string_view StarryFindBar::GetMatchCountText() const {
  FindBarView* bar = view();
  return bar ? bar->match_text() : std::u16string_view();
}

int StarryFindBar::GetContentsWidth() const {
  return metrics::kFindBarWidth;
}

size_t StarryFindBar::GetAudibleAlertCount() const {
  return audible_alerts_;
}

}  // namespace starry
