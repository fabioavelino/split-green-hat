#include "split/viewport.hpp"

#include <algorithm>

namespace split {
namespace {

float NonNegative(float value) {
  return value > 0.0f ? value : 0.0f;
}

}  // namespace

std::array<Rect, 2> Viewports(Layout layout, float width, float height, float divider) {
  width = NonNegative(width);
  height = NonNegative(height);
  divider = std::clamp(divider, 0.0f, std::min(width, height));

  if (width <= 0.0f || height <= 0.0f) {
    return {Rect{}, Rect{}};
  }

  if (layout == Layout::vertical) {
    const float half = width * 0.5f;
    const float half_gap = divider * 0.5f;
    const float split = std::clamp(half, 0.0f, width);
    const Rect left{0.0f, 0.0f, NonNegative(split - half_gap), height};
    const float right_x = std::min(width, split + half_gap);
    const Rect right{right_x, 0.0f, NonNegative(width - right_x), height};
    return {left, right};
  }

  const float half = height * 0.5f;
  const float half_gap = divider * 0.5f;
  const float split = std::clamp(half, 0.0f, height);
  const Rect top{0.0f, 0.0f, width, NonNegative(split - half_gap)};
  const float bottom_y = std::min(height, split + half_gap);
  const Rect bottom{0.0f, bottom_y, width, NonNegative(height - bottom_y)};
  return {top, bottom};
}

}  // namespace split
