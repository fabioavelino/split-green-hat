#pragma once

// Settings for the single-instance two-player split module.
//
// The file is `split.ini` in the emulator's config folder. Unknown keys or
// invalid values are reported and turn split mode off, so a typo never starts
// a half-configured session.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace split {

enum class Layout {
  vertical,    // left / right
  horizontal,  // top / bottom
};

struct Settings {
  bool enabled = true;
  Layout layout = Layout::vertical;
  float enemy_hp = 2.0f;          // 1.0 to 4.0
  int player2_controller = 2;     // Lichii controller index, 2 to 8
  bool cutscene_first = true;     // first player in owns the full screen
  bool shared_game_over = true;   // one game over, Continue restarts both
  float second_view_scale = 1.0f; // 0.5 to 1.0
};

const char* LayoutName(Layout layout);

// Parses a `key = value` document. Each problem is appended to `errors` with
// its line number.
void ApplyIni(std::string_view text, Settings& settings, std::vector<std::string>& errors);

// Reads `path` (a missing file is fine), then validates. Any error clears
// `enabled`, so the emulator starts normally.
Settings LoadSettings(const std::filesystem::path& path, std::vector<std::string>& errors);

}  // namespace split
