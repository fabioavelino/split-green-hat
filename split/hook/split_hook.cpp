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
#include <unordered_set>
#include <utility>
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

#include "coop/host/game_flags.hpp"
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
std::uint64_t g_player_actor = 0;
bool g_split_screen = false;
bool g_game_over = false;
int g_shared_health = -1;
int g_shared_max = -1;

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
class EmulatorMemory final : public coop::host::WritableGameMemory {
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

  bool write(std::uint64_t address, std::span<const std::uint8_t> in) override {
    if (in.empty()) {
      return true;
    }
    if (address + in.size() < address ||
        !memory_.IsValidVirtualAddressRange(address, in.size())) {
      return false;
    }
    return memory_.WriteBlockUnsafe(address, in.data(), in.size());
  }

  std::optional<std::uint32_t> compare_exchange_u32(
      std::uint64_t address, std::uint32_t expected, std::uint32_t desired) override {
    std::uint32_t current = 0;
    if (!read(address, std::span<std::uint8_t>(
                           reinterpret_cast<std::uint8_t*>(&current), 4))) {
      return std::nullopt;
    }
    if (current != expected) {
      return current;
    }
    if (!write(address, std::span<const std::uint8_t>(
                            reinterpret_cast<const std::uint8_t*>(&desired), 4))) {
      return std::nullopt;
    }
    return expected;
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
  // LICHII_SPLIT_OFFSET_Y: place the puppet below/above Link 1's feet, for a
  // penetration test of the body's real physics.
  if (const char* offset_y = std::getenv("LICHII_SPLIT_OFFSET_Y");
      offset_y != nullptr) {
    planner_settings.debug_offset[1] = std::strtof(offset_y, nullptr);
  }
  if (const char* actor = std::getenv("LICHII_SPLIT_ACTOR"); actor != nullptr) {
    planner_settings.actor = actor;
  }

  CoopPuppet::Configure(planner_settings, true, [](const std::string& line) {
    LOG_INFO(Core, "split: {}", line);
  });
  if (std::getenv("LICHII_SPLIT_MATRIX") != nullptr) {
    // For a puppet with a real physics body: call the game's setter once a
    // frame (the draw-only puppet does not need it).
    CoopPuppet::TheDirector().SetMatrixCall(true);
  }
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
                       const coop::host::PlayerReading& reading, bool event_running,
                       std::uint64_t now_ms) {
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
  if (event_running) {
    // One cutscene, owned by player 1: park Link 2 out of sight and do not
    // move it. After the event the normal target shows it again.
    target->position = g_puppet_input.position;
    target->show = CoopPuppet::Show::hide;
    target->anim.reset();
    g_puppet_input.last_ms = now_ms;
    g_puppet_input.prev_buttons = 0;
    return;
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
  // LICHII_SPLIT_HOLD_MS: after the delay, stop posing the puppet so its own
  // physics can show whether it resolves a penetration.
  if (const char* hold = std::getenv("LICHII_SPLIT_HOLD_MS"); hold != nullptr) {
    const std::uint64_t after = std::strtoull(hold, nullptr, 0);
    if (now_ms - g_puppet_input.test_start_ms > after) {
      target->show = CoopPuppet::Show::hold;
    }
  }
  target->anim = CoopPuppet::Anim{g_puppet_input.anim, 0.0f, 1.0f};
  target->anim_serial = g_puppet_input.serial;
}

std::unordered_set<std::uint64_t> g_scaled_enemies;

// Link's Life component, the same pointer chain as the enemy one.
std::optional<std::pair<int, int>> ReadLife(EmulatorMemory& memory, std::uint64_t actor) {
  const auto components = coop::host::read_ptr(memory, actor + 0x270);
  const auto life = components ? coop::host::read_ptr(memory, *components + 0x2b8)
                               : std::nullopt;
  if (!life) {
    return std::nullopt;
  }
  const auto value_ptr = coop::host::read_ptr(memory, *life + 0x1c10);
  const auto max_ptr = coop::host::read_ptr(memory, *life + 0x1c18);
  if (!value_ptr || !max_ptr) {
    return std::nullopt;
  }
  const auto value = coop::host::read_u32(memory, *value_ptr + 8);
  const auto max = coop::host::read_u32(memory, *max_ptr + 8);
  if (!value || !max || *max == 0 || *max > 0x100000) {
    return std::nullopt;
  }
  return std::pair<int, int>{static_cast<int>(*value), static_cast<int>(*max)};
}

// Give Link 2 the same Life component as Link 1 when its component table
// has an empty Life slot. Both Links then read and write one shared pool.
// LICHII_SPLIT_ATTACH_FREE: the game creates the per-player state class for
// more objects than the registered actors (7 instances, 2 referenced). Find
// an initialized instance no actor's component table points at, and attach it
// to the child's +0x158 so the player manager has a real state object.
std::uint64_t FindFreePlayerState(EmulatorMemory& memory, std::uint64_t main_base) {
  if (main_base == 0) {
    return 0;
  }
  std::unordered_set<std::uint64_t> used;
  const auto manager = coop::host::read_ptr(memory, main_base + 0x4e91888);
  const auto lists = manager ? coop::host::read_ptr(memory, *manager + 0x118) : std::nullopt;
  if (lists) {
    const std::uint64_t head = *lists + 0x670;
    const auto count = coop::host::read_u32(memory, head + 0x10);
    auto node = coop::host::read_ptr(memory, head);
    std::size_t walked = 0;
    while (node && *node != head && count && walked < *count) {
      ++walked;
      const auto owner = coop::host::read_ptr(memory, *node + 0x10);
      if (owner && *owner + 0x538 == *node) {
        const auto table = coop::host::read_ptr(memory, *owner + 0x270);
        if (table) {
          const auto state = coop::host::read_ptr(memory, *table + 0x158);
          if (state) {
            used.insert(*state);
          }
        }
      }
      node = coop::host::read_ptr(memory, *node);
    }
  }
  const std::uint64_t vtable = main_base + 0x4b47608;
  const std::uint64_t actor_vtable = main_base + 0x4b25e18;
  std::vector<std::uint8_t> block(0x10000);
  for (std::uint64_t base = 0x2140000000ull; base < 0x2150000000ull;
       base += block.size()) {
    if (!memory.read(base, block)) {
      continue;
    }
    for (std::size_t off = 0; off + 8 <= block.size(); off += 8) {
      std::uint64_t value = 0;
      std::memcpy(&value, block.data() + off, sizeof(value));
      if (value != vtable) {
        continue;
      }
      const std::uint64_t state = base + off;
      std::uint64_t owner = 0;
      if (!memory.read(state + 0x18, std::span<std::uint8_t>(
                                          reinterpret_cast<std::uint8_t*>(&owner),
                                          sizeof(owner))) ||
          owner == 0) {
        continue;
      }
      std::uint64_t owner_vtable = 0;
      if (!memory.read(owner, std::span<std::uint8_t>(
                                   reinterpret_cast<std::uint8_t*>(&owner_vtable),
                                   sizeof(owner_vtable))) ||
          owner_vtable != actor_vtable) {
        continue;
      }
      if (used.contains(state)) {
        continue;
      }
      LOG_INFO(Core, "split: free player state {:#x} owner {:#x} (used {})", state,
               owner, used.size());
      return state;
    }
  }
  return 0;
}

void BridgeChildLife(EmulatorMemory& memory, std::uint64_t player_actor,
                     std::uint64_t now_ms) {
  if (player_actor == 0) {
    return;
  }
  const auto player_components = coop::host::read_ptr(memory, player_actor + 0x270);
  const auto player_life = player_components
                               ? coop::host::read_ptr(memory, *player_components + 0x2b8)
                               : std::nullopt;
  if (!player_life || *player_life == 0) {
    return;
  }
  static int probe = 0;
  if (probe < 3) {
    ++probe;
    LOG_INFO(Core, "split: life bridge player {:#x} life {:#x}", player_actor,
             *player_life);
  }
  for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
    const bool child_body = slot.actor_name == "Dm_Player_Child_Op" ||
                            slot.actor_name == "CoopLink_Child" ||
                            slot.actor_name == "CoopLink_Adult" ||
                            slot.actor_name == "C";
    if (slot.number != 0 || slot.actor == 0 || !child_body) {
      continue;  // the draw-only puppet has no player-life component
    }
    const auto child_components = coop::host::read_ptr(memory, slot.actor + 0x270);
    const auto child_life = child_components
                                ? coop::host::read_u64(memory, *child_components + 0x2b8)
                                : std::nullopt;
    static int slot_probe = 0;
    if (slot_probe < 6) {
      ++slot_probe;
      LOG_INFO(Core,
               "split: life bridge slot {} actor {:#x} components {:#x} life {:#x}",
               slot.number, slot.actor, *child_components, child_life.value_or(0));
    }
    if (!child_life) {
      continue;  // unreadable
    }
    if (*child_life == 0) {
      const std::uint64_t value = *player_life;
      if (!memory.write(*child_components + 0x2b8,
                        std::span<const std::uint8_t>(
                            reinterpret_cast<const std::uint8_t*>(&value), 8))) {
        continue;
      }
    }
    // The C body's PlayerRef is created but its embedded player-state
    // fields stay 0xffffffff (the player manager never initialized them).
    // LICHII_SPLIT_PLAYERSTATE=1 copies Link 1's initialized state bytes into
    // the child component and points its actor field back at the C body.
    static const bool bridge_state =
        std::getenv("LICHII_SPLIT_PLAYERSTATE") != nullptr;
    if (bridge_state && slot.actor_name == "C") {
      const auto player_ref = coop::host::read_ptr(memory, *player_components + 0x2b0);
      const auto child_ref = coop::host::read_ptr(memory, *child_components + 0x2b0);
      if (player_ref && child_ref && *player_ref != 0 && *child_ref != 0) {
        std::uint64_t sentinel = 0;
        if (memory.read(*child_ref + 0x58,
                        std::span<std::uint8_t>(
                            reinterpret_cast<std::uint8_t*>(&sentinel), 8)) &&
            sentinel == ~std::uint64_t{0}) {
          std::array<std::uint8_t, 0x30> state{};
          if (memory.read(*player_ref + 0x58, state)) {
            const std::uint64_t child = slot.actor;
            std::memcpy(state.data() + 0x18, &child, sizeof(child));
            if (memory.write(*child_ref + 0x58, state)) {
              static std::uint64_t last_state_log = 0;
              if (now_ms - last_state_log >= 5000) {
                last_state_log = now_ms;
                LOG_INFO(Core,
                         "split: player state bridge child {:#x} PlayerRef {:#x} <- "
                         "Link1 PlayerRef {:#x}",
                         slot.actor, *child_ref, *player_ref);
              }
            }
          }
        }
      }
    }
    // Optional: give the real child body the player-only components it
    // lacks (PlayerRef, BSARef, camera target, ...) by sharing Link 1's
    // pointers. A zero child slot is a component the Dm_Player pack does
    // not have; the non-zero slots stay the child's own model/AS/physics.
    static const bool bridge_all =
        std::getenv("LICHII_SPLIT_PLAYERBRIDGE") != nullptr;
    if (bridge_all) {
      std::uint64_t copied = 0;
      std::uint64_t copied_mask = 0;
      for (std::uint64_t off = 0; off < 0x300; off += 8) {
        if (off == 0x2b8) {
          continue;  // Life is already shared.
        }
        std::uint64_t player_value = 0;
        std::uint64_t child_value = 0;
        if (!memory.read(*player_components + off,
                         std::span<std::uint8_t>(
                             reinterpret_cast<std::uint8_t*>(&player_value), 8)) ||
            !memory.read(*child_components + off,
                         std::span<std::uint8_t>(
                             reinterpret_cast<std::uint8_t*>(&child_value), 8))) {
          continue;
        }
        if (player_value != 0 && child_value == 0) {
          if (memory.write(*child_components + off,
                           std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(&player_value), 8))) {
            ++copied;
            if (off / 8 < 64) {
              copied_mask |= std::uint64_t{1} << (off / 8);
            }
          }
        }
      }
      static std::uint64_t last_bridge_log = 0;
      if (copied != 0 && now_ms - last_bridge_log >= 5000) {
        last_bridge_log = now_ms;
        LOG_INFO(Core,
                 "split: player bridge child {:#x} copied {} components mask {:#x}",
                 slot.actor, copied, copied_mask);
      }
    }
    // LICHII_SPLIT_ACTORSTATE: the manager update reads the embedded player
    // sub-object at actor+0x108 and calls its vtable (+0x70). The C actor's
    // +0x108 vtable is zero. Copy Link 1's +0x108..+0x948 block and retarget
    // owner pointers to the C actor.
    static const bool bridge_actor_state =
        std::getenv("LICHII_SPLIT_ACTORSTATE") != nullptr;
    if (bridge_actor_state && slot.actor_name == "C") {
      std::uint64_t child_vtable = 0;
      if (memory.read(slot.actor + 0x108,
                      std::span<std::uint8_t>(
                          reinterpret_cast<std::uint8_t*>(&child_vtable), 8)) &&
          child_vtable == 0) {
        static const std::size_t state_bytes = [] {
          const char* text = std::getenv("LICHII_SPLIT_ACTORSTATE_BYTES");
          const auto value = text != nullptr ? std::strtoull(text, nullptr, 0) : 0x840;
          return static_cast<std::size_t>(std::min<std::uint64_t>(value, 0x840));
        }();
        std::array<std::uint8_t, 0x840> state{};
        if (memory.read(player_actor + 0x108,
                        std::span<std::uint8_t>(state.data(), state_bytes))) {
          const std::uint64_t child = slot.actor;
          for (std::size_t i = 0; i + 8 <= state.size(); i += 8) {
            if (std::memcmp(state.data() + i, &player_actor, 8) == 0) {
              std::memcpy(state.data() + i, &child, 8);
            }
          }
          if (memory.write(slot.actor + 0x108,
                           std::span<const std::uint8_t>(state.data(), state_bytes))) {
            LOG_INFO(Core,
                     "split: actor state bridge child {:#x} <- Link1 {:#x} "
                     "({:#x} bytes, vtable {:#x})",
                     slot.actor, player_actor, state_bytes, child_vtable);
          }
        }
      }
    }
    static std::uint64_t last_log = 0;
    if (now_ms - last_log >= 5000) {
      last_log = now_ms;
      LOG_INFO(Core, "split: shared life child {:#x} -> Link1 life {:#x}",
               slot.actor, *player_life);
    }
  }
}

