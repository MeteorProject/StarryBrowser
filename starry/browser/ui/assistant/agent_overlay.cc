// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/agent_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/agent/agent_activity.h"
#include "starry/browser/agent/private_windows.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/assistant/agent_background_tasks.h"
#include "starry/browser/ui/assistant/agent_marks.h"
#include "starry/browser/ui/assistant/agent_parts.h"
#include "starry/browser/ui/assistant/assistant_island.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/motion/spring.h"
#include "starry/browser/ui/motion/value_motion.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/base_window.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_utils.h"

namespace starry {

namespace {

// 提示条：高 36，最宽 520，这一格左右至少留 16，离底边 14。
// 里面左边 13、右边 5；星光 13，和文字隔 10；名字和这一步隔 6；「停止」和文字隔 10。
constexpr int kBarHeight = 36;
constexpr int kBarMaxWidth = 520;
constexpr int kBarSide = 16;
constexpr int kBarBottom = 14;
constexpr int kLeading = 13;
constexpr int kTrailing = 5;
constexpr int kSparkleSide = 13;
constexpr int kGap = 10;
constexpr int kTextGap = 6;
// 岛悬浮着开着时盖住这一格底边多高（一条输入框 58 + 离底边 16）：
// 外部智能体的提示条、后台的小卡片放在它上面。
constexpr int kIslandCover = 74;
// 后台的小卡片离内容区底边 14；显示着的标签里也有智能体（底部正中是它的提示条）时 64。
constexpr int kTasksBottom = 14;
constexpr int kTasksRaised = 64;
// 换字时交叉淡化的时长。
constexpr double kFade = 0.2;

const gfx::FontList& ClientFont() {
  return UiFont(12, gfx::Font::Weight::SEMIBOLD);
}

const gfx::FontList& LineFont() {
  return UiFont(12);
}

constexpr int kTextFlags = gfx::Canvas::NO_SUBPIXEL_RENDERING;

// 调试输出里的标签（同 browser_tabs 的短 ID）。
std::string TabIdOf(const tabs::TabHandle& tab) {
  return base::StrCat({"t", base::NumberToString(tab.raw_value())});
}

// 这一步的说明：自己一个图层，换字时新旧两个交叉淡化。
class AgentStatusLine : public views::View {
  METADATA_HEADER(AgentStatusLine, views::View)

 public:
  AgentStatusLine() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  const std::u16string& text() const { return text_; }
  LayerMotion& motion() { return motion_; }

  void SetText(const std::u16string& text) {
    if (text_ != text) {
      text_ = text;
      SchedulePaint();
    }
  }

  void SetColor(SkColor color) {
    if (color_ != color) {
      color_ = color;
      SchedulePaint();
    }
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    canvas->DrawStringRectWithFlags(text_, LineFont(), color_, GetLocalBounds(),
                                    kTextFlags);
  }

 private:
  std::u16string text_;
  SkColor color_ = SK_ColorGRAY;
  LayerMotion motion_;
};

BEGIN_METADATA(AgentStatusLine)
END_METADATA

// 玻璃上面画的底和名字；星光、说明、「停止」是它的子视图。
class AgentStatusBody : public views::View {
  METADATA_HEADER(AgentStatusBody, views::View)

 public:
  AgentStatusBody() = default;

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void SetClient(const std::u16string& client, const gfx::Rect& rect) {
    client_ = client;
    client_rect_ = rect;
    SchedulePaint();
  }

  void SetWorking(bool working) {
    if (working_ != working) {
      working_ = working;
      SchedulePaint();
    }
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    PaintAgentGlassBody(canvas, theme_, bounds, bounds.height() / 2, working_);
    canvas->DrawStringRectWithFlags(client_, ClientFont(), theme_.on_surface,
                                    client_rect_, kTextFlags);
  }

 private:
  Theme theme_ = Theme::DarkBase();
  std::u16string client_;
  gfx::Rect client_rect_;
  bool working_ = false;
};

BEGIN_METADATA(AgentStatusBody)
END_METADATA

}  // namespace

// 网页底部的提示条：玻璃（GlassView）加上面画的东西。视图比玻璃大一圈（阴影），只有玻璃
// 本身接鼠标。
class AgentStatusBar : public GlassView, public views::ViewTargeterDelegate {
  METADATA_HEADER(AgentStatusBar, GlassView)

