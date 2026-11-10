#include <cassert>
#include <cmath>

#include "split/viewport.hpp"

namespace {

bool Near(float a, float b) {
  return std::fabs(a - b) < 1e-5f;
}

}  // namespace

void TestViewports() {
  using split::Layout;

  {
    const auto views = split::Viewports(Layout::vertical, 1920.0f, 1080.0f);
    assert(Near(views[0].x, 0.0f));
    assert(Near(views[0].y, 0.0f));
    assert(Near(views[0].width, 960.0f));
    assert(Near(views[0].height, 1080.0f));
    assert(Near(views[1].x, 960.0f));
    assert(Near(views[1].width, 960.0f));
  }

  {
    const auto views = split::Viewports(Layout::horizontal, 1920.0f, 1080.0f);
    assert(Near(views[0].x, 0.0f));
    assert(Near(views[0].y, 0.0f));
    assert(Near(views[0].width, 1920.0f));
    assert(Near(views[0].height, 540.0f));
    assert(Near(views[1].y, 540.0f));
    assert(Near(views[1].height, 540.0f));
  }

  {
    const auto views = split::Viewports(Layout::vertical, 100.0f, 100.0f, 4.0f);
    assert(Near(views[0].width, 48.0f));
    assert(Near(views[1].x, 52.0f));
    assert(Near(views[1].width, 48.0f));
  }

  {
    const auto views = split::Viewports(Layout::vertical, 0.0f, 100.0f);
    assert(views[0] == split::Rect{});
    assert(views[1] == split::Rect{});
  }
}