// The game's whole-hit record (docs/objects.md, kHitTemplate in
// coop_objects.cpp): a raw Life write only flashes the damage overlay; the
// Life step 0x49d8c0 takes a complete hit record and the player's death flow
// sees it.
constexpr std::array<std::uint32_t, 0x98 / 4> kPlayerHitTemplate{
    0x472d7288, 0x00000021, 0x000000b9, 0x00000001,
    0x00060001, 0x40040000, 0x00000014, 0x00000000,
    0x00220000, 0x00001000, 0x0000000a, 0x00000000,
    0x00000200, 0x00000000, 0x00000000, 0xc21085f6,
    0x42c63e88, 0x40501269, 0x3e76ed80, 0xb3c00000,
    0xbf787210, 0x40588db0, 0xc1129418, 0xc0cb90a0,
    0xc210600c, 0x42c45791, 0x409352f5, 0xbdcc4eb0,
    0x3ecc555c, 0xc0d481b8, 0x000343ff, 0x00040000,
    0x00000004, 0x000000be, 0x00000002, 0x03000000,
    0x03010102, 0x00030000,
};

void WritePlayerHit(EmulatorMemory& memory, std::uint64_t actor,
                    const std::array<float, 3>& at, std::int32_t amount) {
  const auto components = coop::host::read_ptr(memory, actor + 0x270);
  const auto life = components ? coop::host::read_ptr(memory, *components + 0x2b8)
                               : std::nullopt;
  if (!life) {
    return;
  }
  std::uint32_t last = 0;
  if (!memory.read(*life + 0x1b18, std::span<std::uint8_t>(
                                       reinterpret_cast<std::uint8_t*>(&last), 4))) {
    return;
  }
  const std::uint32_t id = (last == 0xffffffffu ? 0x1000u : last + 2u) & 0x7fffffffu;
  auto rec = kPlayerHitTemplate;
  std::memcpy(&rec[0], &actor, 8);
  rec[2] = id;
  rec[0x2c4 / 4 - 0x240 / 4] = id;
  const std::array<float, 3> hit_at{at[0], at[1] + 1.0f, at[2]};
  std::memcpy(&rec[0x27c / 4 - 0x240 / 4], hit_at.data(), 12);
  std::memcpy(&rec[0x2a0 / 4 - 0x240 / 4], at.data(), 12);
  rec[3] = 0;
  const std::array<std::uint32_t, 4> header{0x00060001, 0x40040000, id, 2};
  const std::array<std::int32_t, 2> damage{4, amount};
  const std::uint32_t present = 1;
  if (!memory.write(*life + 0x1f8, std::span<const std::uint8_t>(
                                       reinterpret_cast<const std::uint8_t*>(header.data()), 16)) ||
      !memory.write(*life + 0x224, std::span<const std::uint8_t>(
                                       reinterpret_cast<const std::uint8_t*>(damage.data()), 8)) ||
      !memory.write(*life + 0x240, std::span<const std::uint8_t>(
                                       reinterpret_cast<const std::uint8_t*>(rec.data()),
                                       rec.size() * 4)) ||
      !memory.write(*life + 0x24c, std::span<const std::uint8_t>(
                                       reinterpret_cast<const std::uint8_t*>(&present), 4))) {
    return;
  }
  LOG_INFO(Core, "split: debug player hit amount {} id {:#x}", amount, id);
}

