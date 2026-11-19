// Copyright 2026 The Starry Browser Authors

#include "starry/components/split_layout/split_layout.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace starry::split {
namespace {
base::Value Write(const Layout& node) {
  if (node.children.empty()) {
    return base::Value(node.pane);
  }
  base::DictValue dict;
  dict.Set("horizontal", node.horizontal);
  base::ListValue children;
  base::ListValue fractions;
  for (const auto& child : node.children) {
    children.Append(Write(child));
  }
  for (double fraction : node.fractions) {
    fractions.Append(fraction);
  }
  dict.Set("children", std::move(children));
  dict.Set("fractions", std::move(fractions));
  return base::Value(std::move(dict));
}
std::optional<Layout> Read(const base::Value& value, int depth = 0) {
  if (depth > 2) {
    return std::nullopt;
  }
  Layout result;
  if (value.is_int()) {
    result.pane = value.GetInt();
    return result;
  }
  if (!value.is_dict()) {
    return std::nullopt;
  }
  const auto& dict = value.GetDict();
  const auto* children = dict.FindList("children");
  const auto* fractions = dict.FindList("fractions");
  const auto horizontal = dict.FindBool("horizontal");
  if (!children || children->size() < 2 || children->size() > kMaxPanes ||
      !fractions || fractions->size() != children->size() || !horizontal) {
    return std::nullopt;
  }
  result.horizontal = *horizontal;
  for (const auto& child : *children) {
    auto parsed = Read(child, depth + 1);
    if (!parsed) {
      return std::nullopt;
    }
    result.children.push_back(std::move(*parsed));
  }
  for (const auto& fraction : *fractions) {
    auto number = fraction.GetIfDouble();
    if (!number || !std::isfinite(*number) || *number <= 0) {
      return std::nullopt;
    }
    result.fractions.push_back(*number);
  }
  return result.Normalized();
}
void Place(const Layout& node,
           const gfx::Rect& rect,
           int gap,
           std::vector<int> path,
           Geometry& frames) {
  if (node.children.empty()) {
    frames.panes[node.pane] = rect;
    return;
  }
  const bool horizontal = node.horizontal;
  const int origin = horizontal ? rect.x() : rect.y();
  const int length = horizontal ? rect.width() : rect.height();
  // Keep every pane inside even an extremely small window.
  gap = std::min(gap, length / static_cast<int>(node.children.size() - 1));
  const int available =
      std::max(0, length - gap * static_cast<int>(node.children.size() - 1));
  int start = origin;
  double cumulative = 0;
  for (size_t i = 0; i < node.children.size(); ++i) {
    cumulative += node.fractions[i];
    int end = i + 1 == node.children.size()
                  ? origin + length
                  : origin + std::lround(available * cumulative) + gap * i;
    gfx::Rect child =
        horizontal ? gfx::Rect(start, rect.y(), end - start, rect.height())
                   : gfx::Rect(rect.x(), start, rect.width(), end - start);
    auto child_path = path;
    child_path.push_back(i);
    Place(node.children[i], child, gap, child_path, frames);
    if (i + 1 < node.children.size()) {
      frames.dividers.push_back(
          {path, static_cast<int>(i), horizontal,
           horizontal ? gfx::Rect(end, rect.y(), gap, rect.height())
                      : gfx::Rect(rect.x(), end, rect.width(), gap),
           rect});
    }
    start = end + gap;
  }
}
}  // namespace

