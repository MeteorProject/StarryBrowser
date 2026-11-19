// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/agent_background_tasks.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "starry/browser/agent/agent_activity.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/assistant/agent_parts.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "ui/base/base_window.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_utils.h"

namespace starry {

namespace {

// 卡片：圆角 16，左 9、右 6、上下 6；图标 16 加 4 的边距（外面一圈 1.6 的渐变环），
// 和文字隔 10；标题 12 号半粗，下面一行 11 号，隔 2；星光 9，和文字隔 4；无痕标记和标题
// 隔 6；「停止」和文字隔 10。卡片之间隔 6，离内容区右边 14。
constexpr int kCardRadius = 16;
constexpr int kLeading = 9;
constexpr int kTrailing = 6;
constexpr int kVertical = 6;
constexpr int kFavicon = 16;
constexpr int kIconBox = kFavicon + 2 * 4;
constexpr float kRingStroke = 1.6f;
constexpr int kGap = 10;
constexpr int kLineGap = 2;
constexpr int kSparkle = 9;
constexpr int kSparkleGap = 4;
constexpr int kTagGap = 6;
constexpr int kSpacing = 6;
constexpr int kRight = 14;
// 「还有 N 个标签页」：高 24，左右 10，圆角 12，11 号中等字重。
constexpr int kMoreHeight = 24;
constexpr int kMorePadding = 10;
constexpr int kMoreRadius = 12;
// 无痕标记：10 号半粗，左右 6，上下 1.5，图标和字隔 3。
constexpr int kTagPadding = 6;
constexpr int kTagIcon = 9;
constexpr int kTagIconGap = 3;
// 标题、这一步最多这么多字。
constexpr size_t kTitleLimit = 30;
constexpr size_t kActionLimit = 36;
// 悬停时以右边为中心放大一点。
constexpr float kHoverScale = 1.015f;
constexpr double kFade = 0.2;

const gfx::FontList& TitleFont() {
  return UiFont(12, gfx::Font::Weight::SEMIBOLD);
}

const gfx::FontList& LineFont() {
  return UiFont(11);
}

const gfx::FontList& TagFont() {
  return UiFont(10, gfx::Font::Weight::SEMIBOLD);
}

const gfx::FontList& MoreFont() {
  return UiFont(11, gfx::Font::Weight::MEDIUM);
}

constexpr int kTextFlags = gfx::Canvas::NO_SUBPIXEL_RENDERING;

// 去掉两头的空白，太长的截断加「…」。
std::u16string Short(const std::u16string& text, size_t limit) {
  std::u16string trimmed;
  base::TrimWhitespace(text, base::TRIM_ALL, &trimmed);
  if (trimmed.size() <= limit) {
    return trimmed;
  }
  return trimmed.substr(0, limit - 1) + u"…";
}

std::u16string StatusLine(const AgentBackgroundTasks::Entry& entry) {
  return L(
      "agent.status.accessibility",
      {{"client", entry.client}, {"action", Short(entry.line, kActionLimit)}});
}

int TagWidth() {
  return kTagPadding + kTagIcon + kTagIconGap +
         gfx::GetStringWidth(L("agent.background.private"), TagFont()) +
         kTagPadding;
}

int TextHeight() {
  return TitleFont().GetHeight() + kLineGap + LineFont().GetHeight();
}

GlassStyle StyleFor(const Theme& theme) {
  GlassStyle style;
  style.dark = theme.is_dark;
  style.luma = theme.is_dark ? 0.1f : 0.9f;
  return style;
}

// 点卡片：切到那个标签。智能体的无痕窗口没显示时拿出来并放到前面。
void ShowTab(tabs::TabHandle handle, bool is_private) {
  tabs::TabInterface* tab = handle.Get();
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser) {
    return;
  }
  TabStripModel* model = browser->GetTabStripModel();
  const int index = model ? model->GetIndexOfTab(tab) : TabStripModel::kNoTab;
  if (index != TabStripModel::kNoTab) {
    model->ActivateTabAt(index);
  }
  if (ui::BaseWindow* window = is_private ? browser->GetWindow() : nullptr) {
    if (window->IsMinimized()) {
      window->Restore();
    }
    window->Show();
    window->Activate();
  }
  // 别的窗口里的小卡片：这个无痕标签用户看得到了。
  agent::AgentActivities::Get()->NotifySoon();
}

}  // namespace

// 卡片上能点的部分（除了「停止」都是）：底、图标和环、标题、无痕标记、这一步。
class CardButton : public views::Button {
  METADATA_HEADER(CardButton, views::Button)