// Debug: copy the active player link's holder/id into the empty manager link.
// This tests whether the player manager accepts a second link before we clone
// the holder/actor; LICHII_SPLIT_LINKCOPY=1.
void DebugCopyActiveLink(EmulatorMemory& memory, std::uint64_t main_base,
                         std::uint64_t now_ms) {
  static const bool on = std::getenv("LICHII_SPLIT_LINKCOPY") != nullptr ||
                         std::getenv("LICHII_SPLIT_LINKCLONE") != nullptr ||
                         std::getenv("LICHII_SPLIT_CHILDHOLDER") != nullptr ||
                         std::getenv("LICHII_LOG_LINKS") != nullptr;
  if (!on || main_base == 0) {
    return;
  }
  static std::uint64_t last_ms = 0;
  if (last_ms != 0 && now_ms - last_ms < 1000) {
    return;
  }
  last_ms = now_ms;
  const auto scene = coop::host::read_ptr(memory, main_base + 0x4e97648);
  const auto table = scene ? coop::host::read_ptr(memory, *scene + 0x1f0) : std::nullopt;
  const auto count = table ? coop::host::read_u32(memory, *table + 0x50) : std::nullopt;
  const auto array = table ? coop::host::read_ptr(memory, *table + 0x58) : std::nullopt;
  const auto manager = array ? coop::host::read_ptr(memory, *array + 0x78) : std::nullopt;
  const auto must = manager ? coop::host::read_u64(memory, *manager + 0x30) : std::nullopt;
  if (!scene || !table || !count || *count < 0x10 || !array || !manager || !must ||
      *must == 0) {
    return;
  }
  if (std::getenv("LICHII_LOG_LINKS") != nullptr) {
    static std::uint64_t last_manager_log = 0;
    if (now_ms - last_manager_log >= 1000) {
      last_manager_log = now_ms;
      const auto link0 = coop::host::read_ptr(memory, *manager + 0x40);
      const auto link1 = coop::host::read_ptr(memory, *manager + 0x50);
      LOG_INFO(Core, "split: link manager {:#x} slots {:#x} {:#x}", *manager,
               link0.value_or(0), link1.value_or(0));
    }
  }
  std::uint64_t active_link = 0, active_holder = 0, empty_link = 0;
  std::uint32_t active_id = 0;
  for (int i = 0; i < 2; ++i) {
    const auto link = coop::host::read_ptr(memory, *manager + 0x40 + i * 0x10);
    if (!link) {
      continue;
    }
    const auto holder = coop::host::read_ptr(memory, *link + 0x8);
    const auto id = coop::host::read_u32(memory, *link + 0x10);
    if (!holder || !id || *id == 0xffffffffu) {
      empty_link = *link;
      continue;
    }
    const auto actor = coop::host::read_ptr(memory, *holder + 0x40);
    const auto state = actor ? coop::host::read_u32(memory, *actor + 0x2c) : std::nullopt;
    if (actor && state && *state == 4) {
      active_link = *link;
      active_holder = *holder;
      active_id = *id;
    } else {
      empty_link = *link;
    }
  }
  if (active_link == 0 || empty_link == 0) {
    return;
  }
  if (std::getenv("LICHII_LOG_LINKS") != nullptr) {
    static std::uint64_t last_links_log = 0;
    if (now_ms - last_links_log >= 1000) {
      last_links_log = now_ms;
      LOG_INFO(Core, "split: links active {:#x} holder {:#x} empty {:#x}",
               active_link, active_holder, empty_link);
    }
  }
  // LICHII_SPLIT_REAL_STATE: replace the shallow clone with a fresh state
  // built by the game's own constructor (main+0x2dc6f90). The constructor
  // zeroes/initializes the callback slots and the lock; we then restore the
  // base vtable the manager expects and point the owner at the child.
  if (std::getenv("LICHII_SPLIT_REAL_STATE") != nullptr) {
    std::uint64_t child = 0;
    std::uint64_t block = 0;
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
      if (slot.actor != 0) {
        child = slot.actor;
        block = slot.block;
      }
    }
    if (child != 0 && block != 0) {
      const std::uint64_t scratch = block + 0x700;
      static std::uint64_t ctor_queued_for = 0;
      const auto vtable = coop::host::read_ptr(memory, scratch);
      if (ctor_queued_for != scratch) {
        const std::array<std::uint8_t, 0x1a0> zero{};
        memory.write(scratch, zero);
        CoopPuppet::Call call;
        call.function = main_base + 0x2dc6f90;
        call.x[0] = scratch;
        CoopPuppet::TheDirector().QueueCall(call);
        ctor_queued_for = scratch;
        LOG_INFO(Core, "split: real state ctor queued at {:#x} for child {:#x}",
                 scratch, child);
      } else if (vtable && *vtable != main_base + 0x4b47608) {
        LOG_INFO(Core, "split: real state ctor vtable {:#x} at {:#x}", *vtable, scratch);
        const auto player_actor =
            active_holder ? coop::host::read_ptr(memory, active_holder + 0x40) : std::nullopt;
        const auto source_table =
            player_actor ? coop::host::read_ptr(memory, *player_actor + 0x270) : std::nullopt;
        const auto source_state =
            source_table ? coop::host::read_ptr(memory, *source_table + 0x158) : std::nullopt;
        if (std::getenv("LICHII_SPLIT_RETARGET_358") != nullptr && source_table) {
          if (const auto component = coop::host::read_ptr(memory, *source_table + 0x358);
              component && *component != 0) {
            const std::uint64_t owner = child;
            if (memory.write(*component + 0x18,
                             std::span<const std::uint8_t>(
                                 reinterpret_cast<const std::uint8_t*>(&owner),
                                 sizeof(owner)))) {
              LOG_INFO(Core, "split: retargeted shared +0x358 owner to child {:#x}",
                       child);
            }
          }
        }
        if (source_state && *source_state) {
          for (const std::uint64_t off : {0x8, 0x10, 0x20, 0x28}) {
            if (const auto v = coop::host::read_ptr(memory, *source_state + off); v && *v != 0) {
              const std::uint64_t value = *v;
              memory.write(scratch + off,
                           std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
            }
          }
          LOG_INFO(Core, "split: real state player fields copied from {:#x}",
                   *source_state);
        }
        const std::uint64_t base_vtable = main_base + 0x4b47608;
        const std::uint64_t owner = child;
        const auto child_table = coop::host::read_ptr(memory, child + 0x270);
        if (child_table &&
            memory.write(scratch,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(&base_vtable),
                             sizeof(base_vtable))) &&
            memory.write(scratch + 0x18,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(&owner), sizeof(owner))) &&
            memory.write(*child_table + 0x158,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(&scratch), sizeof(scratch)))) {
          LOG_INFO(Core,
                   "split: real state {:#x} base vtable attached to child {:#x}",
                   scratch, child);
        }
      }
    }
  }

  // LICHII_SPLIT_PRIVATE_CONTEXT: actor_table+0x358 is Link 1's Player
  // component, allocated 0x358 bytes into a 0x8000-byte Player-system block
  // that also holds its 30 blackboard nodes. Copy the whole block into
  // block+0x3000, relocate every pointer into the source range, point the
  // copied component's owner at C and attach it to the child's table.
  if (std::getenv("LICHII_SPLIT_PRIVATE_CONTEXT") != nullptr) {
    std::uint64_t child = 0;
    std::uint64_t block = 0;
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
      if (slot.actor != 0) {
        child = slot.actor;
        block = slot.block;
      }
    }
    if (child != 0 && block != 0) {
      constexpr std::uint64_t kComponentOffset = 0x358;
      constexpr std::uint64_t kArenaSize = 0x8000;
      const std::uint64_t arena = block + 0x3000;
      const std::uint64_t context = arena + kComponentOffset;
      static std::uint64_t context_done_for = 0;
      if (context_done_for != context) {
        std::uint64_t shared_context = 0;
        if (active_holder) {
          if (const auto actor = coop::host::read_ptr(memory, active_holder + 0x40);
              actor) {
            if (const auto actor_table = coop::host::read_ptr(memory, *actor + 0x270);
                actor_table) {
              if (const auto component =
                      coop::host::read_ptr(memory, *actor_table + 0x358);
                  component) {
                shared_context = *component;
              }
            }
          }
        }
        if (shared_context >= kComponentOffset) {
          const std::uint64_t shared_arena = shared_context - kComponentOffset;
          std::array<std::uint8_t, kArenaSize> bytes{};
          if (memory.read(shared_arena, bytes)) {
            const std::int64_t delta = static_cast<std::int64_t>(arena) -
                                       static_cast<std::int64_t>(shared_arena);
            for (std::size_t off = 0; off + sizeof(std::uint64_t) <= bytes.size();
                 off += sizeof(std::uint64_t)) {
              std::uint64_t value = 0;
              std::memcpy(&value, bytes.data() + off, sizeof(value));
              const std::uint64_t base = value & ~std::uint64_t{7};
              if (base >= shared_arena && base < shared_arena + kArenaSize) {
                value = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(value) + delta);
                std::memcpy(bytes.data() + off, &value, sizeof(value));
              }
            }
            if (memory.write(arena, bytes)) {
              const std::uint64_t owner = child;
              const auto child_table = coop::host::read_ptr(memory, child + 0x270);
              if (child_table &&
                  memory.write(context + 0x18,
                               std::span<const std::uint8_t>(
                                   reinterpret_cast<const std::uint8_t*>(&owner),
                                   sizeof(owner))) &&
                  memory.write(*child_table + 0x358,
                               std::span<const std::uint8_t>(
                                   reinterpret_cast<const std::uint8_t*>(&context),
                                   sizeof(context)))) {
                context_done_for = context;
                LOG_INFO(Core,
                         "split: private player arena copy {:#x} <- {:#x} "
                         "({:#x} bytes), component {:#x} attached to child {:#x}",
                         arena, shared_arena, kArenaSize, context, child);
              }
            }
          }
        }
      }
    }
  }

  // Register the child's own game-created holder. The actor keeps it at
  // +0x1b0; the holder has the actor/id at +0x40/+0x48 and the sub-actor
  // holder at +0xa0. No holder bytes are copied.
  const bool child_holder = std::getenv("LICHII_SPLIT_CHILDHOLDER") != nullptr;
  if (child_holder) {
    std::uint64_t child = 0;
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
      if (slot.actor != 0 && (slot.actor_name == "Dm_Player_Child_Op" ||
                              slot.actor_name == "CoopLink_Child" ||
                              slot.actor_name == "C")) {
        child = slot.actor;
      }
    }
    if (child == 0) {
      return;
    }
    // LICHII_SPLIT_BIND_HOLD_MS: hold the bind for a test window after the
    // child first appears, so a read watchpoint can be armed on the child's
    // components+0x158 slot before the manager processes the link.
    if (const char* hold_env = std::getenv("LICHII_SPLIT_BIND_HOLD_MS");
        hold_env != nullptr) {
      static std::uint64_t hold_start = 0;
      if (hold_start == 0) {
        hold_start = now_ms;
      }
      if (now_ms - hold_start < std::strtoull(hold_env, nullptr, 0)) {
        return;
      }
    }
    std::uint64_t holder = 0;
    std::uint32_t holder_id = 0;
    if (!memory.read(child + 0x1b0, std::span<std::uint8_t>(
                                        reinterpret_cast<std::uint8_t*>(&holder), 8)) ||
        holder == 0 ||
        !memory.read(holder + 0x48, std::span<std::uint8_t>(
                                         reinterpret_cast<std::uint8_t*>(&holder_id), 4))) {
      return;
    }
    // LICHII_SPLIT_PRIVATE_STATE: clone Link 1's real player state into the
    // puppet block's spare area, reinitialize its lock at +0x98, point the
    // owner at the child and attach it to the child's table +0x158.
    if (std::getenv("LICHII_SPLIT_PRIVATE_STATE") != nullptr &&
        std::getenv("LICHII_SPLIT_SHARE_358") == nullptr) {
      std::uint64_t block = 0;
      for (const auto& slot_view : CoopPuppet::TheDirector().Slots()) {
        if (slot_view.actor == child) {
          block = slot_view.block;
        }
      }
      if (block == 0) {
        return;
      }
      const std::uint64_t scratch = block + 0x700;
      std::uint64_t player_actor = 0;
      std::uint64_t player_table = 0;
      std::uint64_t player_state = 0;
      if (memory.read(active_holder + 0x40,
                      std::span<std::uint8_t>(
                          reinterpret_cast<std::uint8_t*>(&player_actor), 8)) &&
          player_actor != 0 &&
          memory.read(player_actor + 0x270,
                      std::span<std::uint8_t>(
                          reinterpret_cast<std::uint8_t*>(&player_table), 8)) &&
          player_table != 0 &&
          memory.read(player_table + 0x158,
                      std::span<std::uint8_t>(
                          reinterpret_cast<std::uint8_t*>(&player_state), 8)) &&
          player_state != 0) {
        static std::uint64_t cloned_for = 0;
        if (cloned_for != scratch) {
          std::array<std::uint8_t, 0x1a0> bytes{};
          if (memory.read(player_state, bytes) &&
              memory.write(scratch, bytes)) {
            const std::array<std::uint8_t, 0x40> zero{};
            memory.write(scratch + 0x98, zero);
            CoopPuppet::Call call;
            call.function = main_base + 0x3c42490;
            call.x[0] = scratch + 0x98;
            CoopPuppet::TheDirector().QueueCall(call);
            cloned_for = scratch;
            LOG_INFO(Core,
                     "split: private state clone {:#x} <- Link1 state {:#x}, "
                     "lock init queued",
                     scratch, player_state);
          }
          return;
        }
        std::uint64_t child_table = 0;
        const std::uint64_t owner = child;
        if (memory.read(child + 0x270, std::span<std::uint8_t>(
                                          reinterpret_cast<std::uint8_t*>(&child_table), 8)) &&
            child_table != 0 &&
            memory.write(scratch + 0x18, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(&owner), 8)) &&
            memory.write(child_table + 0x158,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(&scratch), 8))) {
          static std::uint64_t attached_for = 0;
          if (attached_for != scratch) {
            attached_for = scratch;
            LOG_INFO(Core,
                     "split: private state {:#x} attached to child {:#x} table {:#x}",
                     scratch, child, child_table);
          }
        }
      }
    }
    if (std::getenv("LICHII_SPLIT_SUBSHARE") != nullptr) {
      std::uint64_t active_sub = 0;
      std::uint32_t active_sub_id = 0;
      if (memory.read(active_holder + 0xa0, std::span<std::uint8_t>(
                                              reinterpret_cast<std::uint8_t*>(&active_sub), 8)) &&
          active_sub != 0 &&
          memory.read(active_holder + 0xa8, std::span<std::uint8_t>(
                                               reinterpret_cast<std::uint8_t*>(&active_sub_id), 4))) {
        memory.write(holder + 0xa0, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(&active_sub), 8));
        memory.write(holder + 0xa8, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(&active_sub_id), 4));
        LOG_INFO(Core, "split: childholder sub shared {:#x} id {:#x}", active_sub,
                 active_sub_id);
      }
    }
    // LICHII_SPLIT_HOLDERMATCH: the active player holder has 0 at +0x28/
    // +0x38 and a player-state word at +0xb0. The generic child holder has
    // 1/0x81ff and no +0xb0; matching the active holder's non-actor fields
    // tests whether those are what the manager's player update reads.
    if (std::getenv("LICHII_SPLIT_HOLDERMATCH") != nullptr) {
      const std::uint64_t zero = 0;
      std::uint64_t b0 = 0;
      if (memory.read(active_holder + 0xb0,
                      std::span<std::uint8_t>(
                          reinterpret_cast<std::uint8_t*>(&b0), 8))) {
        memory.write(holder + 0x28, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(&zero), 8));
        memory.write(holder + 0x38, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(&zero), 8));
        memory.write(holder + 0xb0, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(&b0), 8));
        LOG_INFO(Core, "split: childholder fields matched +0x28/0x38=0 +0xb0={:#x}",
                 b0);
      }
    }
    const std::uint64_t name_ptr = empty_link + 0x2c;
    const std::uint32_t name_cap = 0x40;
    std::array<char, 0x40> link_name{};
    const char* link_text = std::getenv("LICHII_SPLIT_LINKNAME");
    if (link_text == nullptr || link_text[0] == '\0') {
      link_text = "Dm_Player_Child_Op";
    }
    std::memcpy(link_name.data(), link_text,
                std::min<std::size_t>(std::strlen(link_text), link_name.size() - 1));
    bool name_ok =
        memory.write(empty_link + 0x20, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(&name_ptr), 8)) &&
        memory.write(empty_link + 0x28, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(&name_cap), 4)) &&
        memory.write(empty_link + 0x2c, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(link_name.data()),
                                             link_name.size()));
    if (!name_ok) {
      return;
    }
    // LICHII_SPLIT_ATTACH_FREE: attach an initialized, unused player-state
    // instance before the deferred bind runs.
    if (std::getenv("LICHII_SPLIT_ATTACH_FREE") != nullptr) {
      std::uint64_t child_table = 0;
      std::uint64_t child_state = 0;
      if (memory.read(child + 0x270, std::span<std::uint8_t>(
                                        reinterpret_cast<std::uint8_t*>(&child_table), 8)) &&
          child_table != 0 &&
          memory.read(child_table + 0x158, std::span<std::uint8_t>(
                                          reinterpret_cast<std::uint8_t*>(&child_state), 8)) &&
          child_state == 0) {
        const std::uint64_t free_state = FindFreePlayerState(memory, g_main_base);
        if (free_state != 0 &&
            memory.write(child_table + 0x158, std::span<const std::uint8_t>(
                                                 reinterpret_cast<const std::uint8_t*>(&free_state),
                                                 sizeof(free_state)))) {
          LOG_INFO(Core, "split: attached free player state {:#x} to child {:#x}",
                   free_state, child);
          // LICHII_SPLIT_SHARE_STATE100: Link 1's state carries a per-player
          // object at +0x100 that every unused instance has as 0. Copy it to
          // test whether that is the object the manager locks.
          if (std::getenv("LICHII_SPLIT_SHARE_STATE100") != nullptr) {
            std::uint64_t player_actor = 0;
            std::uint64_t player_table = 0;
            std::uint64_t player_state = 0;
            std::uint64_t sub = 0;
            if (memory.read(active_holder + 0x40,
                            std::span<std::uint8_t>(
                                reinterpret_cast<std::uint8_t*>(&player_actor), 8)) &&
                player_actor != 0 &&
                memory.read(player_actor + 0x270,
                            std::span<std::uint8_t>(
                                reinterpret_cast<std::uint8_t*>(&player_table), 8)) &&
                player_table != 0 &&
                memory.read(player_table + 0x158,
                            std::span<std::uint8_t>(
                                reinterpret_cast<std::uint8_t*>(&player_state), 8)) &&
                player_state != 0 &&
                memory.read(player_state + 0x100,
                            std::span<std::uint8_t>(
                                reinterpret_cast<std::uint8_t*>(&sub), 8)) &&
                sub != 0 &&
                memory.write(free_state + 0x100,
                             std::span<const std::uint8_t>(
                                 reinterpret_cast<const std::uint8_t*>(&sub), 8))) {
              LOG_INFO(Core, "split: state +0x100 {:#x} shared into {:#x}", sub,
                       free_state);
            }
          }
        }
      }
    }
    // LICHII_SPLIT_WAIT_STATE: do not bind until the child's component table
    // has the per-player state at +0x158. That lets a test attach a real
    // game-created state first; binding without it crashes the manager.
    if (std::getenv("LICHII_SPLIT_WAIT_STATE") != nullptr) {
      std::uint64_t child_table = 0;
      std::uint64_t child_state = 0;
      if (!memory.read(child + 0x270, std::span<std::uint8_t>(
                                        reinterpret_cast<std::uint8_t*>(&child_table), 8)) ||
          child_table == 0 ||
          !memory.read(child_table + 0x158, std::span<std::uint8_t>(
                                          reinterpret_cast<std::uint8_t*>(&child_state), 8)) ||
          child_state == 0) {
        return;
      }
    }
    // LICHII_SPLIT_BIND: use the game's own player-link binder instead of
    // writing the holder/id. main+0x2bc440(link, actor, 0) is what the
    // player-entry code calls when Link 1 spawns (caught by a watchpoint on
    // the empty link's holder field); it performs the same state/ID checks
    // and any bookkeeping we cannot see.
    if (std::getenv("LICHII_SPLIT_BIND") != nullptr) {
      CoopPuppet::Call call;
      call.function = main_base + 0x2bc440;
      call.x[0] = empty_link;
      call.x[1] = child;
      call.x[2] = 0;
      CoopPuppet::TheDirector().QueueCall(call);
      static std::uint64_t last_bind_log = 0;
      if (now_ms - last_bind_log >= 5000) {
        last_bind_log = now_ms;
        LOG_INFO(Core,
                 "split: childholder bind queued link {:#x} child {:#x} fn {:#x}",
                 empty_link, child, call.function);
      }
      return;
    }
    if (!memory.write(empty_link + 0x8, std::span<const std::uint8_t>(
                                            reinterpret_cast<const std::uint8_t*>(&holder), 8)) ||
        !memory.write(empty_link + 0x10, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(&holder_id), 4))) {
      return;
    }
    static std::uint64_t child_logged = 0;
    if (now_ms - child_logged >= 5000) {
      child_logged = now_ms;
      LOG_INFO(Core,
               "split: childholder empty {:#x} <- child {:#x} holder {:#x} id {:#x}",
               empty_link, child, holder, holder_id);
    }
    return;
  }
  const bool clone = std::getenv("LICHII_SPLIT_LINKCLONE") != nullptr;
  if (clone) {
    std::uint64_t block = 0, child = 0;
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
      if (slot.block != 0 && block == 0) {
        block = slot.block;
      }
      if (slot.actor != 0 && slot.actor_name == "Dm_Player_Child_Op") {
        child = slot.actor;
      }
    }
    if (block == 0 || child == 0) {
      return;
    }
    const auto sub_src = coop::host::read_ptr(memory, active_holder + 0xa0);
    if (!sub_src) {
      return;
    }
    const std::uint64_t holder_copy = block + 0x700;
    const std::uint64_t sub_copy = block + 0x800;
    std::array<std::uint8_t, 0x100> holder_bytes{};
    std::array<std::uint8_t, 0x100> sub_bytes{};
    if (!memory.read(active_holder, std::span<std::uint8_t>(holder_bytes.data(),
                                                            holder_bytes.size())) ||
        !memory.read(*sub_src, std::span<std::uint8_t>(sub_bytes.data(), sub_bytes.size()))) {
      return;
    }
    if (!memory.write(holder_copy, std::span<const std::uint8_t>(holder_bytes.data(),
                                                                holder_bytes.size())) ||
        !memory.write(sub_copy, std::span<const std::uint8_t>(sub_bytes.data(),
                                                             sub_bytes.size()))) {
      return;
    }
    const std::uint64_t copy_holder = holder_copy;
    const std::uint64_t copy_sub = sub_copy;
    const std::uint32_t copy_id = 0xcc;
    const std::uint32_t copy_sub_id = 0xcd;
    memory.write(holder_copy + 0xa0, std::span<const std::uint8_t>(
                                         reinterpret_cast<const std::uint8_t*>(&copy_sub), 8));
    memory.write(holder_copy + 0xa8, std::span<const std::uint8_t>(
                                         reinterpret_cast<const std::uint8_t*>(&copy_sub_id), 4));
    memory.write(holder_copy + 0x40, std::span<const std::uint8_t>(
                                         reinterpret_cast<const std::uint8_t*>(&child), 8));
    memory.write(holder_copy + 0x48, std::span<const std::uint8_t>(
                                         reinterpret_cast<const std::uint8_t*>(&copy_id), 4));
    // If the sub-holder is another actor link, point it at the child too.
    const auto active_actor = coop::host::read_ptr(memory, active_holder + 0x40);
    const auto sub_actor = coop::host::read_ptr(memory, *sub_src + 0x40);
    if (active_actor && sub_actor && *sub_actor == *active_actor) {
      memory.write(sub_copy + 0x40, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(&child), 8));
      memory.write(sub_copy + 0x48, std::span<const std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(&copy_sub_id), 4));
    }
    if (!memory.write(empty_link + 0x8, std::span<const std::uint8_t>(
                                            reinterpret_cast<const std::uint8_t*>(&copy_holder), 8)) ||
        !memory.write(empty_link + 0x10, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(&copy_id), 4))) {
      return;
    }
    // The link also carries the actor/link user name the manager resolves.
    // The active link points +0x20 at its name at +0x2c; an unnamed empty
    // link made the manager dereference a null player component.
    const std::uint64_t name_ptr = empty_link + 0x2c;
    const std::uint32_t name_cap = 0x40;
    std::array<char, 0x40> link_name{};
    const char* link_text = "Dm_Player_Child_Op";
    std::memcpy(link_name.data(), link_text, std::strlen(link_text));
    if (!memory.write(empty_link + 0x20, std::span<const std::uint8_t>(
                                            reinterpret_cast<const std::uint8_t*>(&name_ptr), 8)) ||
        !memory.write(empty_link + 0x28, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(&name_cap), 4)) ||
        !memory.write(empty_link + 0x2c, std::span<const std::uint8_t>(
                                             reinterpret_cast<const std::uint8_t*>(link_name.data()),
                                             link_name.size()))) {
      return;
    }
    static std::uint64_t clone_logged = 0;
    if (now_ms - clone_logged >= 5000) {
      clone_logged = now_ms;
      LOG_INFO(Core,
               "split: linkclone empty {:#x} -> holder {:#x} sub {:#x} child {:#x} id {:#x}",
               empty_link, holder_copy, sub_copy, child, copy_id);
    }
    return;
  }
  const std::uint64_t holder_value = active_holder;
  const std::uint32_t id_value = active_id;
  if (!memory.write(empty_link + 0x8, std::span<const std::uint8_t>(
                                          reinterpret_cast<const std::uint8_t*>(&holder_value), 8)) ||
      !memory.write(empty_link + 0x10, std::span<const std::uint8_t>(
                                           reinterpret_cast<const std::uint8_t*>(&id_value), 4))) {
    LOG_INFO(Core, "split: linkcopy write failed empty {:#x}", empty_link);
    return;
  }
  static std::uint64_t logged_ms = 0;
  if (now_ms - logged_ms >= 5000) {
    logged_ms = now_ms;
    LOG_INFO(Core,
             "split: linkcopy empty {:#x} <- active {:#x} holder {:#x} id {:#x}",
             empty_link, active_link, active_holder, active_id);
  }
}