 public:
  explicit AgentStatusBar(tabs::TabHandle tab)
      : tab_(tab),
        width_(0,
               base::BindRepeating(&AgentStatusBar::Relayout,
                                   base::Unretained(this))) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
    SetCornerRadius(kBarHeight / 2.0f, true);
    GetViewAccessibility().SetRole(ax::mojom::Role::kStatus);
    body_ = AddChildView(std::make_unique<AgentStatusBody>());
    sparkle_ = body_->AddChildView(std::make_unique<AgentSparkleView>());
    for (raw_ptr<AgentStatusLine>& line : lines_) {
      line = body_->AddChildView(std::make_unique<AgentStatusLine>());
    }
    lines_[1]->layer()->SetOpacity(0);
    stop_ = body_->AddChildView(
        std::make_unique<AgentStopButton>(base::BindRepeating(
            [](tabs::TabHandle tab) {
              agent::AgentActivities::Get()->Stop(tab.Get());
            },
            tab)));
  }

  const tabs::TabHandle& tab() const { return tab_; }
  bool leaving() const { return leaving_; }
  void set_leaving() {
    leaving_ = true;
    SetCanProcessEventsWithinSubtree(false);
  }
  LayerMotion& motion() { return motion_; }
  int drop() const { return drop_; }
  const std::u16string& client() const { return client_; }
  const std::u16string& line() const { return lines_[current_]->text(); }
  bool working() const { return working_; }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    body_->SetTheme(theme);
    for (AgentStatusLine* line : lines_) {
      line->SetColor(theme.on_surface_variant);
    }
    stop_->SetTheme(theme);
  }

  void SetContent(const std::u16string& client,
                  const std::u16string& text,
                  bool working) {
    working_ = working;
    client_ = client;
    body_->SetWorking(working);
    sparkle_->SetPulsing(working);
    if (text != lines_[current_]->text()) {
      if (!placed_ || motion::ReduceMotion()) {
        lines_[current_]->SetText(text);
      } else {
        // 换字：旧的淡出，新的淡入。
        AgentStatusLine* old = lines_[current_];
        current_ = 1 - current_;
        AgentStatusLine* now = lines_[current_];
        now->SetText(text);
        old->motion().AnimateOpacity(old->layer(), 0,
                                     MotionCurve::EaseOut(kFade));
        now->motion().AnimateOpacityFrom(now->layer(), 0, 1,
                                         MotionCurve::EaseOut(kFade));
      }
    }
    GetViewAccessibility().SetName(L("agent.status.accessibility",
                                     {{"client", client}, {"action", text}}));
    Relayout();
  }

  // 摆在 center_x、下边在 bottom，最宽 room。第一次摆时从这一格的底边下面升上来（drop 是
  // 升多高）。covered（岛盖着底边）变了时上下挪动是弹簧，别的变化（窗口变大小、侧栏在动）
  // 直接跟上。
  void Place(int center_x, int bottom, int room, int drop, bool covered) {
    const int target = std::min(PreferredWidth(), std::max(0, room));
    const gfx::Rect old = glass_bounds();
    const bool moved_by_island = placed_ && covered != covered_;
    const MotionCurve curve = MotionCurve::Of(motion::kAgentStatus);
    center_x_ = center_x;
    bottom_ = bottom;
    room_ = room;
    drop_ = drop;
    covered_ = covered;
    if (!placed_ || motion::ReduceMotion()) {
      width_.SetValue(target);
    } else if (width_.target() != target) {
      width_.AnimateTo(target, curve);
    }
    Relayout();
    if (!placed_) {
      placed_ = true;
      // 从底边下面升上来并淡入。
      if (!motion::ReduceMotion()) {
        LayerPose from;
        from.offset = gfx::Vector2dF(0, drop);
        motion_.AnimateTransformFrom(layer(), from, LayerPose(), curve);
      }
      motion_.AnimateOpacityFrom(layer(), 0, 1, MotionCurve::EaseOut(kFade));
      return;
    }
    if (moved_by_island && !motion::ReduceMotion() &&
        old.y() != glass_bounds().y()) {
      motion_.ShiftAndAnimate(layer(),
                              gfx::Vector2dF(0, old.y() - glass_bounds().y()),
                              LayerPose(), curve);
    }
  }

  // 「停止」在 target 的坐标里。
  gfx::Rect StopBoundsIn(const views::View* target) const {
    return views::View::ConvertRectToTarget(stop_, target,
                                            stop_->GetLocalBounds());
  }

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override {
    return glass_rect().Intersects(rect);
  }

 private:
  int PreferredWidth() const {
    return kLeading + kSparkleSide + kGap +
           gfx::GetStringWidth(client_, ClientFont()) + 2 + kTextGap +
           gfx::GetStringWidth(lines_[current_]->text(), LineFont()) + 2 +
           kGap + AgentStopButton::PreferredWidth() + kTrailing;
  }

  void Relayout() {
    if (!placed_ && center_x_ == 0 && bottom_ == 0) {
      return;
    }
    const int width = static_cast<int>(std::lround(width_.value()));
    SetGlassBounds(gfx::Rect(center_x_ - width / 2, bottom_ - kBarHeight, width,
                             kBarHeight));
    body_->SetBoundsRect(glass_rect());
    sparkle_->SetBounds(kLeading, (kBarHeight - kSparkleSide) / 2, kSparkleSide,
                        kSparkleSide);
    const int stop_width = AgentStopButton::PreferredWidth();
    stop_->SetBounds(width - kTrailing - stop_width,
                     (kBarHeight - AgentStopButton::kHeight) / 2, stop_width,
                     AgentStopButton::kHeight);
    // 名字优先完整显示，剩下的宽度给这一步。
    const int left = kLeading + kSparkleSide + kGap;
    const int right = std::max(left, stop_->x() - kGap);
    const int client_width =
        std::min(gfx::GetStringWidth(client_, ClientFont()) + 2, right - left);
    body_->SetClient(client_, gfx::Rect(left, 0, client_width, kBarHeight));
    const int line_x = std::min(right, left + client_width + kTextGap);
    for (AgentStatusLine* line : lines_) {
      line->SetBounds(line_x, 0, right - line_x, kBarHeight);
    }
  }

  const tabs::TabHandle tab_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<AgentStatusBody> body_ = nullptr;
  raw_ptr<AgentSparkleView> sparkle_ = nullptr;
  std::array<raw_ptr<AgentStatusLine>, 2> lines_;
  int current_ = 0;
  raw_ptr<AgentStopButton> stop_ = nullptr;
  std::u16string client_;
  bool working_ = false;
  bool placed_ = false;
  bool leaving_ = false;
  bool covered_ = false;
  int center_x_ = 0;
  int bottom_ = 0;
  int room_ = 0;
  int drop_ = 0;
  ValueMotion width_;
  LayerMotion motion_;
};

