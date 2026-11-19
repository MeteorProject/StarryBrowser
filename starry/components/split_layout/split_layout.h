// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_SPLIT_LAYOUT_SPLIT_LAYOUT_H_
#define STARRY_COMPONENTS_SPLIT_LAYOUT_SPLIT_LAYOUT_H_

#include <optional>
#include <string>
#include <vector>

#include "ui/gfx/geometry/rect.h"

namespace starry::split {

inline constexpr int kMaxPanes = 3;

// Split layout tree. Pane numbers refer to the group's reading order;
// operations retain numbers until Renumber() is called.
struct Layout {
  int pane = 0;
  bool horizontal = true;
  std::vector<Layout> children;
  std::vector<double> fractions;

  static Layout Pair(int target, int added, bool horizontal, bool leading);
  static Layout Decode(const std::string& json,
                       int count,
                       bool horizontal,
                       double ratio);
  std::string Encode() const;
  std::vector<int> Panes() const;
  Layout Normalized() const;
  bool Insert(int added, int target, bool horizontal, bool leading);
  std::optional<Layout> Remove(int removed) const;
  void Equalize();
  void Renumber();
  Layout* At(const std::vector<int>& path);
  bool operator==(const Layout&) const = default;
};

struct DividerGeometry {
  std::vector<int> path;
  int index = 0;
  bool horizontal = true;
  gfx::Rect bounds;
  gfx::Rect container;
};
struct Geometry {
  std::vector<gfx::Rect> panes;
  std::vector<DividerGeometry> dividers;
};

Geometry Frames(const Layout& layout, const gfx::Rect& bounds, int gap = 8);

}  // namespace starry::split

#endif  // STARRY_COMPONENTS_SPLIT_LAYOUT_SPLIT_LAYOUT_H_