// Play an AS command (Dead/Die) on Link 1 through the puppet director's
// code cave. The player AS slot is the game's own death animation; the
// PlayerRef/BSA then runs its death state.
bool PlayPlayerDeath(EmulatorMemory& memory, std::uint64_t main_base,
                     std::uint64_t player_actor) {
  const char* name = std::getenv("LICHII_SPLIT_DEATH_AS");
  if (name == nullptr || name[0] == '\0' || player_actor == 0 || main_base == 0) {
    return false;
  }
  std::uint64_t block = 0;
  for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
    if (slot.block != 0) {
      block = slot.block;
      break;
    }
  }
  if (block == 0) {
    return false;
  }
  const std::uint64_t text = block + CoopPuppet::block::kNameText;
  std::array<char, CoopPuppet::block::kNameCapacity> chars{};
  const std::size_t length = std::min(std::strlen(name), chars.size() - 1);
  std::memcpy(chars.data(), name, length);
  if (!memory.write(text, std::span<const std::uint8_t>(
                              reinterpret_cast<const std::uint8_t*>(chars.data()),
                              chars.size()))) {
    return false;
  }
  std::array<std::uint8_t, 0x30> request{};
  const std::int32_t index = 0;
  const float rate = 1.0f;
  const float unset = -1.0f;
  std::memcpy(request.data() + 0x00, &player_actor, 8);
  std::memcpy(request.data() + 0x08, &text, 8);
  std::memcpy(request.data() + 0x10, &index, 4);
  std::memcpy(request.data() + 0x14, &rate, 4);
  std::memcpy(request.data() + 0x18, &unset, 4);
  std::memcpy(request.data() + 0x1c, &unset, 4);
  const std::uint64_t at = block + CoopPuppet::block::kPlayRequest;
  if (!memory.write(at, std::span<const std::uint8_t>(request.data(), request.size()))) {
    return false;
  }
  CoopPuppet::Call call;
  call.function = main_base + 0x2022a0;  // kPlayAs
  call.x[0] = at;
  CoopPuppet::TheDirector().QueueCall(call);
  LOG_INFO(Core, "split: death AS '{}' queued on Link1 {:#x}", name, player_actor);
  return true;
}

