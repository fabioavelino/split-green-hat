#include "split_hook.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"

#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop_camera.h"
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
std::optional<CoopPuppet::PlannerSettings> g_planner_settings;
std::optional<std::string> g_scene;
std::uint64_t g_drawn_since_ms = 0;
std::uint64_t g_last_frame_ms = 0;
std::uint64_t g_components = 0;

// M1b: the second Link's input-driven state. The actor is the draw-only
// CoopPuppet (the engine refuses a second PlayerRef), moved by the game's
// own pose setter and animated with the player's AS commands.
struct PuppetInputState {
  bool started = false;
  CoopPuppet::Vec3 position{};
  float yaw = 0.0f;
  std::string anim = "Wait";
  std::uint32_t serial = 1;
  std::uint64_t action_until_ms = 0;
  std::uint64_t last_ms = 0;
  std::uint64_t test_start_ms = 0;
  std::uint64_t prev_buttons = 0;
};
PuppetInputState g_puppet_input;

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
  if (const char* actor = std::getenv("LICHII_SPLIT_ACTOR"); actor != nullptr) {
    planner_settings.actor = actor;
  }

  CoopPuppet::Configure(planner_settings, true, [](const std::string& line) {
    LOG_INFO(Core, "split: {}", line);
  });
  if (std::getenv("LICHII_SPLIT_HOLD") != nullptr ||
      std::getenv("LICHII_SPLIT_INPUT") != nullptr) {
    // M1b: the second Link runs its own animation, so the debug puppet
    // director must not copy player 1's pose onto it.
    CoopPuppet::TheDirector().SetPoseCopy(false);
  }
  // The planner is made on the first reading, when the player's age (and
  // so the puppet actor) is known.
  g_planner_settings = planner_settings;
  LOG_INFO(Core, "split: M1 debug puppet enabled, {} ms after the scene is known",
           kDebugPuppetDelayMs);
}

// M1b: find the component that owns the PlayerComponent blackboard. The
// blackboard key names live in the loaded parameter table, so a component
// that points at that table is the one to write. This is a diagnostic scan;
// the stable offsets replace it once they are known.
void ScanPlayerComponents(EmulatorMemory& memory, std::uint64_t components) {
  constexpr std::size_t kSlots = 143;
  constexpr const char* kNeedle = "RawLeftStick";
  for (std::size_t i = 0; i < kSlots; ++i) {
    std::uint64_t component = 0;
    if (!memory.read(components + i * 8, std::span<std::uint8_t>(
                                            reinterpret_cast<std::uint8_t*>(&component),
                                            sizeof(component))) ||
        component == 0) {
      continue;
    }
    std::array<std::uint8_t, 0x200> head{};
    if (!memory.read(component, head)) {
      continue;
    }
    for (std::size_t at = 0; at + 8 <= head.size(); at += 8) {
      std::uint64_t pointer = 0;
      std::memcpy(&pointer, head.data() + at, sizeof(pointer));
      if (pointer < 0x100000 || (pointer & 7) != 0) {
        continue;
      }
      std::array<std::uint8_t, 0x1000> block{};
      if (!memory.read(pointer, block)) {
        continue;
      }
      for (std::size_t off = 0; off + std::strlen(kNeedle) < block.size(); ++off) {
        if (std::memcmp(block.data() + off, kNeedle, std::strlen(kNeedle)) == 0) {
          LOG_INFO(Core,
                   "split: M1b component {} at {:#x}: key table {:#x} (+{:#x}), key at {:#x}",
                   i, component, pointer, at, pointer + off);
          break;
        }
      }
    }
  }
}

CoopPuppet::Mat3 YawMatrix(float yaw) {
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  return {c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c};
}

bool Held(std::uint64_t raw, Core::HID::NpadButton key) {
  return (raw & static_cast<std::uint64_t>(key)) != 0;
}

