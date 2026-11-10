#include "split_hook.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"

#include "split/settings.hpp"
#include "split/version.hpp"

#ifndef LICHII_SPLIT_VERSION
#define LICHII_SPLIT_VERSION "dev"
#endif

namespace SplitHook {
namespace {

constexpr std::uint64_t kStatusEveryFrames = 1800;  // about 30 s at 60 fps

split::Settings g_settings;
bool g_started = false;
std::uint64_t g_frames = 0;

std::filesystem::path SettingsPath() {
  return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "split.ini";
}

void LogSettings() {
  LOG_INFO(Core, "split: build {}, layout {}, enemy hp {:.2f}x, player 2 controller {}",
           split::Version(), split::LayoutName(g_settings.layout), g_settings.enemy_hp,
           g_settings.player2_controller);
  LOG_INFO(Core, "split: cutscene {}, game over {}, second view scale {:.2f}",
           g_settings.cutscene_first ? "first player" : "both players",
           g_settings.shared_game_over ? "shared" : "solo", g_settings.second_view_scale);
}

}  // namespace

void Start() {
  if (g_started) {
    return;
  }
  g_started = true;
  g_frames = 0;

  std::vector<std::string> errors;
  const auto path = SettingsPath();
  g_settings = split::LoadSettings(path, errors);
  for (const auto& error : errors) {
    LOG_ERROR(Core, "split: {}", error);
  }

  LOG_INFO(Core, "split: loaded {} ({})", split::Version(), LICHII_SPLIT_VERSION);
  if (!g_settings.enabled) {
    LOG_INFO(Core, "split: off");
    return;
  }
  LOG_INFO(Core, "split: on, settings {}", path.string());
  LogSettings();
}

void Shutdown() {
  if (!g_started) {
    return;
  }
  g_started = false;
  LOG_INFO(Core, "split: shutdown");
}

void OnFrame(Core::System& system) {
  static_cast<void>(system);
  if (!g_started || !g_settings.enabled) {
    return;
  }
  ++g_frames;
  if (g_frames == 1 || g_frames % kStatusEveryFrames == 0) {
    LOG_INFO(Core, "split: {} presented frames, layout {}, enemy hp {:.2f}x", g_frames,
             split::LayoutName(g_settings.layout), g_settings.enemy_hp);
  }
}

bool Enabled() {
  return g_started && g_settings.enabled;
}

const split::Settings& CurrentSettings() {
  return g_settings;
}

std::vector<OverlayLine> OverlayLines() {
  if (!Enabled()) {
    return {};
  }
  std::vector<OverlayLine> lines;
  lines.push_back({"Split: M0", false});
  lines.push_back({fmt::format("layout: {}", split::LayoutName(g_settings.layout)), false});
  lines.push_back({fmt::format("enemy hp: {:.2f}x", g_settings.enemy_hp), false});
  lines.push_back({fmt::format("P2 controller: {}", g_settings.player2_controller), true});
  return lines;
}

}  // namespace SplitHook
