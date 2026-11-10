#include <cassert>
#include <cmath>
#include <string>
#include <vector>

#include "split/settings.hpp"

void TestSettings() {
  using split::Layout;
  using split::Settings;

  {
    Settings s;
    std::vector<std::string> errors;
    split::ApplyIni("enabled = 1\nlayout = horizontal\nenemy_hp = 2.5\n"
                    "player2_controller = 3\nsecond_view_scale = 0.75\n",
                    s, errors);
    assert(errors.empty());
    assert(s.enabled);
    assert(s.layout == Layout::horizontal);
    assert(std::fabs(s.enemy_hp - 2.5f) < 1e-6f);
    assert(s.player2_controller == 3);
    assert(std::fabs(s.second_view_scale - 0.75f) < 1e-6f);
  }

  {
    Settings s;
    std::vector<std::string> errors;
    split::ApplyIni("layout = sideways\nenemy_hp = 9\n", s, errors);
    assert(errors.size() == 2);
    // A typo must not leave split mode half-configured.
    std::vector<std::string> load_errors;
    s = split::LoadSettings("/nonexistent/split.ini", load_errors);
    assert(load_errors.empty());
    assert(s.enabled);
  }

  {
    Settings s;
    std::vector<std::string> errors;
    split::ApplyIni("enemy_hp = 4\n", s, errors);
    assert(errors.empty());
    assert(std::fabs(s.enemy_hp - 4.0f) < 1e-6f);
    split::ApplyIni("enemy_hp = 4.1\n", s, errors);
    assert(errors.size() == 1);
  }
}