 public:
  CardButton(PressedCallback callback,
             base::RepeatingCallback<void(bool)> hovered)
      : views::Button(std::move(callback)), hovered_(std::move(hovered)) {}

  void Set(const AgentBackgroundTasks::Entry& entry, const Theme& theme) {
    entry_ = entry;
    theme_ = theme;
    SchedulePaint();
  }

  // views::Button:
  void StateChanged(ButtonState old_state) override {
    views::Button::StateChanged(old_state);
    const bool hovered =
        GetState() == STATE_HOVERED || GetState() == STATE_PRESSED;
    if (hovered != hovered_state_) {
      hovered_state_ = hovered;
      hovered_.Run(hovered);
    }
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    PaintAgentGlassBody(canvas, theme_, bounds, kCardRadius, entry_.working);
    // 图标和外面一圈渐变环（做事时 1，等下一步时 0.6）。
    const gfx::Rect box(kLeading, (height() - kIconBox) / 2, kIconBox,
                        kIconBox);
    const gfx::PointF center(box.CenterPoint());
    cc::PaintFlags ring = AgentRingGradient(center, 0.7f);
    ring.setStyle(cc::PaintFlags::kStroke_Style);
    ring.setStrokeWidth(kRingStroke);
    ring.setAlphaf(entry_.working ? 1.0f : 0.6f);
    canvas->DrawCircle(center, kIconBox / 2.0f - kRingStroke / 2, ring);
    if (!entry_.favicon.IsEmpty() && GetColorProvider()) {
      const gfx::ImageSkia image = entry_.favicon.Rasterize(GetColorProvider());
      canvas->DrawImageInt(image, 0, 0, image.width(), image.height(),
                           box.x() + (kIconBox - kFavicon) / 2,
                           box.y() + (kIconBox - kFavicon) / 2, kFavicon,
                           kFavicon, true);
    }
    // 标题和无痕标记、下面一行（星光是子视图）。
    const int x = box.right() + kGap;
    const int top = (height() - TextHeight()) / 2;
    const std::u16string title = Short(entry_.title, kTitleLimit);
    const int title_width = std::min(
        gfx::GetStringWidth(title, TitleFont()) + 2,
        std::max(0, text_right_ - x -
                        (entry_.is_private ? kTagGap + TagWidth() : 0)));
    canvas->DrawStringRectWithFlags(
        title, TitleFont(), theme_.on_surface,
        gfx::Rect(x, top, title_width, TitleFont().GetHeight()), kTextFlags);
    if (entry_.is_private) {
      PaintTag(canvas, gfx::Point(x + title_width + kTagGap,
                                  top + TitleFont().GetHeight() / 2));
    }
    const int line_x = x + kSparkle + kSparkleGap;
    canvas->DrawStringRectWithFlags(
        StatusLine(entry_), LineFont(), theme_.on_surface_variant,
        gfx::Rect(line_x, top + TitleFont().GetHeight() + kLineGap,
                  std::max(0, text_right_ - line_x), LineFont().GetHeight()),
        kTextFlags);
  }

  // 文字最多画到哪里（「停止」左边隔 10）。
  void set_text_right(int right) { text_right_ = right; }

 private:
  // 「无痕」小标记：无痕窗口的紫色，放在任何主题的卡片上都认得出。
  // center_y 是标记的竖直中心。
  void PaintTag(gfx::Canvas* canvas, const gfx::Point& start) {
    const SkColor accent = Theme::PrivateBrowsing().accent;
    const int height = TagFont().GetHeight() + 3;
    const gfx::RectF tag(start.x(), start.y() - height / 2.0f, TagWidth(),
                         height);
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(accent, 0.16f * 255));
    canvas->DrawRoundRect(tag, height / 2.0f, fill);
    canvas->Save();
    canvas->Translate(
        gfx::Vector2d(static_cast<int>(tag.x()) + kTagPadding,
                      static_cast<int>(tag.CenterPoint().y()) - kTagIcon / 2));
    gfx::PaintVectorIcon(canvas, icons::kEyeSlash, kTagIcon, accent);
    canvas->Restore();
    const int text_x =
        static_cast<int>(tag.x()) + kTagPadding + kTagIcon + kTagIconGap;
    canvas->DrawStringRectWithFlags(
        L("agent.background.private"), TagFont(), accent,
        gfx::Rect(text_x, static_cast<int>(tag.y()),
                  static_cast<int>(tag.right()) - text_x,
                  static_cast<int>(tag.height())),
        kTextFlags);
  }

  AgentBackgroundTasks::Entry entry_;
  Theme theme_ = Theme::DarkBase();
  base::RepeatingCallback<void(bool)> hovered_;
  bool hovered_state_ = false;
  int text_right_ = 0;
};