BEGIN_METADATA(AgentStatusBar)
END_METADATA

AgentOverlay::AgentOverlay(StarryBrowserView* browser_view)
    : browser_view_(browser_view) {
  SetPaintToLayer(ui::LAYER_NOT_DRAWN);
  // 只有提示条和后台的小卡片接鼠标（DoesIntersectRect）。
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  tasks_ = AddChildView(std::make_unique<AgentBackgroundTasks>());
  activities_ = agent::AgentActivities::Get()->AddObserver(
      base::BindRepeating(&AgentOverlay::Update, base::Unretained(this)));
}

AgentOverlay::~AgentOverlay() = default;

void AgentOverlay::SetTheme(const Theme& theme) {
  theme_ = theme;
  for (views::View* child : children()) {
    if (AgentStatusBar* bar = views::AsViewClass<AgentStatusBar>(child)) {
      bar->SetTheme(theme);
    }
  }
  tasks_->SetTheme(theme);
}

size_t AgentOverlay::TasksIndex() const {
  return GetIndexOf(tasks_).value_or(children().size());
}

void AgentOverlay::Update() {
  agent::AgentActivities* activities = agent::AgentActivities::Get();
  MultiContentsView* contents_view =
      GetWidget() ? browser_view_->multi_contents_view() : nullptr;
  std::vector<ContentsContainerView*> containers;
  if (contents_view) {
    if (contents_view->IsInSplitView()) {
      for (ContentsContainerView* container :
           contents_view->contents_container_views()) {
        containers.push_back(container);
      }
    } else {
      containers.push_back(contents_view->GetActiveContentsContainerView());
    }
  }
  struct Shown {
    Pane pane;
    raw_ptr<tabs::TabInterface> tab;
    raw_ptr<const agent::AgentActivity> activity;
  };
  std::vector<Shown> shown;
  // 显示着的标签（有没有智能体都算）和整个内容区（几格合起来）。
  std::set<const tabs::TabInterface*> displayed;
  gfx::Rect area;
  int content_bottom = 0;
  for (ContentsContainerView* container : containers) {
    if (!container || !container->GetVisible() || !container->parent()) {
      continue;
    }
    const gfx::Rect card = views::View::ConvertRectToTarget(
        container->parent(), this, container->bounds());
    content_bottom = std::max(content_bottom, card.bottom());
    area.Union(card);
    content::WebContents* web_contents =
        container->contents_view()->web_contents();
    tabs::TabInterface* tab =
        web_contents ? tabs::TabInterface::MaybeGetFromContents(web_contents)
                     : nullptr;
    if (tab) {
      displayed.insert(tab);
    }
    const agent::AgentActivity* activity = tab ? activities->For(tab) : nullptr;
    if (!activity) {
      continue;
    }
    shown.push_back(
        {Pane{tab->GetHandle(), card, activity->working()}, tab, activity});
  }

  AssistantIsland* island = browser_view_->assistant_island();
  const bool island_over_page =
      island && island->is_open() && !island->docked();
  std::vector<raw_ptr<AgentStatusBar>> next;
  std::map<tabs::TabHandle, raw_ptr<AgentPaneMarks>> next_marks;
  std::vector<Pane> panes;
  for (const Shown& each : shown) {
    panes.push_back(each.pane);
    const gfx::Rect& card = each.pane.card;
    content::WebContents* contents = each.tab->GetContents();

    // 描边、光标、高亮框、扫描光带：网页的坐标（主文档视口的 CSS 像素）从哪里开始、
    // 放大多少。
    auto found = marks_.find(each.pane.tab);
    AgentPaneMarks* marks = nullptr;
    if (found != marks_.end()) {
      marks = found->second;
    } else {
      marks = AddChildViewAt(std::make_unique<AgentPaneMarks>(), 0);
    }
    const float zoom = static_cast<float>(blink::ZoomLevelToZoomFactor(
        content::HostZoomMap::GetZoomLevel(contents)));
    gfx::Point page_origin = contents->GetContainerBounds().origin();
    views::View::ConvertPointFromScreen(this, &page_origin);
    marks->Update(*each.activity, card,
                  gfx::PointF(page_origin - card.OffsetFromOrigin()), zoom);
    next_marks[each.pane.tab] = marks;

    // 岛悬浮着、盖着这一格的底边：内置助手的不显示（岛自己说），外部智能体的放在岛上面。
    const bool covered =
        island_over_page && card.bottom() >= content_bottom - 1;
    if (covered && each.activity->assistant) {
      continue;
    }
    auto it = std::ranges::find_if(bars_, [&](AgentStatusBar* bar) {
      return bar->tab() == each.pane.tab;
    });
    AgentStatusBar* bar = nullptr;
    if (it != bars_.end()) {
      bar = *it;
    } else {
      bar = AddChildViewAt(std::make_unique<AgentStatusBar>(each.pane.tab),
                           TasksIndex());
      bar->SetTheme(theme_);
    }
    bar->SetContent(each.activity->client, each.activity->Line(contents),
                    each.activity->working());
    const int bottom =
        card.bottom() - kBarBottom - (covered ? kIslandCover : 0);
    bar->Place(card.CenterPoint().x(), bottom,
               std::min(kBarMaxWidth, card.width() - 2 * kBarSide),
               card.bottom() - (bottom - kBarHeight), covered);
    next.push_back(bar);
  }
  for (AgentStatusBar* bar : bars_) {
    if (std::ranges::find(next, bar) == next.end()) {
      Leave(bar);
    }
  }
  bars_ = std::move(next);
  for (auto& [tab, marks] : marks_) {
    if (!next_marks.contains(tab)) {
      marks->Leave(base::BindOnce(&AgentOverlay::Remove,
                                  weak_factory_.GetWeakPtr(),
                                  base::Unretained(marks.get())));
    }
  }
  marks_ = std::move(next_marks);
  panes_ = std::move(panes);

  // 后台的小卡片：这个窗口里被操作、没显示着的标签，加上智能体的临时无痕标签（用户没在
  // 看那个无痕窗口时；智能体自己的无痕窗口里不列）。岛悬浮着开着时，内置助手的后台任务是
  // 岛上的胶囊，这里只列外部智能体的。
  std::vector<AgentBackgroundTasks::Entry> entries;
  BrowserWindowInterface* own = browser_view_->browser();
  Profile* profile = own->GetProfile();
  agent::PrivateWindows* private_windows = agent::PrivateWindows::Get();
  const bool agent_window = private_windows->OwnerName(profile).has_value();
  for (const auto& [tab, activity] : activities->All()) {
    if (displayed.contains(tab) || (island_over_page && activity->assistant)) {
      continue;
    }
    BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
    if (!browser) {
      continue;
    }
    bool is_private = false;
    if (browser != own) {
      Profile* other = browser->GetProfile();
      ui::BaseWindow* window = browser->GetWindow();
      const bool seen = window && window->IsVisible() &&
                        !window->IsMinimized() && tab->IsActivated();
      if (agent_window || seen ||
          !private_windows->OwnerName(other).has_value() ||
          other->GetOriginalProfile() != profile->GetOriginalProfile()) {
        continue;
      }
      is_private = true;
    }
    AgentBackgroundTasks::Entry entry;
    entry.tab = tab->GetHandle();
    const tabs::TabData data = StarryTabData(tab);
    entry.title = TabDisplayTitle(data);
    entry.favicon = data.favicon;
    entry.client = activity->client;
    entry.line = activity->Line(tab->GetContents());
    entry.working = activity->working();
    entry.is_private = is_private;
    entries.push_back(std::move(entry));
  }
  if (area.IsEmpty()) {
    entries.clear();
  }
  tasks_->SetBoundsRect(GetLocalBounds());
  tasks_->Update(entries, area,
                 (shown.empty() ? kTasksBottom : kTasksRaised) +
                     (island_over_page ? kIslandCover : 0));
  browser_view_->MediaObstaclesChanged();
}

