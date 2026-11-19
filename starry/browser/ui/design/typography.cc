// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/design/typography.h"

#include <map>
#include <utility>

#include "base/no_destructor.h"

namespace starry {

const gfx::FontList& UiFont(int size, gfx::Font::Weight weight) {
  static base::NoDestructor<std::map<std::pair<int, int>, gfx::FontList>>
      cache;
  const std::pair<int, int> key(size, static_cast<int>(weight));
  auto it = cache->find(key);
  if (it == cache->end()) {
    const gfx::FontList base;
    it = cache
             ->emplace(key, base.Derive(size - base.GetFontSize(),
                                        gfx::Font::NORMAL, weight))
             .first;
  }
  return it->second;
}

}  // namespace starry