// Debug: drive Link 1's own Life/death path. LICHII_SPLIT_HIT writes the
// game's hit record; LICHII_SPLIT_DRAIN writes the value directly.
void DebugDrainHealth(EmulatorMemory& memory, std::uint64_t player_actor,
                      std::uint64_t now_ms, bool can_drain) {
  // LICHII_SPLIT_HITCHILD: one game-native hit record on the C body, five
  // seconds after it exists. Its Life slot is the shared Link 1 object, so a
  // successful hit must move the shared pool and the HUD.
  static const bool hit_child =
      std::getenv("LICHII_SPLIT_HITCHILD") != nullptr;
  if (hit_child) {
    static bool done = false;
    static std::uint64_t first_ms = 0;
    if (done) {
      return;
    }
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
      if (slot.actor == 0 || slot.actor_name != "C") {
        continue;
      }
      if (first_ms == 0) {
        first_ms = now_ms;
        return;
      }
      if (now_ms - first_ms < 5000) {
        return;
      }
      std::array<float, 3> at{};
      if (memory.read(slot.actor + 0x31c,
                      std::span<std::uint8_t>(
                          reinterpret_cast<std::uint8_t*>(at.data()), 12))) {
        // The director hides a new puppet under the floor until the scene is
        // drawn; a hit record there is out of the world.
        if (at[1] < -100.0f) {
          return;
        }
        WritePlayerHit(memory, slot.actor, at, 8);
        LOG_INFO(Core,
                 "split: debug hit child {:#x} at {:.1f} {:.1f} {:.1f}",
                 slot.actor, at[0], at[1], at[2]);
        done = true;
      }
      return;
    }
    return;
  }
  static const bool drain = std::getenv("LICHII_SPLIT_DRAIN") != nullptr;
  static const bool hit = std::getenv("LICHII_SPLIT_HIT") != nullptr;
  if ((!drain && !hit) || player_actor == 0 || !can_drain) {
    return;
  }
  // LICHII_SPLIT_DRAIN_WAIT_CHILD=1: hold the drain until the C child body
  // exists, so the shared Life pointer and the before/after value are both
  // observable in the same run.
  static const bool wait_child =
      std::getenv("LICHII_SPLIT_DRAIN_WAIT_CHILD") != nullptr;
  if (drain && wait_child) {
    bool child_alive = false;
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
      if (slot.actor != 0 && slot.actor_name == "C") {
        child_alive = true;
      }
    }
    if (!child_alive) {
      return;
    }
  }
  static std::uint64_t last_ms = 0;
  if (last_ms != 0 && now_ms - last_ms < 1000) {
    return;
  }
  last_ms = now_ms;
  if (hit) {
    std::array<float, 3> at{};
    if (memory.read(player_actor + 0x31c, std::span<std::uint8_t>(
                                               reinterpret_cast<std::uint8_t*>(at.data()), 12))) {
      WritePlayerHit(memory, player_actor, at, 32);
    }
    return;
  }
  // One shot: once the pool reaches 0, let the game over/Continue flow run
  // without draining the respawned Link.
  static bool drained = false;
  if (drained) {
    return;
  }
  const auto components = coop::host::read_ptr(memory, player_actor + 0x270);
  const auto life = components ? coop::host::read_ptr(memory, *components + 0x2b8)
                               : std::nullopt;
  // The game's own status path: PlayerStatus.Life (int32, per player).
  constexpr std::uint32_t kPlayerLife = 0x31ab5580;
  coop::host::GameFlags flags(memory);
  const auto flag_life = flags.get_int(kPlayerLife);
  if (flag_life) {
    const std::int32_t next = *flag_life > 4 ? *flag_life - 4 : 0;
    const bool queued = coop::host::GameFlags::set_int(memory, kPlayerLife, next);
    LOG_INFO(Core, "split: debug drain flag life {} -> {} (queued {})", *flag_life,
             next, queued);
    // Continue with the actor Life too: the status flag is a save copy and
    // does not drive the live death path.
  } else {
    static bool logged = false;
    if (!logged) {
      logged = true;
      LOG_INFO(Core, "split: PlayerStatus.Life flag not readable; using actor life");
    }
  }
  const auto value_ptr = life ? coop::host::read_ptr(memory, *life + 0x1c10)
                              : std::nullopt;
  if (!value_ptr) {
    return;
  }
  std::uint32_t value = 0;
  if (!memory.read(*value_ptr + 8, std::span<std::uint8_t>(
                                       reinterpret_cast<std::uint8_t*>(&value), 4))) {
    return;
  }
  if (value == 0) {
    drained = true;
    LOG_INFO(Core, "split: debug drain finished at 0");
    return;
  }
  value = value > 4 ? value - 4 : 0;
  if (memory.write(*value_ptr + 8, std::span<const std::uint8_t>(
                                       reinterpret_cast<const std::uint8_t*>(&value), 4))) {
    LOG_INFO(Core, "split: debug drain health to {}", value);
    if (value == 0) {
      drained = true;
      LOG_INFO(Core, "split: debug drain finished at 0");
    }
  }
}

