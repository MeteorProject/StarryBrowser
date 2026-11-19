// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/split/split_tabs.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/split_tab_metrics.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/split_tab_data.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"

namespace starry::split {

namespace {

std::optional<split_tabs::SplitTabId> SplitOf(tabs::TabInterface* tab) {
  return tab ? tab->GetSplit() : std::nullopt;
}

void Persist(BrowserWindowInterface* browser, split_tabs::SplitTabId split) {
  TabStripModel* model = browser->GetTabStripModel();
  if (!model->ContainsSplit(split)) {
    return;
  }
  SessionService* session =
      SessionServiceFactory::GetForProfile(browser->GetProfile());
  if (!session) {
    return;
  }
  session->SetSplitTabData(browser->GetSessionID(), split,
                           model->GetSplitData(split)->visual_data());
}

}  // namespace

Edge NearestEdge(const gfx::Point& point, const gfx::Rect& rect) {
  if (rect.IsEmpty()) {
    return Edge::kRight;
  }
  const double x = static_cast<double>(point.x() - rect.x()) / rect.width();
  const double y = static_cast<double>(point.y() - rect.y()) / rect.height();
  const std::array<std::pair<Edge, double>, 4> distances = {{
      {Edge::kRight, 1 - x},
      {Edge::kLeft, x},
      {Edge::kBottom, 1 - y},
      {Edge::kTop, y},
  }};
  return std::ranges::min_element(
             distances, {}, [](const auto& entry) { return entry.second; })
      ->first;
}

bool IsLeading(Edge edge) {
  return edge == Edge::kLeft || edge == Edge::kTop;
}

split_tabs::SplitTabLayout LayoutFor(Edge edge) {
  return edge == Edge::kLeft || edge == Edge::kRight
             ? split_tabs::SplitTabLayout::kSideBySide
             : split_tabs::SplitTabLayout::kStacked;
}

std::u16string PlacementTitle(Edge edge) {
  switch (edge) {
    case Edge::kLeft:
      return L("split.placement.left");
    case Edge::kRight:
      return L("split.placement.right");
    case Edge::kTop:
      return L("split.placement.top");
    case Edge::kBottom:
      return L("split.placement.bottom");
  }
}

const gfx::VectorIcon& PlacementIcon(Edge edge) {
  switch (edge) {
    case Edge::kLeft:
      return icons::kRectLeftHalf;
    case Edge::kRight:
      return icons::kRectRightHalf;
    case Edge::kTop:
      return icons::kRectTopHalf;
    case Edge::kBottom:
      return icons::kRectBottomHalf;
  }
}

Drop Evaluate(TabStripModel* model,
              tabs::TabInterface* dragged,
              tabs::TabInterface* target,
              Edge edge) {
  Drop drop;
  drop.edge = edge;
  if (dragged) {
    drop.dragged = dragged->GetHandle();
  }
  if (target) {
    drop.target = target->GetHandle();
  }
  if (!dragged || !target || dragged == target) {
    return drop;
  }
  if (model->GetIndexOfTab(dragged) == TabStripModel::kNoTab ||
      model->GetIndexOfTab(target) == TabStripModel::kNoTab) {
    drop.refusal = Refusal::kOtherWindow;
    return drop;
  }
  const auto target_split = SplitOf(target);
  Preview preview;
  if (target_split) {
    preview.layout = GetLayout(model, *target_split);
    for (auto* tab : model->GetSplitData(*target_split)->ListTabs()) {
      preview.tabs.push_back(tab->GetHandle());
    }
  } else {
    preview.tabs.push_back(drop.target);
  }
  auto target_it = std::ranges::find(preview.tabs, drop.target);
  int target_id = target_it - preview.tabs.begin();
  auto dragged_it = std::ranges::find(preview.tabs, drop.dragged);
  int dragged_id = dragged_it - preview.tabs.begin();
  if (dragged_it != preview.tabs.end()) {
    preview.layout = *preview.layout.Remove(dragged_id);
  } else {
    if (preview.tabs.size() >= kMaxPanes) {
      drop.refusal = Refusal::kFull;
      return drop;
    }
    preview.tabs.push_back(drop.dragged);
  }
  preview.layout.Insert(
      dragged_id, target_id,
      LayoutFor(edge) == split_tabs::SplitTabLayout::kSideBySide,
      IsLeading(edge));
  std::vector<tabs::TabHandle> ordered;
  for (int id : preview.layout.Panes()) {
    ordered.push_back(preview.tabs[id]);
  }
  preview.tabs = std::move(ordered);
  preview.layout.Renumber();
  drop.preview = preview;
  return drop;
}

namespace {
split_tabs::SplitTabVisualData VisualData(const Layout& layout) {
  split_tabs::SplitTabVisualData data(
      layout.horizontal ? split_tabs::SplitTabLayout::kSideBySide
                        : split_tabs::SplitTabLayout::kStacked,
      layout.fractions[0]);
  data.set_starry_layout(layout.Encode());
  return data;
}

split_tabs::SplitTabId Create(BrowserWindowInterface* browser,
                              const std::vector<tabs::TabHandle>& tabs,
                              const Layout& layout,
                              tabs::TabInterface* pivot) {
  auto* model = browser->GetTabStripModel();
  std::vector<int> indices = {model->GetIndexOfTab(pivot)};
  for (auto handle : tabs) {
    if (handle.Get() != pivot) {
      indices.push_back(model->GetIndexOfTab(handle.Get()));
    }
  }
  const auto split =
      model->AddToNewSplit(indices, VisualData(layout),
                           split_tabs::SplitTabCreatedSource::kDragAndDropTab);
  const int start = model->GetSplitData(split)->GetIndexRange().start();
  for (size_t i = 0; i < tabs.size(); ++i) {
    model->MoveWebContentsAt(model->GetIndexOfTab(tabs[i].Get()), start + i,
                             false);
  }
  Persist(browser, split);
  return split;
}
}  // namespace

bool Apply(BrowserWindowInterface* browser, const Drop& drop) {
  auto* model = browser->GetTabStripModel();
  // A posted drop may outlive its preview. Re-evaluate against current state.
  const Drop current =
      Evaluate(model, drop.dragged.Get(), drop.target.Get(), drop.edge);
  if (!current.preview || current.refusal) {
    return false;
  }
  auto* dragged = drop.dragged.Get();
  auto* target = drop.target.Get();
  auto target_split = target->GetSplit();
  if (dragged->GetSplit() && dragged->GetSplit() != target_split) {
    RemoveFromSplit(browser, dragged);
  }
  if (target_split) {
    model->RemoveSplit(*target_split);
  }
  Create(browser, current.preview->tabs, current.preview->layout, target);
  model->ActivateTabAt(model->GetIndexOfTab(dragged));
  return true;
}

tabs::TabInterface* Partner(TabStripModel* model, tabs::TabInterface* tab) {
  const std::optional<split_tabs::SplitTabId> split = SplitOf(tab);
  if (!split || !model->ContainsSplit(*split)) {
    return nullptr;
  }
  for (tabs::TabInterface* other : model->GetSplitData(*split)->ListTabs()) {
    if (other != tab) {
      return other;
    }
  }
  return nullptr;
}

void RemoveFromSplit(BrowserWindowInterface* browser, tabs::TabInterface* tab) {
  TabStripModel* model = browser->GetTabStripModel();
  const std::optional<split_tabs::SplitTabId> split = SplitOf(tab);
  if (!split || !model->ContainsSplit(*split)) {
    return;
  }
  Layout rest = GetLayout(model, *split);
  const auto members = model->GetSplitData(*split)->ListTabs();
  int removed = std::ranges::find(members, tab) - members.begin();
  rest = *rest.Remove(removed);
  std::vector<tabs::TabHandle> kept;
  for (int id : rest.Panes()) {
    kept.push_back(members[id]->GetHandle());
  }
  rest.Renumber();
  if (tab->IsActivated()) {
    model->ActivateTabAt(model->GetIndexOfTab(kept[0].Get()));
  }
  model->RemoveSplit(*split);
  if (kept.size() >= 2) {
    Create(browser, kept, rest, kept[0].Get());
  }
}

void ShowOnly(BrowserWindowInterface* browser, tabs::TabInterface* tab) {
  TabStripModel* model = browser->GetTabStripModel();
  const std::optional<split_tabs::SplitTabId> split = SplitOf(tab);
  if (!split || !model->ContainsSplit(*split)) {
    return;
  }
  const int index = model->GetIndexOfTab(tab);
  if (index != model->active_index()) {
    model->ActivateTabAt(index);
  }
  model->RemoveSplit(*split);
}

Layout GetLayout(TabStripModel* model, split_tabs::SplitTabId split) {
  const auto* data = model->GetSplitData(split);
  const auto* visual = data->visual_data();
  return Layout::Decode(
      visual->starry_layout(), data->ListTabs().size(),
      visual->split_layout() == split_tabs::SplitTabLayout::kSideBySide,
      visual->split_ratio());
}

void SetLayout(BrowserWindowInterface* browser,
               split_tabs::SplitTabId split,
               const Layout& layout,
               bool done) {
  auto* model = browser->GetTabStripModel();
  if (!model->ContainsSplit(split)) {
    return;
  }
  model->UpdateStarrySplitLayout(split, VisualData(layout), !done);
  if (done) {
    Persist(browser, split);
  }
}

void Equalize(BrowserWindowInterface* browser, split_tabs::SplitTabId split) {
  Layout layout = GetLayout(browser->GetTabStripModel(), split);
  layout.Equalize();
  SetLayout(browser, split, layout, true);
}

void SetRatio(BrowserWindowInterface* browser,
              split_tabs::SplitTabId split,
              double ratio,
              bool done) {
  Layout layout = GetLayout(browser->GetTabStripModel(), split);
  ratio = std::clamp(ratio, 0.001, 0.999);
  const double remaining = 1 - layout.fractions[0];
  for (size_t i = 1; i < layout.fractions.size(); ++i) {
    layout.fractions[i] *= (1 - ratio) / remaining;
  }
  layout.fractions[0] = ratio;
  SetLayout(browser, split, layout, done);
}

void PreserveRemainingPanes(TabStripModel* model,
                            tabs::TabInterface* removing) {
  if (!removing->GetSplit()) {
    return;
  }
  const auto split = *removing->GetSplit();
  const auto tabs = model->GetSplitData(split)->ListTabs();
  if (tabs.size() != kMaxPanes) {
    return;
  }
  const int index = std::ranges::find(tabs, removing) - tabs.begin();
  Layout rest = *GetLayout(model, split).Remove(index);
  std::vector<tabs::TabHandle> kept;
  for (int id : rest.Panes()) {
    kept.push_back(tabs[id]->GetHandle());
  }
  rest.Renumber();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::vector<tabs::TabHandle> kept, Layout rest) {
            auto* first = kept[0].Get();
            auto* second = kept[1].Get();
            if (!first || !second || first->IsSplit() || second->IsSplit()) {
              return;
            }
            auto* browser = first->GetBrowserWindowInterface();
            if (!browser || second->GetBrowserWindowInterface() != browser) {
              return;
            }
            Create(browser, kept, rest, first);
          },
          kept, rest));
}

}  // namespace starry::split