// M1b: controller 2 to the second Link. The camera frame is the local
// player's camera (M2 gives Link 2 its own). The puppet actor has no
// PlayerComponent, so this is the part that would move into the real
// SplitLink once the engine allows a second player.
void UpdatePuppetInput(Core::System& system, CoopPuppet::Input& input,
                       const coop::host::PlayerReading& reading, std::uint64_t now_ms) {
  CoopPuppet::Target* target = nullptr;
  for (auto& t : input.targets) {
    if (t.number == 0) {
      target = &t;
    }
  }
  if (target == nullptr) {
    return;
  }
  if (!g_puppet_input.started) {
    g_puppet_input.started = true;
    g_puppet_input.position = target->position;
    g_puppet_input.position[1] = reading.position[1];
    g_puppet_input.last_ms = now_ms;
    g_puppet_input.test_start_ms = now_ms;
  }

  const bool test = std::getenv("LICHII_SPLIT_P2TEST") != nullptr;
  float stick_x = 0.0f;
  float stick_y = 0.0f;
  Core::HID::NpadButtonState buttons{};
  if (test) {
    const std::uint64_t t = (now_ms - g_puppet_input.test_start_ms) % 7000;
    if (t < 2000) {
      stick_y = 1.0f;  // walk forward
    } else if (t < 4000) {
      stick_x = 1.0f;  // run right
    } else if (t < 4300) {
      buttons.raw = Core::HID::NpadButton::A;  // jump
    } else if (t < 4600) {
      buttons.raw = Core::HID::NpadButton::X;  // roll
    } else if (t < 4900) {
      buttons.raw = Core::HID::NpadButton::B;  // sword
    }
  } else {
    auto* controller =
        system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player2);
    if (controller == nullptr || !controller->IsConnected()) {
      return;
    }
    const auto sticks = controller->GetSticks();
    stick_x = static_cast<float>(sticks.left.x) / 32767.0f;
    stick_y = static_cast<float>(sticks.left.y) / 32767.0f;
    buttons = controller->GetNpadButtons();
  }

  std::uint64_t dt_ms = now_ms - g_puppet_input.last_ms;
  g_puppet_input.last_ms = now_ms;
  if (dt_ms > 100) {
    dt_ms = 100;
  }
  if (dt_ms == 0) {
    dt_ms = 16;
  }
  const float dt = static_cast<float>(dt_ms) / 1000.0f;

  const float length = std::min(1.0f, std::hypot(stick_x, stick_y));
  const bool moving = length > 0.1f;
  const bool running = length > 0.75f;

  float forward_x = 0.0f;
  float forward_z = 1.0f;
  float right_x = 1.0f;
  float right_z = 0.0f;
  if (const auto camera = CoopCamera::Latest()) {
    const auto& m = camera->clip_from_world;
    const float fl = std::hypot(m[12], m[14]);
    const float rl = std::hypot(m[0], m[2]);
    if (fl > 1e-4f) {
      forward_x = m[12] / fl;
      forward_z = m[14] / fl;
    }
    if (rl > 1e-4f) {
      right_x = m[0] / rl;
      right_z = m[2] / rl;
    }
  }
  float wish_x = right_x * stick_x + forward_x * stick_y;
  float wish_z = right_z * stick_x + forward_z * stick_y;
  const float wish_length = std::hypot(wish_x, wish_z);
  if (wish_length > 1.0f) {
    wish_x /= wish_length;
    wish_z /= wish_length;
  }
  if (moving && wish_length > 1e-4f) {
    const float speed = running ? 3.6f : 1.2f;
    g_puppet_input.position[0] += wish_x * speed * dt;
    g_puppet_input.position[2] += wish_z * speed * dt;
    g_puppet_input.yaw = std::atan2(wish_x, wish_z);
  }
  // The ground under Link 1 is also the ground under Link 2 in the same
  // scene; follow it within a small step so stairs and ledges work.
  const float ground = reading.position[1];
  if (std::fabs(ground - g_puppet_input.position[1]) < 2.0f) {
    g_puppet_input.position[1] = ground;
  }

  const std::uint64_t raw_buttons = static_cast<std::uint64_t>(buttons.raw);
  const bool pressed_a = Held(raw_buttons, Core::HID::NpadButton::A) &&
                         !Held(g_puppet_input.prev_buttons, Core::HID::NpadButton::A);
  const bool pressed_b = Held(raw_buttons, Core::HID::NpadButton::B) &&
                         !Held(g_puppet_input.prev_buttons, Core::HID::NpadButton::B);
  const bool pressed_x = Held(raw_buttons, Core::HID::NpadButton::X) &&
                         !Held(g_puppet_input.prev_buttons, Core::HID::NpadButton::X);
  g_puppet_input.prev_buttons = raw_buttons;

  std::string anim;
  if (pressed_a) {
    anim = "Jump";
    g_puppet_input.action_until_ms = now_ms + 700;
  } else if (pressed_x) {
    anim = "Rolling";
    g_puppet_input.action_until_ms = now_ms + 600;
  } else if (pressed_b) {
    anim = "Sword_Attack_Vertical1_From_L";
    g_puppet_input.action_until_ms = now_ms + 500;
  } else if (Held(raw_buttons, Core::HID::NpadButton::ZR)) {
    anim = "ShieldGuard";
    g_puppet_input.action_until_ms = now_ms + 100;
  } else if (now_ms < g_puppet_input.action_until_ms) {
    anim = g_puppet_input.anim;
  } else if (moving) {
    anim = running ? "MoveRun" : "Move";
  } else {
    anim = "Wait";
  }
  if (anim != g_puppet_input.anim) {
    g_puppet_input.anim = std::move(anim);
    ++g_puppet_input.serial;
  }

  target->position = g_puppet_input.position;
  target->rotation = YawMatrix(g_puppet_input.yaw);
  target->show = CoopPuppet::Show::show;
  target->anim = CoopPuppet::Anim{g_puppet_input.anim, 0.0f, 1.0f};
  target->anim_serial = g_puppet_input.serial;
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
  // The renderer's camera track (0003) does nothing unless asked.
  CoopCamera::SetWanted(true);

  // M1b: player 2's controller drives the second Link by default (the
  // setting says controller 2). A caller can still force it off with
  // LICHII_SPLIT_INPUT=0; LICHII_SPLIT_INPUT=1 forces it on.