std::optional<std::string> ActorName(EmulatorMemory& memory, std::uint64_t actor) {
  const auto name = coop::host::read_ptr(memory, actor + 0x260);
  if (!name) {
    return std::nullopt;
  }
  std::array<char, 64> text{};
  if (!memory.read(*name, std::span<std::uint8_t>(
                              reinterpret_cast<std::uint8_t*>(text.data()), text.size()))) {
    return std::nullopt;
  }
  std::string out;
  for (const char c : text) {
    if (c == '\0') {
      break;
    }
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
      return std::nullopt;
    }
    out.push_back(c);
  }
  return out.empty() ? std::nullopt : std::optional<std::string>(std::move(out));
}

// Scale each enemy's Life once, when it first appears in the actor list.
// Bosses are `Enemy_DungeonBoss*` / `Boss_*`; breakables and NPCs are left
// alone so pots and signs still take one hit.
void ScaleEnemyLife(EmulatorMemory& memory, std::uint64_t now_ms) {
  if (g_settings.enemy_hp <= 1.0f) {
    return;
  }
  static std::uint64_t last_ms = 0;
  if (last_ms != 0 && now_ms - last_ms < 500) {
    return;
  }
  last_ms = now_ms;

  const auto manager = coop::host::read_ptr(memory, g_main_base + 0x4e91888);
  if (!manager) {
    return;
  }
  const auto lists = coop::host::read_ptr(memory, *manager + 0x118);
  if (!lists) {
    return;
  }
  const std::uint64_t head = *lists + 0x670;
  const auto count = coop::host::read_u32(memory, head + 0x10);
  if (!count || *count == 0) {
    return;
  }
  auto node = coop::host::read_ptr(memory, head);
  std::size_t walked = 0;
  unsigned logged = 0;
  while (node && *node != head && walked < *count) {
    ++walked;
    const auto owner = coop::host::read_ptr(memory, *node + 0x10);
    if (!owner || *owner + 0x538 != *node) {
      node = coop::host::read_ptr(memory, *node);
      continue;
    }
    const std::uint64_t actor = *owner;
    if (!g_scaled_enemies.contains(actor)) {
      const auto name = ActorName(memory, actor);
      const bool enemy = name &&
                         (name->starts_with("Enemy_") || name->starts_with("Boss_"));
      if (enemy) {
        const auto components = coop::host::read_ptr(memory, actor + 0x270);
        const auto life = components ? coop::host::read_ptr(memory, *components + 0x2b8)
                                     : std::nullopt;
        if (life) {
          // The Life component holds pointers to the value and max; the
          // s32 is 8 bytes into each.
          const auto value_ptr = coop::host::read_ptr(memory, *life + 0x1c10);
          const auto max_ptr = coop::host::read_ptr(memory, *life + 0x1c18);
          const auto value = value_ptr ? coop::host::read_u32(memory, *value_ptr + 8)
                                       : std::nullopt;
          const auto max = max_ptr ? coop::host::read_u32(memory, *max_ptr + 8) : std::nullopt;
          if (value && max && *max > 0 && *max < 0x1000000) {
            const auto scale = [](std::uint32_t v) {
              return static_cast<std::uint32_t>(
                  std::lround(static_cast<double>(v) * g_settings.enemy_hp));
            };
            const std::uint32_t new_max = scale(*max);
            const std::uint32_t new_value = scale(*value);
            const std::span<const std::uint8_t> max_bytes(
                reinterpret_cast<const std::uint8_t*>(&new_max), sizeof(new_max));
            const std::span<const std::uint8_t> value_bytes(
                reinterpret_cast<const std::uint8_t*>(&new_value), sizeof(new_value));
            if (memory.write(*max_ptr + 8, max_bytes) &&
                memory.write(*value_ptr + 8, value_bytes)) {
              g_scaled_enemies.insert(actor);
              if (logged < 8) {
                ++logged;
                LOG_INFO(Core, "split: enemy hp {} {} -> {} ({:.2f}x)", *name, *max, new_max,
                         g_settings.enemy_hp);
              }
            }
          }
        }
      }
    }
    node = coop::host::read_ptr(memory, *node);
  }
}

