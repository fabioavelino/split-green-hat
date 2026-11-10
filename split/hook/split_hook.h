#pragma once

// The single-instance split hook. One entry point is called once per presented
// guest frame; the rest of the module will grow into the second-player and
// split-render implementation.
//
// M0 is deliberately small: it loads settings, logs the build, and gives the
// Qt panel something to show. No guest memory is touched yet.

#include <string>
#include <vector>

#include "split/settings.hpp"

namespace Core {
class System;
}

namespace SplitHook {

struct OverlayLine {
  std::string text;
  bool dim = false;
};

// Reads `split.ini` and starts the module. Idempotent. Called on the GUI
// thread once the render window exists.
void Start();

// Stops the module. Idempotent. Called when the main window closes.
void Shutdown();

// Once per presented guest frame, on the present thread.
void OnFrame(Core::System& system);

bool Enabled();
const split::Settings& CurrentSettings();

// Lines for the small overlay panel. Empty when split mode is off.
std::vector<OverlayLine> OverlayLines();

}  // namespace SplitHook
