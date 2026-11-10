#pragma once

#include <array>

#include "split/settings.hpp"

namespace split {

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  bool operator==(const Rect&) const = default;
};

// Two viewports covering `width` x `height`. `divider` is the width of the
// gap between the views in pixels; it is split evenly between them. A zero or
// negative size gives two empty rectangles.
std::array<Rect, 2> Viewports(Layout layout, float width, float height, float divider = 0.0f);

}  // namespace split