// M2 prototype: the world-to-clip matrix that centres the view on Link 2.
// The game matrix is row-major with column vectors, so translating the world
// by (link1 - link2) is M * T(delta): add M[row][3] * delta to each of the
// first three columns of the row.
void UpdateSecondCamera(const coop::host::PlayerReading& reading, EmulatorMemory& memory) {
  const bool split = CoopCamera::Split();
  const bool view2 = std::getenv("LICHII_SPLIT_VIEW2") != nullptr;
  const char* camera_file = std::getenv("LICHII_SPLIT_CAMERA_FILE");
  const bool camera_test = camera_file != nullptr;
  const bool wanted = split || view2 || camera_test;
  CoopCamera::SetUseSecond(view2 && !split);
  if (!wanted) {
    return;
  }
  static std::uint64_t last_view_log = 0;
  const std::uint64_t now_log = NowMs();
  if (last_view_log == 0 || now_log - last_view_log >= 1000) {
    last_view_log = now_log;
    const auto probe = CoopCamera::Latest();
    LOG_INFO(Core, "split: view2 split {} use {} camera {} started {} addr {:#x} cpu {:#x}",
             split, CoopCamera::UseSecond(), probe.has_value(), g_puppet_input.started,
             CoopCamera::CameraAddress(), CoopCamera::CameraCpuAddress());
  }
  if (!g_puppet_input.started) {
    return;
  }
  // The game stores row-major matrices with column vectors, so translating
  // the world by `delta` is m * T(delta): only the translation column moves.
  std::array<float, 3> delta{reading.position[0] - g_puppet_input.position[0],
                             reading.position[1] - g_puppet_input.position[1],
                             reading.position[2] - g_puppet_input.position[2]};
  if (const char* test_shift = std::getenv("LICHII_SPLIT_TEST_SHIFT")) {
    delta = {std::strtof(test_shift, nullptr), 0.0f, 0.0f};
  }
  CoopCamera::SetSecondDelta(delta);
  // Translation-only is the default: it keeps the view inside the room the
  // primary camera already framed. The synthetic chase rig moves the eye by
  // the actor offset and can place it through a wall (Temple of Time: the
  // second half showed the outdoor skybox). LICHII_SPLIT_CAMERA_RIG=1 opts in
  // to the rig for open areas.
  const char* rig_env = std::getenv("LICHII_SPLIT_CAMERA_RIG");
  if (rig_env != nullptr && rig_env[0] == '1') {
    CoopCamera::SecondRig rig;
    rig.first = {reading.position[0], reading.position[1], reading.position[2]};
    rig.second = {g_puppet_input.position[0], g_puppet_input.position[1],
                  g_puppet_input.position[2]};
    if (reading.rotation) {
      const auto& q = *reading.rotation;
      // Rotate the actor's local +Z by the quaternion and take its yaw.
      const float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]);
      const float fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
      const float link1_yaw = std::atan2(fx, fz);
      float yaw = g_puppet_input.yaw - link1_yaw;
      while (yaw > 3.14159265f) {
        yaw -= 6.2831853f;
      }
      while (yaw < -3.14159265f) {
        yaw += 6.2831853f;
      }
      rig.yaw_delta = yaw;
    }
    CoopCamera::SetSecondRig(rig);
  }
  // The renderer reads the primary camera from the draw's own uniform, so the
  // matrix in Latest() is only needed by the camera-source test below.
  std::array<float, 16> second{};
  const auto camera = CoopCamera::Latest();
  if (camera) {
    const auto& m = camera->clip_from_world;
    second = m;
    for (std::size_t row = 0; row < 4; ++row) {
      second[row * 4 + 3] += m[row * 4 + 0] * delta[0] + m[row * 4 + 1] * delta[1] +
                             m[row * 4 + 2] * delta[2];
    }
    CoopCamera::SetSecondMatrix(second);
  }

  if (camera_test) {
    static std::uint64_t last_targets_read = 0;
    static std::vector<std::uint64_t> targets;
    static std::uint64_t last_test_log = 0;
    const std::uint64_t now = NowMs();
    if (last_targets_read == 0 || now - last_targets_read >= 1000) {
      last_targets_read = now;
      targets.clear();
      std::ifstream in(*camera_file == '1' ? "/tmp/split-camera-targets.txt" : camera_file);
      std::string line;
      while (std::getline(in, line)) {
        const auto start = line.find("0x");
        if (start == std::string::npos) {
          continue;
        }
        const auto value = std::strtoull(line.c_str() + start, nullptr, 16);
        if (value >= 0x1000) {
          targets.push_back(value);
        }
      }
    }
    if (last_test_log == 0 || now - last_test_log >= 1000) {
      last_test_log = now;
      LOG_INFO(Core, "split: camera test addr {:#x}, {} targets", CoopCamera::CameraAddress(),
               targets.size());
    }
    for (const std::uint64_t target : targets) {
      memory.write(target, std::span<const std::uint8_t>(
                                reinterpret_cast<const std::uint8_t*>(second.data()),
                                sizeof(second)));
    }
  }
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
  // Split rendering is the default co-op view: the renderer rewrites the
  // scene camera's view and view-projection for player 2 on alternate
  // frames. LICHII_SPLIT_SCREEN=0 falls back to the single-camera two-Link
  // mode.
  const char* split_env = std::getenv("LICHII_SPLIT_SCREEN");
  g_split_screen = split_env == nullptr || split_env[0] != '0';
  if (g_split_screen) {
    CoopCamera::SetSplitLayout(g_settings.layout == split::Layout::vertical);
    LOG_INFO(Core, "split: screen split on, layout {}",
             split::LayoutName(g_settings.layout));
  }

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

