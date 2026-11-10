#include "split/settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>

namespace split {
namespace {

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

// A '#' or ';' after a space starts a comment. One without a space stays in
// the value, so a name such as "Link#2" survives.
std::string_view StripComment(std::string_view value) {
  for (std::size_t i = 1; i < value.size(); ++i) {
    if ((value[i] == '#' || value[i] == ';') &&
        (value[i - 1] == ' ' || value[i - 1] == '\t')) {
      return Trim(value.substr(0, i));
    }
  }
  return value;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::optional<bool> ParseBool(std::string_view value) {
  const std::string v = Lower(Trim(value));
  if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
  if (v == "0" || v == "false" || v == "off" || v == "no") return false;
  return std::nullopt;
}

std::optional<float> ParseFloat(std::string_view value) {
  const std::string v(Trim(value));
  if (v.empty()) return std::nullopt;
  char* end = nullptr;
  const float parsed = std::strtof(v.c_str(), &end);
  if (end == v.c_str() || *end != '\0' || !std::isfinite(parsed)) return std::nullopt;
  return parsed;
}

std::optional<int> ParseInt(std::string_view value) {
  const std::string v(Trim(value));
  if (v.empty()) return std::nullopt;
  char* end = nullptr;
  const long parsed = std::strtol(v.c_str(), &end, 10);
  if (end == v.c_str() || *end != '\0' || parsed < -1000000 || parsed > 1000000) {
    return std::nullopt;
  }
  return static_cast<int>(parsed);
}

void Set(std::string_view key, std::string_view value, const std::string& where,
         Settings& settings, std::vector<std::string>& errors) {
  if (key == "enabled") {
    const auto parsed = ParseBool(value);
    if (!parsed) {
      errors.push_back(where + ": enabled must be 0 or 1");
      return;
    }
    settings.enabled = *parsed;
  } else if (key == "layout") {
    const std::string v = Lower(Trim(value));
    if (v == "vertical" || v == "v" || v == "left-right" || v == "lr") {
      settings.layout = Layout::vertical;
    } else if (v == "horizontal" || v == "h" || v == "top-bottom" || v == "tb") {
      settings.layout = Layout::horizontal;
    } else {
      errors.push_back(where + ": layout must be vertical or horizontal");
    }
  } else if (key == "enemy_hp") {
    const auto parsed = ParseFloat(value);
    if (!parsed || *parsed < 1.0f || *parsed > 4.0f) {
      errors.push_back(where + ": enemy_hp must be between 1.0 and 4.0");
      return;
    }
    settings.enemy_hp = *parsed;
  } else if (key == "player2_controller") {
    const auto parsed = ParseInt(value);
    if (!parsed || *parsed < 2 || *parsed > 8) {
      errors.push_back(where + ": player2_controller must be 2 to 8");
      return;
    }
    settings.player2_controller = *parsed;
  } else if (key == "cutscene_mode") {
    const std::string v = Lower(Trim(value));
    if (v == "first") {
      settings.cutscene_first = true;
    } else if (v == "both") {
      settings.cutscene_first = false;
    } else {
      errors.push_back(where + ": cutscene_mode must be first or both");
    }
  } else if (key == "game_over") {
    const std::string v = Lower(Trim(value));
    if (v == "shared") {
      settings.shared_game_over = true;
    } else if (v == "solo") {
      settings.shared_game_over = false;
    } else {
      errors.push_back(where + ": game_over must be shared or solo");
    }
  } else if (key == "second_view_scale") {
    const auto parsed = ParseFloat(value);
    if (!parsed || *parsed < 0.5f || *parsed > 1.0f) {
      errors.push_back(where + ": second_view_scale must be between 0.5 and 1.0");
      return;
    }
    settings.second_view_scale = *parsed;
  } else {
    errors.push_back(where + ": unknown key '" + std::string(key) + "'");
  }
}

}  // namespace

const char* LayoutName(Layout layout) {
  return layout == Layout::vertical ? "vertical" : "horizontal";
}

void ApplyIni(std::string_view text, Settings& settings, std::vector<std::string>& errors) {
  std::size_t line_number = 0;
  while (!text.empty()) {
    const auto end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    ++line_number;

    line = Trim(line);
    if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[') {
      continue;
    }
    const auto eq = line.find('=');
    const std::string where = "split.ini line " + std::to_string(line_number);
    if (eq == std::string_view::npos) {
      errors.push_back(where + ": expected key = value");
      continue;
    }
    Set(Trim(line.substr(0, eq)), StripComment(Trim(line.substr(eq + 1))), where, settings,
        errors);
  }
}

Settings LoadSettings(const std::filesystem::path& path, std::vector<std::string>& errors) {
  Settings settings;
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    ApplyIni(text.str(), settings, errors);
  }
  if (!errors.empty()) {
    settings.enabled = false;
  }
  return settings;
}

}  // namespace split