BEGIN_METADATA(CardButton)
END_METADATA

// 一张卡片：玻璃，上面是能点的 CardButton、星光和「停止」。视图比玻璃大一圈（阴影），
// 只有玻璃接鼠标。
class AgentBackgroundTasks::Card : public GlassView,
                                   public views::ViewTargeterDelegate {
  METADATA_HEADER(Card, GlassView)

 public:
  explicit Card(const Entry& entry) : entry_(entry) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetCornerRadius(kCardRadius, true);
    button_ = AddChildView(std::make_unique<CardButton>(
        base::BindRepeating(
            [](Card* card) {
              // 切过去以后这张卡片会被拿掉：不在它自己的点击回调里做。
              base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                  FROM_HERE, base::BindOnce(&ShowTab, card->entry_.tab,
                                            card->entry_.is_private));
            },
            base::Unretained(this)),
        base::BindRepeating(&Card::Hover, base::Unretained(this))));
    sparkle_ = button_->AddChildView(std::make_unique<AgentSparkleView>());
    stop_ = button_->AddChildView(
        std::make_unique<AgentStopButton>(base::BindRepeating(
            [](Card* card) {
              agent::AgentActivities::Get()->Stop(card->entry_.tab.Get());
            },
            base::Unretained(this))));
  }

  const tabs::TabHandle& tab() const { return entry_.tab; }
  const Entry& entry() const { return entry_; }
  LayerMotion& motion() { return motion_; }
  bool leaving() const { return leaving_; }
  void set_leaving() {
    leaving_ = true;
    SetCanProcessEventsWithinSubtree(false);
  }
  bool placed() const { return placed_; }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SetStyle(StyleFor(theme));
    stop_->SetTheme(theme);
    button_->Set(entry_, theme_);
  }

  void SetEntry(const Entry& entry) {
    entry_ = entry;
    button_->Set(entry_, theme_);
    sparkle_->SetPulsing(entry.working);
    button_->GetViewAccessibility().SetName(
        L(entry.is_private ? "agent.background.privateAccessibility"
                           : "agent.background.accessibility",
          {{"client", entry.client},
           {"tab", entry.title},
           {"action", entry.line}}));
    button_->SetTooltipText(L(entry.is_private
                                  ? "agent.background.showPrivate.help"
                                  : "agent.background.show.help"));
  }

  // 玻璃的大小：按内容宽度自适应。
  gfx::Size GlassSize() const {
    const int title =
        gfx::GetStringWidth(Short(entry_.title, kTitleLimit), TitleFont()) + 2 +
        (entry_.is_private ? kTagGap + TagWidth() : 0);
    const int line = kSparkle + kSparkleGap +
                     gfx::GetStringWidth(StatusLine(entry_), LineFont()) + 2;
    const int width = kLeading + kIconBox + kGap + std::max(title, line) +
                      kGap + AgentStopButton::PreferredWidth() + kTrailing;
    const int height = 2 * kVertical + std::max({kIconBox, TextHeight(),
                                                 AgentStopButton::kHeight});
    return gfx::Size(width, height);
  }

  // 摆在 glass（父视图的坐标）。第一次从右边滑进来、淡入；之后位置变了从原来的地方挪过去。
  void Place(const gfx::Rect& glass) {
    const gfx::Rect old = glass_bounds();
    SetGlassBounds(glass);
    const gfx::Rect inside = glass_rect();
    button_->SetBoundsRect(inside);
    const int stop_width = AgentStopButton::PreferredWidth();
    stop_->SetBounds(inside.width() - kTrailing - stop_width,
                     (inside.height() - AgentStopButton::kHeight) / 2,
                     stop_width, AgentStopButton::kHeight);
    button_->set_text_right(stop_->x() - kGap);
    const int text_x = kLeading + kIconBox + kGap;
    const int line_y = (inside.height() - TextHeight()) / 2 +
                       TitleFont().GetHeight() + kLineGap;
    sparkle_->SetBounds(text_x,
                        line_y + (LineFont().GetHeight() - kSparkle) / 2,
                        kSparkle, kSparkle);
    const MotionCurve curve = MotionCurve::Of(motion::kAgentBackground);
    if (!placed_) {
      placed_ = true;
      // 从右边滑进来并淡入。
      if (!motion::ReduceMotion()) {
        LayerPose from;
        from.offset = gfx::Vector2dF(glass.width() + kRight, 0);
        motion_.AnimateTransformFrom(layer(), from, LayerPose(), curve);
      }
      motion_.AnimateOpacityFrom(layer(), 0, 1, curve);
      return;
    }
    if (old.origin() != glass.origin() && !motion::ReduceMotion()) {
      motion_.ShiftAndAnimate(layer(),
                              gfx::Vector2dF(old.origin() - glass.origin()),
                              LayerPose(), curve);
    }
  }

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override {
    return glass_rect().Intersects(rect);
  }

 private:
  void Hover(bool hovered) {
    if (leaving_ || motion::ReduceMotion()) {
      return;
    }
    LayerPose pose;
    pose.scale = hovered ? kHoverScale : 1;
    const gfx::Rect glass = glass_rect();
    motion_.AnimateTransform(
        layer(), pose, motion::Hover(),
        gfx::PointF(glass.right(), glass.CenterPoint().y()));
  }

  Entry entry_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<CardButton> button_ = nullptr;
  raw_ptr<AgentSparkleView> sparkle_ = nullptr;
  raw_ptr<AgentStopButton> stop_ = nullptr;
  LayerMotion motion_;
  bool placed_ = false;
  bool leaving_ = false;
};