// LICHII_SPLIT_SHARE_358: the player-state update reads the component state at
// [actor_table+0x358] and passes it to the player-update path. The C body has
// the +0x158 state but not the +0x358 component, so the update passes null and
// crashes in the lock. Until a private +0x358 component is constructed, point
// the child's slot at the active player's component (shared) before the update.
void SyncChildTable358(EmulatorMemory& memory, std::uint64_t main_base) {
  const bool share = std::getenv("LICHII_SPLIT_SHARE_358") != nullptr;
  const bool private_state = std::getenv("LICHII_SPLIT_PRIVATE_STATE") != nullptr;
  if ((!share && !private_state) || main_base == 0) {
    return;
  }
  std::uint64_t child = 0;
  std::uint64_t child_block = 0;
  for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
    if (slot.actor != 0) {
      child = slot.actor;
      child_block = slot.block;
    }
  }
  if (child == 0) {
    return;
  }
  const auto child_table = coop::host::read_ptr(memory, child + 0x270);
  if (!child_table) {
    return;
  }
  bool need_358 = share;
  if (const auto current = coop::host::read_ptr(memory, *child_table + 0x358);
      current && *current != 0) {
    need_358 = false;
  }
  bool need_158 = private_state;
  if (const auto current = coop::host::read_ptr(memory, *child_table + 0x158);
      current && *current != 0) {
    need_158 = false;
  }
  if (!need_358 && !need_158) {
    return;
  }
  const auto scene = coop::host::read_ptr(memory, main_base + 0x4e97648);
  const auto table = scene ? coop::host::read_ptr(memory, *scene + 0x1f0) : std::nullopt;
  const auto array = table ? coop::host::read_ptr(memory, *table + 0x58) : std::nullopt;
  const auto manager = array ? coop::host::read_ptr(memory, *array + 0x78) : std::nullopt;
  if (!scene || !table || !array || !manager) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    const auto link = coop::host::read_ptr(memory, *manager + 0x40 + i * 0x10);
    if (!link) {
      continue;
    }
    const auto holder = coop::host::read_ptr(memory, *link + 0x8);
    const auto actor = holder ? coop::host::read_ptr(memory, *holder + 0x40) : std::nullopt;
    const auto source_table = actor ? coop::host::read_ptr(memory, *actor + 0x270) : std::nullopt;
    const auto source =
        source_table ? coop::host::read_ptr(memory, *source_table + 0x358) : std::nullopt;
    const auto source_state =
        source_table ? coop::host::read_ptr(memory, *source_table + 0x158) : std::nullopt;
    if (!source || *source == 0 || !source_table) {
      continue;
    }
    if (need_158 && source_state && *source_state != 0 && child_block != 0) {
      const std::uint64_t scratch = child_block + 0x700;
      std::array<std::uint8_t, 0x1a0> bytes{};
      if (memory.read(*source_state, bytes) && memory.write(scratch, bytes)) {
        // Keep the source state's initialized lock for the first test; the
        // queued SDK initializer did not run through the call cave.
        const std::uint64_t owner = child;
        if (memory.write(scratch + 0x18,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(&owner), sizeof(owner))) &&
            memory.write(*child_table + 0x158,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(&scratch), sizeof(scratch)))) {
          LOG_INFO(Core,
                   "split: early private state {:#x} <- {:#x} for child {:#x}",
                   scratch, *source_state, child);
        }
      }
    }
    if (need_358) {
      const std::uint64_t value = *source;
      if (memory.write(*child_table + 0x358,
                       std::span<const std::uint8_t>(
                           reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)))) {
        LOG_INFO(Core,
                 "split: child {:#x} table+0x358 <- {:#x} (shared component)",
                 child, value);
      }
    }
    return;
  }
}

void OnFrame(Core::System& system) {
  // The CLI frontend has no split panel, so start on the first presented
  // frame. Start() only reads settings and is idempotent; the Qt frontend
  // still calls it from the panel before the first frame.
  if (!g_started) {
    Start();
  }
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
    if (std::getenv("LICHII_LOG_MODULES") != nullptr) {
      const auto modules = Core::FindModules(process);
      for (const auto& entry : modules) {
        LOG_INFO(Core, "split: module {:#x} {}", entry.first, entry.second);
      }
    }
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
  // LICHII_SPLIT_ARENA_OVERFLOW: the per-player event arena is 0x20e0 with
  // only 0x110 free, but the next 0x168 event object fits in the page slack.
  // Force the allocator's end check to take the success branch at
  // main+0x3472288 (b.hs -> b).
  if (std::getenv("LICHII_SPLIT_ARENA_OVERFLOW") != nullptr && g_main_base != 0) {
    static bool arena_patched = false;
    if (!arena_patched) {
      const std::uint32_t branch = 0x1400002a;
      const std::uint64_t address = g_main_base + 0x3472288;
      if (memory.write(address,
                       std::span<const std::uint8_t>(
                           reinterpret_cast<const std::uint8_t*>(&branch),
                           sizeof(branch)))) {
        Core::InvalidateInstructionCacheRange(process, address, sizeof(branch));
        arena_patched = true;
        LOG_INFO(Core, "split: arena end check bypassed at {:#x}", address);
      }
    }
  }
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

  if (g_player_actor == 0 || g_frames % 120 == 0) {
    if (const auto actor = reader.find_player_actor()) {
      g_player_actor = *actor;
    }
  }
  if (std::getenv("LICHII_SPLIT_BLACKBOARD_SHARE") != nullptr &&
      g_main_base != 0 && g_player_actor != 0) {
    static bool blackboard_patched = false;
    if (!blackboard_patched) {
      const std::array<std::uint32_t, 4> insns{
          0x58fffe15, 0xb5000055, 0xf9419d15, 0xb94096aa};
      const std::uint64_t address = g_main_base + 0x2e6fd14;
      if (memory.write(address,
                       std::span<const std::uint8_t>(
                           reinterpret_cast<const std::uint8_t*>(insns.data()),
                           sizeof(insns)))) {
        Core::InvalidateInstructionCacheRange(process, address, sizeof(insns));
        blackboard_patched = true;
        LOG_INFO(Core, "split: blackboard registry fallback patched at {:#x}",
                 address);
      }
    }
    if (const auto table = coop::host::read_ptr(memory, g_player_actor + 0x270);
        table) {
      if (const auto state = coop::host::read_ptr(memory, *table + 0x338); state) {
        const std::uint64_t value = *state;
        memory.write(g_main_base + 0x2e6fcd4,
                     std::span<const std::uint8_t>(
                         reinterpret_cast<const std::uint8_t*>(&value),
                         sizeof(value)));
      }
    }
  }
  const std::uint64_t life_now_ms = NowMs();
  BridgeChildLife(memory, g_player_actor, life_now_ms);
  SyncChildTable358(memory, g_main_base);
  DebugCopyActiveLink(memory, g_main_base, life_now_ms);
  DebugDrainHealth(memory, g_player_actor, life_now_ms,
                   reading && !reading->event_running.value_or(false));
  static bool death_as_played = false;
  if (g_shared_health > 0) {
    death_as_played = false;
  }
  if (g_player_actor != 0) {
    if (const auto life = ReadLife(memory, g_player_actor)) {
      g_shared_health = life->first;
      g_shared_max = life->second;
      if (g_shared_health <= 0 && !g_game_over) {
        g_game_over = true;
        LOG_INFO(Core, "split: shared game over (health 0/{}), hiding Link 2",
                 g_shared_max);
      } else if (g_shared_health > 0 && g_game_over) {
        g_game_over = false;
        LOG_INFO(Core, "split: shared health restored ({}/{}), showing Link 2",
                 g_shared_health, g_shared_max);
      }
    }
  }

  if (g_game_over && !death_as_played &&
      std::getenv("LICHII_SPLIT_DEATH_AS") != nullptr) {
    if (PlayPlayerDeath(memory, g_main_base, g_player_actor)) {
      death_as_played = true;
    }
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
    g_scaled_enemies.clear();
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

  ScaleEnemyLife(memory, now_ms);

  auto input = g_planner->Plan(g_main_base, reading->scene, reading->position, false, {},
                               now_ms);
  static const bool input_puppet = std::getenv("LICHII_SPLIT_INPUT") != nullptr;
  if (input_puppet) {
    UpdatePuppetInput(system, input, *reading, reading->event_running.value_or(false), now_ms);
  }
  // Split only in gameplay: menus, loading and cutscenes stay full-screen.
  const bool event_running = reading->event_running.value_or(false);
  const bool want_split = g_split_screen && reading->scene && !reading->scene->empty() &&
                          !event_running && !g_game_over;
  CoopCamera::SetSplit(want_split);
  static std::uint32_t split_view = 0;
  if (want_split) {
    split_view ^= 1u;
  } else {
    split_view = 0;
  }
  CoopCamera::SetCurrentView(split_view);
  UpdateSecondCamera(*reading, memory);
  static const bool hold_puppet = std::getenv("LICHII_SPLIT_HOLD") != nullptr;
  if (hold_puppet) {
    for (auto& target : input.targets) {
      target.show = CoopPuppet::Show::hold;
    }
  }
  if (g_game_over) {
    // One game over for the shared run. The director sees no target and
    // deletes Link 2; Continue loads the checkpoint and it is made again.
    input.targets.clear();
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
  lines.push_back({"Split: M1b two Links", false});
  lines.push_back({fmt::format("layout: {}", split::LayoutName(g_settings.layout)), false});
  lines.push_back({fmt::format("enemy hp: {:.2f}x", g_settings.enemy_hp), false});
  lines.push_back({fmt::format("shared health: {}/{}", g_shared_health, g_shared_max), true});
  lines.push_back({fmt::format("P2 controller: {}", g_settings.player2_controller), true});
  return lines;
}

}  // namespace SplitHook