Layout Layout::Pair(int target, int added, bool horizontal, bool leading) {
  Layout result;
  result.horizontal = horizontal;
  Layout first, second;
  first.pane = leading ? added : target;
  second.pane = leading ? target : added;
  result.children = {first, second};
  result.fractions = {0.5, 0.5};
  return result;
}
Layout Layout::Decode(const std::string& json,
                      int count,
                      bool horizontal,
                      double ratio) {
  if (json.size() <= 4096) {
    auto value = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    auto parsed = value ? Read(*value) : std::nullopt;
    if (parsed) {
      auto ids = parsed->Panes();
      auto sorted = ids;
      std::ranges::sort(sorted);
      std::vector<int> expected(count);
      std::iota(expected.begin(), expected.end(), 0);
      if (sorted == expected && ids == expected && count <= kMaxPanes) {
        return *parsed;
      }
    }
  }
  Layout result;
  result.horizontal = horizontal;
  for (int i = 0; i < std::clamp(count, 2, kMaxPanes); ++i) {
    Layout leaf;
    leaf.pane = i;
    result.children.push_back(leaf);
    result.fractions.push_back(1.0 / count);
  }
  if (count == 2 && std::isfinite(ratio) && ratio > 0 && ratio < 1) {
    result.fractions = {ratio, 1 - ratio};
  }
  return result.Normalized();
}
std::string Layout::Encode() const {
  return base::WriteJson(Write(*this)).value_or("");
}
std::vector<int> Layout::Panes() const {
  if (children.empty()) {
    return {pane};
  }
  std::vector<int> result;
  for (const auto& child : children) {
    auto ids = child.Panes();
    result.insert(result.end(), ids.begin(), ids.end());
  }
  return result;
}
Layout Layout::Normalized() const {
  if (children.empty()) {
    return *this;
  }
  Layout result;
  result.horizontal = horizontal;
  const bool valid = fractions.size() == children.size() &&
                     std::ranges::all_of(fractions, [](double f) {
                       return std::isfinite(f) && f > 0;
                     });
  const double total =
      valid ? std::accumulate(fractions.begin(), fractions.end(), 0.0)
            : children.size();
  for (size_t i = 0; i < children.size(); ++i) {
    auto child = children[i].Normalized();
    const double share = (valid ? fractions[i] : 1) / total;
    if (!child.children.empty() && child.horizontal == horizontal) {
      for (size_t j = 0; j < child.children.size(); ++j) {
        result.children.push_back(child.children[j]);
        result.fractions.push_back(share * child.fractions[j]);
      }
    } else {
      result.children.push_back(child);
      result.fractions.push_back(share);
    }
  }
  return result.children.size() == 1 ? result.children[0] : result;
}
bool Layout::Insert(int added, int target, bool axis, bool leading) {
  const auto panes = Panes();
  if (panes.size() >= kMaxPanes ||
      std::ranges::find(panes, added) != panes.end()) {
    return false;
  }
  if (children.empty()) {
    if (pane != target) {
      return false;
    }
    *this = Pair(target, added, axis, leading);
    return true;
  }
  for (size_t i = 0; i < children.size(); ++i) {
    if (horizontal == axis && children[i].children.empty() &&
        children[i].pane == target) {
      Layout leaf;
      leaf.pane = added;
      children.insert(children.begin() + i + (leading ? 0 : 1), leaf);
      fractions.assign(children.size(), 1.0 / children.size());
      return true;
    }
    if (children[i].Insert(added, target, axis, leading)) {
      *this = Normalized();
      return true;
    }
  }
  return false;
}
std::optional<Layout> Layout::Remove(int removed) const {
  if (children.empty()) {
    return pane == removed ? std::nullopt : std::optional(*this);
  }
  Layout result;
  result.horizontal = horizontal;
  for (size_t i = 0; i < children.size(); ++i) {
    if (auto child = children[i].Remove(removed)) {
      result.children.push_back(*child);
      result.fractions.push_back(fractions[i]);
    }
  }
  return result.children.empty() ? std::nullopt
                                 : std::optional(result.Normalized());
}
void Layout::Equalize() {
  fractions.assign(children.size(),
                   children.empty() ? 1 : 1.0 / children.size());
  for (auto& child : children) {
    child.Equalize();
  }
}
void Layout::Renumber() {
  int next = 0;
  auto visit = [&](auto&& self, Layout& node) -> void {
    if (node.children.empty()) {
      node.pane = next++;
    }
    for (auto& child : node.children) {
      self(self, child);
    }
  };
  visit(visit, *this);
}
Layout* Layout::At(const std::vector<int>& path) {
  Layout* result = this;
  for (int index : path) {
    if (index < 0 || static_cast<size_t>(index) >= result->children.size()) {
      return nullptr;
    }
    result = &result->children[index];
  }
  return result;
}
Geometry Frames(const Layout& layout, const gfx::Rect& bounds, int gap) {
  Geometry result;
  result.panes.resize(layout.Panes().size());
  Place(layout, bounds, gap, {}, result);
  return result;
}
}  // namespace starry::split