BEGIN_METADATA(AgentBackgroundTasks, Card)
END_METADATA

// 「还有 N 个标签页」：玻璃上的小胶囊，不接鼠标。
class AgentBackgroundTasks::More : public GlassView {
  METADATA_HEADER(More, GlassView)

 public:
  More() {
    SetCornerRadius(kMoreRadius, true);
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SetStyle(StyleFor(theme));
    SchedulePaint();
  }

  void SetCount(int count) {
    if (count_ != count) {
      count_ = count;
      SchedulePaint();
    }
  }
  int count() const { return count_; }
  std::u16string Text() const {
    return L("agent.background.more",
             {{"count", base::NumberToString16(count_)}});
  }
  int GlassWidth() const {
    return gfx::GetStringWidth(Text(), MoreFont()) + 2 + 2 * kMorePadding;
  }
  LayerMotion& motion() { return motion_; }
  bool shown() const { return shown_; }

  void Show(const gfx::Rect& glass) {
    const gfx::Rect old = glass_bounds();
    SetGlassBounds(glass);
    if (!shown_) {
      shown_ = true;
      SetVisible(true);
      motion_.SetTransform(layer(), LayerPose());
      motion_.AnimateOpacity(layer(), 1, MotionCurve::EaseOut(kFade));
      return;
    }
    if (old.origin() != glass.origin() && !motion::ReduceMotion()) {
      motion_.ShiftAndAnimate(
          layer(), gfx::Vector2dF(old.origin() - glass.origin()), LayerPose(),
          MotionCurve::Of(motion::kAgentBackground));
    }
  }

  void Hide() {
    if (!shown_) {
      return;
    }
    shown_ = false;
    motion_.AnimateOpacity(layer(), 0, MotionCurve::EaseOut(kFade));
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    GlassView::OnPaint(canvas);
    const gfx::RectF glass(glass_rect());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(theme_.surface_panel,
                              (theme_.is_dark ? 0.66f : 0.76f) * 255));
    canvas->DrawRoundRect(glass, kMoreRadius, fill);
    gfx::RectF stroke = glass;
    stroke.Inset(0.5f);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    canvas->DrawRoundRect(stroke, kMoreRadius - 0.5f, border);
    canvas->DrawStringRectWithFlags(
        Text(), MoreFont(), theme_.on_surface, glass_rect(),
        kTextFlags | gfx::Canvas::TEXT_ALIGN_CENTER);
  }

 private:
  Theme theme_ = Theme::DarkBase();
  int count_ = 0;
  bool shown_ = false;
  LayerMotion motion_;
};

BEGIN_METADATA(AgentBackgroundTasks, More)
END_METADATA

AgentBackgroundTasks::AgentBackgroundTasks() {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  more_ = AddChildView(std::make_unique<More>());
  more_->layer()->SetOpacity(0);
  more_->SetVisible(false);
}

AgentBackgroundTasks::~AgentBackgroundTasks() = default;

void AgentBackgroundTasks::SetTheme(const Theme& theme) {
  theme_ = theme;
  for (views::View* child : children()) {
    if (Card* card = views::AsViewClass<Card>(child)) {
      card->SetTheme(theme);
    }
  }
  more_->SetTheme(theme);
}