void AgentOverlay::Leave(AgentStatusBar* bar) {
  bar->set_leaving();
  if (!motion::ReduceMotion()) {
    LayerPose pose;
    pose.offset = gfx::Vector2dF(0, bar->drop());
    bar->motion().AnimateTransform(bar->layer(), pose,
                                   MotionCurve::Of(motion::kAgentStatus));
  }
  // 放完再拿掉（不在它自己的动画回调里删它）。
  bar->motion().AnimateOpacity(
      bar->layer(), 0, MotionCurve::EaseOut(kFade),
      base::BindOnce(
          [](base::WeakPtr<AgentOverlay> overlay, AgentStatusBar* bar) {
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(&AgentOverlay::Remove, overlay,
                                          base::Unretained(bar)));
          },
          weak_factory_.GetWeakPtr(), base::Unretained(bar)));
}

void AgentOverlay::Remove(views::View* view) {
  if (!Contains(view) || std::ranges::find(bars_, view) != bars_.end()) {
    return;
  }
  for (const auto& [tab, marks] : marks_) {
    if (marks == view) {
      return;
    }
  }
  RemoveChildViewT(view);
}

std::string AgentOverlay::DebugDescription() const {
  std::string text;
  for (const Pane& pane : panes_) {
    auto marks = marks_.find(pane.tab);
    base::StrAppend(
        &text, {"pane[", TabIdOf(pane.tab), pane.working ? " working" : " idle",
                " card=", pane.card.ToString(),
                marks != marks_.end() ? " " + marks->second->DebugDescription()
                                      : std::string(),
                "] "});
  }
  size_t leaving = 0;
  for (const views::View* child : children()) {
    if (const AgentStatusBar* bar = views::AsViewClass<AgentStatusBar>(child);
        bar && bar->leaving()) {
      ++leaving;
    }
  }
  for (const AgentStatusBar* bar : bars_) {
    base::StrAppend(&text, {"bar[", TabIdOf(bar->tab()), " \"",
                            base::UTF16ToUTF8(bar->client()), " | ",
                            base::UTF16ToUTF8(bar->line()), "\"",
                            bar->working() ? " working" : " idle",
                            " glass=", bar->glass_bounds().ToString(), " stop=",
                            bar->StopBoundsIn(this).ToString(), "] "});
  }
  if (leaving) {
    base::StrAppend(&text, {"leaving=", base::NumberToString(leaving), " "});
  }
  const std::string tasks = tasks_->DebugDescription();
  if (tasks != "none") {
    text += tasks;
  }
  return text.empty() ? std::string("none") : text;
}

std::vector<gfx::Rect> AgentOverlay::ObstacleRects() const {
  std::vector<gfx::Rect> rects;
  for (const AgentStatusBar* bar : bars_) {
    if (bar->GetVisible() && !bar->leaving()) {
      rects.push_back(bar->glass_bounds());
    }
  }
  for (const gfx::Rect& rect : tasks_->CardRects()) {
    rects.push_back(rect);
  }
  return rects;
}

bool AgentOverlay::DoesIntersectRect(const views::View* target,
                                     const gfx::Rect& rect) const {
  for (const AgentStatusBar* bar : bars_) {
    if (bar->GetVisible() && !bar->leaving() &&
        bar->glass_bounds().Intersects(rect)) {
      return true;
    }
  }
  return tasks_->HitsCard(rect);
}

BEGIN_METADATA(AgentOverlay)
END_METADATA

}  // namespace starry
