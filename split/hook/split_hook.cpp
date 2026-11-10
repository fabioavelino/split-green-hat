#include "split_hook.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"

#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop_puppet.h"
#include "split/settings.hpp"
#include "split/version.hpp"

#ifndef LICHII_SPLIT_VERSION
#define LICHII_SPLIT_VERSION "dev"
#endif

namespace SplitHook {
namespace {

// The game and build the addresses in the co-op host library are for.
constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr const char* kGameBuildId = "e0f3ccfaf50065ae";
constexpr std::uint64_t kStatusEveryFrames = 1800;  // about 30 s at 60 fps
constexpr std::uint64_t kDebugPuppetDelayMs = 3000;

split::Settings g_settings;
bool g_started = false;
std::uint64_t g_frames = 0;
Kernel::KProcess* g_process = nullptr;
std::uint64_t g_main_base = 0;
bool g_game_ok = false;
bool g_build_warned = false;
coop::host::PlayerReader::DrawnState g_drawn_state;
std::optional<CoopPuppet::Planner> g_planner;
std::optional<std::string> g_scene;
std::uint64_t g_drawn_since_ms = 0;
std::uint64_t g_last_frame_ms = 0;

std::filesystem::path SettingsPath() {
  return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "split.ini";
}

std::string Hex(const std::array<u8, 0x20>& id, std::size_t bytes) {
  std::string out;
  char buf[3];
  for (std::size_t i = 0; i < bytes; ++i) {
    std::snprintf(buf, sizeof(buf), "%02x", id[i]);
    out += buf;
  }
  return out;
}

// The co-op host library's view of guest memory.
class EmulatorMemory final : public coop::host::GameMemory {
public:
  EmulatorMemory(Core::Memory::Memory& memory, std::uint64_t main_base)
      : memory_(memory), main_base_(main_base) {}

  std::uint64_t main_base() const override {
    return main_base_;
  }

  bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
    if (out.empty()) {
      return true;
    }
    if (address + out.size() < address ||
        !memory_.IsValidVirtualAddressRange(address, out.size())) {
      return false;
    }
    return memory_.ReadBlockUnsafe(address, out.data(), out.size());
  }

private:
  Core::Memory::Memory& memory_;
  std::uint64_t main_base_;
};

Loader::AppLoader::Modules Modules(Core::System& system, Kernel::KProcess* process) {
  Loader::AppLoader::Modules modules;
  if (system.GetAppLoader().ReadNSOModules(modules) != Loader::ResultStatus::Success ||
      modules.empty()) {
    modules = Core::FindModules(process);
  }
  return modules;
}

std::uint64_t FindMain(Core::System& system, Kernel::KProcess* process) {
  for (const auto& [address, name] : Modules(system, process)) {
    if (name == "main" || name.starts_with("main.")) {
      return address;
    }
  }
  return GetInteger(Core::FindMainModuleEntrypoint(process));
}

std::uint64_t NowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void ConfigureDebugPuppet() {
  CoopPuppet::PlannerSettings planner_settings;
  planner_settings.debug = true;
  planner_settings.debug_delay_ms = kDebugPuppetDelayMs;
  planner_settings.debug_offset = {2.0f, 0.0f, 0.0f};  // about 2 m to Link's right

  CoopPuppet::Configure(planner_settings, true, [](const std::string& line) {
    LOG_INFO(Core, "split: {}", line);
  });
  g_planner.emplace(planner_settings);
  LOG_INFO(Core, "split: M1 debug puppet enabled, {} ms after the scene is known",
           kDebugPuppetDelayMs);
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
  g_process = nullptr;
  g_main_base = 0;
  g_game_ok = false;
  g_build_warned = false;
  g_scene.reset();
  g_drawn_since_ms = 0;
  g_last_frame_ms = 0;

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

  // CoopSvc::PatchMain reads these when the game's main module is patched.
  // The debug puppet is created by the audited co-op actor path; M1 uses it
  // to prove the code cave and the puppet pack before the real SplitLink.
#ifdef _MSC_VER
  _putenv_s("LICHII_COOP_PUPPET", "1");
  _putenv_s("LICHII_COOP_TEST_PUPPET", "1");
#else
  setenv("LICHII_COOP_PUPPET", "1", 1);
  setenv("LICHII_COOP_TEST_PUPPET", "1", 1);
#endif

  ConfigureDebugPuppet();
}

void Shutdown() {
  if (!g_started) {
    return;
  }
  g_started = false;
  g_planner.reset();
  g_scene.reset();
  g_drawn_since_ms = 0;
  g_last_frame_ms = 0;
  CoopPuppet::TheDirector().SetInput(CoopPuppet::Input{});
  LOG_INFO(Core, "split: shutdown");
}

void OnFrame(Core::System& system) {
  if (!g_started || !g_settings.enabled) {
    return;
  }
  ++g_frames;

  Kernel::KProcess* const process = system.ApplicationProcess();
  if (process == nullptr) {
    return;
  }
  if (system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
    return;
  }

  if (process != g_process) {
    g_process = process;
    g_main_base = FindMain(system, process);
    const std::string build = Hex(system.GetApplicationProcessBuildID(), 8);
    g_game_ok = build == kGameBuildId;
    LOG_INFO(Core, "split: M1 game build {}, main at {:#x}", build, g_main_base);
    if (!g_game_ok) {
      if (!g_build_warned) {
        g_build_warned = true;
        LOG_WARNING(Core, "split: game build {} is not {}; M1 spawn is idle", build,
                    kGameBuildId);
      }
      return;
    }
  }
  if (!g_game_ok || !g_planner) {
    return;
  }

  EmulatorMemory memory(system.ApplicationMemory(), g_main_base);
  coop::host::PlayerReader reader(memory, &g_drawn_state);
  const auto reading = reader.read();
  if (!reading) {
    return;
  }

  const std::uint64_t now_ms = NowMs();
  // The puppet director waits for a short unbroken run of drawn frames before
  // it creates an actor. A camera-patch build would feed this from
  // CoopCamera::Latest(); M1a has no camera patch, so use the present clock:
  // start the run now and reset it after a gap or a scene change.
  if (g_last_frame_ms != 0 && now_ms - g_last_frame_ms > 250) {
    g_drawn_since_ms = now_ms;
  }
  g_last_frame_ms = now_ms;
  if (reading->scene && g_scene != reading->scene) {
    g_scene = reading->scene;
    g_drawn_since_ms = now_ms;
  }
  if (g_drawn_since_ms == 0) {
    g_drawn_since_ms = now_ms;
  }

  auto input = g_planner->Plan(g_main_base, reading->scene, reading->position, false, {},
                               now_ms);
  const std::size_t target_count = input.targets.size();

  // M1 diagnostics: what the planner decided, and from what.
  static std::uint64_t last_diag_ms = 0;
  if (last_diag_ms == 0 || now_ms - last_diag_ms >= 10000) {
    last_diag_ms = now_ms;
    LOG_INFO(Core,
             "split: diag scene '{}', pos {:.1f} {:.1f} {:.1f}, debug {}, targets {}, "
             "drawn since {}",
             reading->scene.value_or("<none>"), reading->position[0], reading->position[1],
             reading->position[2], g_planner->settings().debug, target_count,
             g_drawn_since_ms);
  }
  input.camera = true;
  input.drawn_since_ms = g_drawn_since_ms;
  input.settle_ms = 0;
  CoopPuppet::TheDirector().SetInput(std::move(input));

  if (g_frames == 1 || g_frames % kStatusEveryFrames == 0) {
    LOG_INFO(Core, "split: {} presented frames, layout {}, enemy hp {:.2f}x, puppet targets {}",
             g_frames, split::LayoutName(g_settings.layout), g_settings.enemy_hp, target_count);
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
  lines.push_back({"Split: M1 debug puppet", false});
  lines.push_back({fmt::format("layout: {}", split::LayoutName(g_settings.layout)), false});
  lines.push_back({fmt::format("enemy hp: {:.2f}x", g_settings.enemy_hp), false});
  lines.push_back({fmt::format("P2 controller: {}", g_settings.player2_controller), true});
  return lines;
}

}  // namespace SplitHook