void AgentBackgroundTasks::Update(const std::vector<Entry>& entries,
                                  const gfx::Rect& area,
                                  int bottom) {
  const size_t shown = std::min<size_t>(entries.size(), 3);
  std::vector<raw_ptr<Card>> next;
  for (size_t i = 0; i < shown; ++i) {
    const Entry& entry = entries[i];
    auto it = std::ranges::find_if(
        cards_, [&](Card* card) { return card->tab() == entry.tab; });
    Card* card = nullptr;
    if (it != cards_.end()) {
      card = *it;
    } else {
      card = AddChildViewAt(std::make_unique<Card>(entry), 0);
      card->SetTheme(theme_);
    }
    card->SetEntry(entry);
    next.push_back(card);
  }
  for (Card* card : cards_) {
    if (std::ranges::find(next, card) == next.end()) {
      LeaveCard(card);
    }
  }
  cards_ = std::move(next);

  // 从下往上摆：最下面是「还有 N 个」，上面是卡片（第一张在最上面），都靠右。
  const int right = area.right() - kRight;
  int y = area.bottom() - bottom;
  if (entries.size() > shown) {
    more_->SetCount(static_cast<int>(entries.size() - shown));
    const int width = more_->GlassWidth();
    more_->Show(gfx::Rect(right - width, y - kMoreHeight, width, kMoreHeight));
    y -= kMoreHeight + kSpacing;
  } else {
    more_->Hide();
  }
  for (auto it = cards_.rbegin(); it != cards_.rend(); ++it) {
    const gfx::Size size = (*it)->GlassSize();
    (*it)->Place(gfx::Rect(right - size.width(), y - size.height(),
                           size.width(), size.height()));
    y -= size.height() + kSpacing;
  }
}

void AgentBackgroundTasks::LeaveCard(Card* card) {
  card->set_leaving();
  const MotionCurve curve = MotionCurve::Of(motion::kAgentBackground);
  if (!motion::ReduceMotion()) {
    LayerPose to;
    to.offset = gfx::Vector2dF(card->glass_bounds().width() + kRight, 0);
    card->motion().AnimateTransform(card->layer(), to, curve);
  }
  card->motion().AnimateOpacity(
      card->layer(), 0, MotionCurve::EaseOut(kFade),
      base::BindOnce(&AgentBackgroundTasks::RemoveLater,
                     weak_factory_.GetWeakPtr(), base::Unretained(card)));
}

void AgentBackgroundTasks::RemoveLater(views::View* view) {
  // 不在它自己的动画回调里删它。
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<AgentBackgroundTasks> tasks, views::View* view) {
            if (tasks && tasks->Contains(view) &&
                std::ranges::find(tasks->cards_, view) == tasks->cards_.end()) {
              tasks->RemoveChildViewT(view);
            }
          },
          weak_factory_.GetWeakPtr(), base::Unretained(view)));
}

bool AgentBackgroundTasks::HitsCard(const gfx::Rect& rect) const {
  for (const Card* card : cards_) {
    if (!card->leaving() && card->glass_bounds().Intersects(rect)) {
      return true;
    }
  }
  return false;
}

std::vector<gfx::Rect> AgentBackgroundTasks::CardRects() const {
  std::vector<gfx::Rect> rects;
  for (const Card* card : cards_) {
    if (!card->leaving()) {
      rects.push_back(card->glass_bounds());
    }
  }
  return rects;
}

bool AgentBackgroundTasks::DoesIntersectRect(const views::View* target,
                                             const gfx::Rect& rect) const {
  return HitsCard(rect);
}

std::string AgentBackgroundTasks::DebugDescription() const {
  std::string text;
  for (const Card* card : cards_) {
    const Entry& entry = card->entry();
    base::StrAppend(
        &text,
        {"task[t", base::NumberToString(entry.tab.raw_value()), " \"",
         base::UTF16ToUTF8(entry.title), " | ", base::UTF16ToUTF8(entry.client),
         ": ", base::UTF16ToUTF8(entry.line), "\"",
         entry.working ? " working" : " idle",
         entry.is_private ? " private" : "",
         " glass=", card->glass_bounds().ToString(), "] "});
  }
  if (more_->shown()) {
    base::StrAppend(&text, {"more=", base::NumberToString(more_->count()),
                            " glass=", more_->glass_bounds().ToString(), " "});
  }
  return text.empty() ? std::string("none") : text;
}

BEGIN_METADATA(AgentBackgroundTasks)
END_METADATA

}  // namespace starry