#ifdef _MSC_VER
  if (std::getenv("LICHII_SPLIT_INPUT") == nullptr && g_settings.player2_controller == 2) {
    _putenv_s("LICHII_SPLIT_INPUT", "1");
  }
#else
  if (g_settings.player2_controller == 2) {
    setenv("LICHII_SPLIT_INPUT", "1", 0);
  }
#endif

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
  CoopCamera::SetWanted(false);
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
  if (!g_game_ok) {
    return;
  }

  // M1b experiment: hold player 1's left stick right so the game itself
  // writes RawLeftStick into the player's blackboard. The live memory diff
  // then shows where the values live. LICHII_SPLIT_STICK turns it on. The
  // emulated controller is reset while the game boots, so put it back
  // whenever it disappears.
  if (std::getenv("LICHII_SPLIT_STICK") != nullptr) {
    static bool stick_logged = false;
    for (const auto id : {Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld}) {
      auto* controller = system.HIDCore().GetEmulatedController(id);
      if (controller == nullptr || !controller->IsConnected() ||
          controller->HasInputInjector()) {
        continue;
      }
      controller->SetInputInjector([]() -> std::optional<Core::HID::InjectedInput> {
        Core::HID::InjectedInput input;
        input.left = Core::HID::AnalogStickState{32767, 0};
        return input;
      });
      if (!stick_logged) {
        stick_logged = true;
        LOG_INFO(Core, "split: M1b stick experiment on, controller {}", static_cast<int>(id));
      }
    }
  }

  EmulatorMemory memory(system.ApplicationMemory(), g_main_base);
  coop::host::PlayerReader reader(memory, &g_drawn_state);
  auto reading = reader.read();
  if (!reading) {
    // The module list can be incomplete while the game boots, so the first
    // base may be an entry point rather than main. Ask again now that the
    // application is running.
    static std::uint64_t last_base_check_ms = 0;
    const std::uint64_t check_ms = NowMs();
    if (check_ms - last_base_check_ms >= 2000) {
      last_base_check_ms = check_ms;
      const std::uint64_t base = FindMain(system, process);
      if (base != 0 && base != g_main_base) {
        LOG_INFO(Core, "split: main base corrected {:#x} -> {:#x}", g_main_base, base);
        g_main_base = base;
        EmulatorMemory corrected(system.ApplicationMemory(), g_main_base);
        coop::host::PlayerReader again(corrected, &g_drawn_state);
        reading = again.read();
      }
    }
    if (!reading) {
      return;
    }
  }

  if (!g_planner && g_planner_settings) {
    if (g_planner_settings->actor.empty() && reading->age) {
      g_planner_settings->actor = *reading->age == coop::protocol::Age::adult
                                      ? "CoopPuppet_Adult"
                                      : "CoopPuppet_Child";
    }
    g_planner.emplace(*g_planner_settings);
  }
  if (!g_planner) {
    return;
  }

  const std::uint64_t now_ms = NowMs();
  // The puppet director waits for a short unbroken run of drawn frames before
  // it creates an actor. A camera-patch build would feed this from
  // CoopCamera::Latest(); M1a has no camera patch, so use the present clock:
  // start the run now and reset it after a gap or a scene change.
  // A slow host (a few fps) has normal frame gaps far above 250 ms; only a
  // real load stops presenting for much longer. The director's own drawn
  // threshold is 750 ms.
  if (g_last_frame_ms != 0 && now_ms - g_last_frame_ms > 1000) {
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

  // M1b experiment: snapshot the player component heap once a second while
  // LICHII_SPLIT_DUMP is set. The debug input tool can hold a stick during
  // the captures; diffing the files then shows exactly what the game wrote.
  static std::uint64_t last_dump_ms = 0;
  static unsigned dump_index = 0;
  if (g_components != 0) {
    if (const char* prefix = std::getenv("LICHII_SPLIT_DUMP"); prefix != nullptr) {
      if (last_dump_ms == 0 || now_ms - last_dump_ms >= 1000) {
        last_dump_ms = now_ms;
        std::vector<std::uint8_t> block(0x40000);
        if (memory.read(g_components, block)) {
          std::ofstream out(fmt::format("{}_{:03}.bin", prefix, dump_index++),
                            std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(block.data()),
                    static_cast<std::streamsize>(block.size()));
        }
      }
    }
  }

  auto input = g_planner->Plan(g_main_base, reading->scene, reading->position, false, {},
                               now_ms);
  static const bool input_puppet = std::getenv("LICHII_SPLIT_INPUT") != nullptr;
  if (input_puppet) {
    UpdatePuppetInput(system, input, *reading, now_ms);
  }
  static const bool hold_puppet = std::getenv("LICHII_SPLIT_HOLD") != nullptr;
  if (hold_puppet) {
    for (auto& target : input.targets) {
      target.show = CoopPuppet::Show::hold;
    }
  }
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
    LOG_INFO(Core, "split: diag event {} age {}", reading->event_running.value_or(false),
             reading->age ? static_cast<int>(*reading->age) : -1);
  }
  input.camera = true;
  input.drawn_since_ms = g_drawn_since_ms;
  input.settle_ms = 0;
  CoopPuppet::TheDirector().SetInput(std::move(input));

  // M1b: the player actor and its component table, once per process.
  static bool logged_actor = false;
  if (!logged_actor) {
    if (const auto actor = reader.find_player_actor()) {
      logged_actor = true;
      std::uint64_t components = 0;
      memory.read(*actor + 0x270, std::span<std::uint8_t>(
                                     reinterpret_cast<std::uint8_t*>(&components),
                                     sizeof(components)));
      LOG_INFO(Core, "split: M1b player actor {:#x}, components {:#x}", *actor, components);
      g_components = components;
      ScanPlayerComponents(memory, components);
    }
  }

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
