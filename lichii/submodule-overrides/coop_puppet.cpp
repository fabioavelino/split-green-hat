// Puppets (coop_puppet.h). No emulator code.
#include "coop_puppet.h"

#include "coop_camera.h"
#include "coop_pose.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "coop/host/scene.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/presence/presence.hpp"

namespace CoopPuppet {
namespace {

template <typename T>
bool ReadValue(Guest& guest, std::uint64_t address, T& out) {
    return guest.Read(address, &out, sizeof(T));
}

template <typename T>
bool WriteValue(Guest& guest, std::uint64_t address, const T& value) {
    return guest.Write(address, &value, sizeof(T));
}

std::string Hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string VecText(const Vec3& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v[0], v[1], v[2]);
    return buf;
}

bool Gone(std::uint32_t state) {
    return state >= 7 && state <= 11;  // as 0x37cca0
}

} // namespace

Mat3 QuatToMatrix(const Quat& q) {
    const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] +
                               double(q[3]) * q[3]);
    if (!(n > 1e-6) || !std::isfinite(n)) {
        return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    }
    const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    return {
        float(1 - 2 * (y * y + z * z)), float(2 * (x * y - z * w)),     float(2 * (x * z + y * w)),
        float(2 * (x * y + z * w)),     float(1 - 2 * (x * x + z * z)), float(2 * (y * z - x * w)),
        float(2 * (x * z - y * w)),     float(2 * (y * z + x * w)),     float(1 - 2 * (x * x + y * y)),
    };
}

const char* PhaseText(Phase phase) {
    switch (phase) {
    case Phase::no_block:
        return "no block";
    case Phase::allocating:
        return "allocating";
    case Phase::idle:
        return "idle";
    case Phase::creating:
        return "creating";
    case Phase::live:
        return "live";
    case Phase::deleting:
        return "deleting";
    }
    return "?";
}

Director::Director(Log log, bool verbose, bool anim)
    : log_(std::move(log)), verbose_(verbose), anim_(anim) {}

void Director::SetInput(Input input) {
    std::lock_guard lock(input_mutex_);
    next_input_ = std::move(input);
}

void Director::QueueCall(const Call& call) {
    std::lock_guard lock(extra_mutex_);
    if (extra_calls_.size() < 8) {
        extra_calls_.push_back(call);
    }
}

void Director::Say(const std::string& text) const {
    if (log_) {
        log_("coop: puppet: " + text);
    }
}

std::vector<Director::SlotView> Director::Slots() const {
    std::vector<SlotView> out;
    for (const auto& s : slots_) {
        out.push_back({s.number, s.phase, s.block, s.actor, s.actor_name, s.anim_name});
    }
    return out;
}

const Target* Director::Wanted(std::uint8_t number) const {
    for (const auto& t : input_.targets) {
        if (t.number == number) {
            return &t;
        }
    }
    return nullptr;
}

Director::Slot& Director::SlotFor(std::uint8_t number) {
    for (auto& s : slots_) {
        if (s.number == number) {
            return s;
        }
    }
    Slot s;
    s.number = number;
    slots_.push_back(s);
    return slots_.back();
}

std::optional<Call> Director::Step(Guest& guest, bool after_call, std::uint64_t result,
                                   std::uint64_t now_ms) {
    if (!after_call) {
        NewFrame(guest, now_ms);
        PoseFrame(guest, now_ms);
    } else if (pending_ != Pending::none) {
        Finish(guest, result, now_ms);
    }
    if (input_.main_base == 0 || calls_ >= kMaxCallsPerFrame) {
        return std::nullopt;
    }
    {
        std::lock_guard lock(extra_mutex_);
        if (!extra_calls_.empty()) {
            Call call = extra_calls_.front();
            extra_calls_.pop_front();
            ++calls_;
            return call;
        }
    }
    for (const auto& t : input_.targets) {
        SlotFor(t.number);
    }
    for (auto& slot : slots_) {
        if (auto call = Next(slot, guest, now_ms)) {
            pending_slot_ = slot.number;
            ++calls_;
            return call;
        }
    }
    return std::nullopt;
}

// Once a frame, the full-body pose copy (coop_pose.cpp): our skeleton is
// read and each live puppet's is written.
void Director::PoseFrame(Guest& guest, std::uint64_t now_ms) {
    if (!pose_copy_) {
        return;
    }
    std::vector<std::pair<std::uint8_t, std::uint64_t>> puppets;
    for (const auto& slot : slots_) {
        if (slot.phase == Phase::live && slot.actor != 0) {
            puppets.emplace_back(slot.number, slot.actor);
        }
    }
    CoopPose::OnCalcFrame(guest, input_.main_base, puppets, now_ms);
}

void Director::NewFrame(Guest& guest, std::uint64_t now_ms) {
    ++frames_;
    calls_ = 0;
    if (pending_ != Pending::none) {
        // The cave did not make the call (its own limit): undo the phase.
        Slot& slot = SlotFor(pending_slot_);
        if (pending_ == Pending::malloc) {
            slot.phase = Phase::no_block;
        } else if (pending_ == Pending::create) {
            slot.phase = Phase::idle;
        } else if (pending_ == Pending::take || pending_ == Pending::drop) {
            // Still creating with the flag set: asked again.
        } else {
            slot.phase = Phase::live;
        }
        pending_ = Pending::none;
    }
    {
        std::lock_guard lock(input_mutex_);
        input_ = next_input_;
    }
    if (input_.scene != scene_) {
        scene_ = input_.scene;
        scene_since_ms_ = now_ms;
    }
    if (input_.main_base == 0) {
        return;
    }
    for (auto& slot : slots_) {
        if (slot.phase != Phase::creating && slot.phase != Phase::live &&
            slot.phase != Phase::deleting) {
            continue;
        }
        if (slot.phase == Phase::creating) {
            Poll(slot, guest, now_ms);
            if (slot.phase != Phase::creating) {
                continue;
            }
        }
        slot.actor = Resolve(slot, guest);
        std::uint32_t state = 0;
        const bool have_state =
            slot.actor != 0 && ReadValue(guest, slot.actor + addr::kActorState, state);
        switch (slot.phase) {
        case Phase::creating:
            if (have_state && state == addr::kStatePresent) {
                Vec3 at{};
                ReadValue(guest, slot.actor + addr::kActorPosition, at);
                Say("P" + std::to_string(slot.number) + " " + slot.actor_name + " spawned after " +
                    std::to_string(now_ms - slot.since_ms) + " ms: request " + Hex(slot.request) +
                    ", actor " + Hex(slot.actor) + ", state 4, at " + VecText(at));
                slot.phase = Phase::live;
                slot.since_ms = now_ms;
            } else if (now_ms - slot.since_ms > kCreateTimeoutMs) {
                Say("P" + std::to_string(slot.number) + " " + slot.actor_name +
                    " not spawned after " + std::to_string(kCreateTimeoutMs) + " ms (actor " +
                    Hex(slot.actor) + ", state " + (have_state ? std::to_string(state) : "-") +
                    "); trying again in 5 s");
                slot.phase = Phase::idle;
                slot.retry_ms = now_ms + 5000;
                slot.take = slot.drop = false;
            }
            break;
        case Phase::live:
            if (!have_state || Gone(state)) {
                Say("P" + std::to_string(slot.number) + " gone (" +
                    (have_state ? "state " + std::to_string(state) : std::string("no actor")) +
                    ")");
                slot.phase = Phase::idle;
                slot.actor = 0;
                slot.taken = 0;
                slot.lost_ms = now_ms;  // a load deletes it before the camera goes
            } else if (const Target* t = Wanted(slot.number);
                       t != nullptr && state == addr::kStatePresent && t->actor == slot.actor_name &&
                       input_.scene == slot.scene) {
                if (t->show == Show::show) {
                    Pose(slot, guest, t->position, &t->rotation, now_ms);
                } else if (t->show == Show::hide) {
                    Vec3 parked = t->position;
                    parked[1] -= kHideDrop;
                    Pose(slot, guest, parked, nullptr, now_ms);
                }  // hold: left where it is
            }
            break;
        case Phase::deleting:
            if (!have_state || Gone(state) || now_ms - slot.since_ms > 5000) {
                Say("P" + std::to_string(slot.number) + " deleted (" +
                    (have_state ? "state " + std::to_string(state) : std::string("no actor")) +
                    ")");
                slot.phase = Phase::idle;
                slot.actor = 0;
                slot.taken = 0;
            }
            break;
        default:
            break;
        }
    }
    Status(guest, now_ms);
}

void Director::Finish(Guest& guest, std::uint64_t result, std::uint64_t now_ms) {
    Slot& slot = SlotFor(pending_slot_);
    const std::string who = "P" + std::to_string(slot.number);
    switch (pending_) {
    case Pending::malloc:
        if (result == 0) {
            Say(who + ": malloc gave nothing; trying again in 5 s");
            slot.phase = Phase::no_block;
            slot.retry_ms = now_ms + 5000;
        } else {
            const std::array<std::uint8_t, block::kSize> zero{};
            guest.Write(result, zero.data(), zero.size());
            slot.block = result;
            slot.phase = Phase::idle;
            Say(who + ": block at " + Hex(result));
        }
        break;
    case Pending::create:
        Say(who + ": create " + slot.actor_name + " requested (returned " +
            Hex(result & 0xffffffff) + "), handle slot " + Hex(slot.handle));
        if ((result & 0xffffffff) != 0) {
            slot.refused = 0;
            break;
        }
        // Refused (seen right after a load deleted the puppet, while the old
        // scene goes): the handle stayed empty and nothing was made, so ask
        // again soon. Should the game have queued a request after all, wait
        // for it as for any create, so there are never two.
        if (std::uint64_t queued = 0; ReadValue(guest, slot.handle, queued) && queued != 0) {
            break;
        }
        ++slot.refused;
        slot.phase = Phase::idle;
        if (slot.refused < kRefusedFast) {
            slot.retry_ms = now_ms + kRefusedRetryMs;
            if (slot.refused == 1) {
                Say(who + ": create refused; trying again each second");
            }
        } else {
            slot.retry_ms = now_ms + kRefusedSlowMs;
            if (slot.refused == kRefusedFast) {
                Say(who + ": create refused " + std::to_string(kRefusedFast) +
                    " times; trying again every 5 s");
            }
        }
        break;
    case Pending::take:
        slot.take = false;
        WriteValue(guest, slot.handle, std::uint64_t{0});  // as the game does
        if (result == 0 || !ReadValue(guest, result, slot.vtable)) {
            Say(who + ": request " + Hex(slot.request) + " gave no actor; trying again in 5 s");
            slot.phase = Phase::idle;
            slot.retry_ms = now_ms + 5000;
        } else {
            slot.taken = result;
            Say(who + ": took actor " + Hex(result) + " from request " + Hex(slot.request));
        }
        break;
    case Pending::drop:
        slot.drop = false;
        WriteValue(guest, slot.handle, std::uint64_t{0});
        Say(who + ": request " + Hex(slot.request) + " dropped (state 4); trying again in 5 s");
        slot.phase = Phase::idle;
        slot.retry_ms = now_ms + 5000;
        break;
    case Pending::remove:
        Say(who + ": delete requested (returned " + Hex(result & 0xff) + ")");
        break;
    case Pending::play:
        slot.anim_name = slot.play_name;
        slot.anim_serial = slot.play_serial;
        slot.frame_to_set = slot.play_frame;
        slot.sync_next = 1;  // then the partial slots, after the frame
        if (anim_) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " from frame %.1f", slot.play_frame);
            Say(who + " plays '" + slot.play_name + "'" + buf + " (returned " +
                Hex(result & 0xff) + ")");
        }
        break;
    case Pending::frame:
        slot.frame_to_set.reset();
        break;
    case Pending::sync:
        if (anim_) {
            Say(who + " slot " + std::to_string(slot.sync_next) + " synced to slot 0 ('" +
                slot.anim_name + "')");
        }
        ++slot.sync_next;
        break;
    case Pending::equip:
        slot.worn[slot.equip_index] = slot.equip_name;
        if (const int s = kGearSlots[slot.equip_index]; s >= 0) {
            // A new piece is worn put away, whatever the old one was.
            slot.in_hand = static_cast<std::uint8_t>(slot.in_hand & ~(1u << s));
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known & ~(1u << s));
        }
        Say(who + " wears " + kGearNames[slot.equip_index] + " '" + slot.equip_name +
            "' (returned " + Hex(result & 0xff) + ")");
        break;
    case Pending::hold: {
        // Taken when the slot's byte shows the state asked for.
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        std::uint8_t bits = 0;
        const bool read = ReadValue(guest, slot.actor + addr::kActorParts, parts) && parts != 0 &&
                          ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) && eu != 0 &&
                          ReadValue(guest, eu + addr::kEquipSlotBits + slot.hold_slot, bits);
        const std::uint8_t want = slot.hold_in_hand ? addr::kEquipBitInHand : addr::kEquipBitAway;
        const auto bit = static_cast<std::uint8_t>(1u << slot.hold_slot);
        if (read && (bits & want) != 0) {
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known | bit);
            slot.in_hand = static_cast<std::uint8_t>(slot.hold_in_hand ? (slot.in_hand | bit)
                                                                       : (slot.in_hand & ~bit));
            Say(who + " equipment slot " + std::to_string(slot.hold_slot) +
                (slot.hold_in_hand ? " in hand" : " put away"));
        } else {
            slot.hold_retry_ms = now_ms + 250;  // the piece is not ready: ask again
            if (anim_) {
                Say(who + " equipment slot " + std::to_string(slot.hold_slot) +
                    (slot.hold_in_hand ? " in hand" : " put away") + " not taken (byte " +
                    Hex(bits) + ")");
            }
        }
        break;
    }
    case Pending::move:
        break;
    case Pending::none:
        break;
    }
    pending_ = Pending::none;
}

std::optional<Call> Director::Next(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    const Target* t = Wanted(slot.number);
    if (t == nullptr) {
        slot.wanted_actor.clear();
    } else if (t->actor != slot.wanted_actor) {
        slot.wanted_actor = t->actor;
        slot.wanted_since_ms = now_ms;
    }
    switch (slot.phase) {
    case Phase::no_block:
        if (t != nullptr && now_ms >= slot.retry_ms) {
            Call c;
            c.function = input_.main_base + addr::kMalloc;
            c.x[0] = block::kSize;
            slot.phase = Phase::allocating;
            pending_ = Pending::malloc;
            return c;
        }
        return std::nullopt;
    case Phase::idle:
        // A failed create waits 5 s before the same actor is tried again; a
        // different one (the player's age was read wrong while loading) at once.
        if (t != nullptr && slot.block != 0 && !input_.scene.empty() &&
            (now_ms >= slot.retry_ms || t->actor != slot.actor_name)) {
            if (now_ms - slot.wanted_since_ms < input_.settle_ms) {
                return std::nullopt;  // the age may still change (Input::settle_ms)
            }
            if (!Drawn(slot, now_ms)) {
                if (!slot.wait_said) {
                    slot.wait_said = true;
                    Say("P" + std::to_string(slot.number) + ": waiting for the game to draw");
                }
                return std::nullopt;
            }
            auto c = Create(slot, *t, guest);
            if (c) {
                slot.since_ms = now_ms;
                slot.wait_said = false;
            }
            return c;
        }
        return std::nullopt;
    case Phase::creating:
        if (slot.take || slot.drop) {
            Call c;
            c.x[0] = slot.request;
            if (slot.take) {
                c.function = input_.main_base + addr::kTakeCreated;
                c.x[1] = 1;
                pending_ = Pending::take;
            } else {
                c.function = input_.main_base + addr::kDropCreate;
                pending_ = Pending::drop;
            }
            return c;
        }
        return std::nullopt;
    case Phase::live:
        if (input_.scene.empty() || slot.actor == 0) {
            return std::nullopt;  // loading: wait and see what is left
        }
        if (t != nullptr) {
            slot.missing_ms = 0;
        } else if (input_.scene == slot.scene) {
            if (slot.missing_ms == 0) {
                slot.missing_ms = now_ms;
            }
            if (now_ms - slot.missing_ms < kLeaveGraceMs) {
                return std::nullopt;  // maybe a hiccup: keep it a moment
            }
        }
        if (t == nullptr || t->actor != slot.actor_name || input_.scene != slot.scene) {
            slot.missing_ms = 0;
            Call c;
            c.function = input_.main_base + addr::kRequestDelete;
            c.x[0] = slot.actor;
            c.x[1] = 0;
            slot.phase = Phase::deleting;
            slot.since_ms = now_ms;
            pending_ = Pending::remove;
            return c;
        }
        if (auto c = Equip(slot, *t, guest, now_ms)) {
            return c;
        }
        if (auto c = Hold(slot, *t, guest, now_ms)) {
            return c;
        }
        if (matrix_call_ && t->show != Show::hold) {
            if (auto c = Move(slot, guest)) {
                return c;
            }
        }
        if (pose_copy_ && CoopPose::Drives(slot.number, now_ms)) {
            return std::nullopt;  // the pose copy moves it
        }
        return Animate(slot, *t, guest);
    default:
        return std::nullopt;
    }
}

// Whether a create now would be drawn. Made while the game loads a scene
// (the scene is readable, the camera is gone), a puppet reaches state 4 but
// never shows until it is made again (Caleb's two-player run, 2026-11-09:
// B created P1 at 37.1 s, its loading screen ran 36.8 to 40.2 s).
bool Director::Drawn(const Slot& slot, std::uint64_t now_ms) const {
    if (!input_.camera) {
        return true;  // nothing to go by
    }
    const std::uint64_t since = input_.drawn_since_ms;
    if (since == 0 || now_ms < since || now_ms - since < kDrawnMs) {
        return false;
    }
    const std::uint64_t change = std::max(scene_since_ms_, slot.lost_ms);
    return since > change || now_ms - change >= kSteadyMs;
}

std::optional<Call> Director::Create(Slot& slot, const Target& target, Guest& guest) {
    const std::uint64_t base = input_.main_base;
    std::uint64_t manager = 0;
    std::uint64_t create_default = 0;
    if (!ReadValue(guest, base + addr::kActorManagerPtr, manager) || manager == 0 ||
        !ReadValue(guest, base + addr::kGotCreateDefault, create_default)) {
        return std::nullopt;  // not set up yet
    }
    const std::uint64_t b = slot.block;

    // The create argument as SeqExecuteCreateActor (0x32bca10) fills it in
    // the middle of play, with our position and rotation (docs/puppet.md).
    // The preload walker's form (+0x48 = 0x10001, +0x98 = 0) is only taken
    // up while a scene loads: made later, its request stays in state 2.
    std::array<std::uint8_t, 0x100> arg{};
    const Vec3 scale{1.0f, 1.0f, 1.0f};
    std::memcpy(arg.data() + 0x00, target.position.data(), 12);
    std::memcpy(arg.data() + 0x0c, target.rotation.data(), 36);
    std::memcpy(arg.data() + 0x30, scale.data(), 12);
    std::memcpy(arg.data() + 0x40, &create_default, 8);
    arg[0x98] = 1;
    if (!guest.Write(b + block::kCreateArg, arg.data(), arg.size())) {
        return std::nullopt;
    }

    std::array<char, block::kNameCapacity> text{};
    std::memcpy(text.data(), target.actor.data(),
                std::min<std::size_t>(target.actor.size(), text.size() - 1));
    guest.Write(b + block::kNameText, text.data(), text.size());
    WriteValue(guest, b + block::kName, b + block::kNameText);
    WriteValue(guest, b + block::kName + 8, std::uint64_t{block::kNameCapacity});

    // A fresh handle slot each time, in case the manager still holds the last one.
    slot.handle = b + block::kHandles + 8 * slot.handle_index;
    slot.handle_index = (slot.handle_index + 1) % block::kHandleCount;
    WriteValue(guest, slot.handle, std::uint64_t{0});

    Call c;
    c.function = base + addr::kCreate;
    c.x[0] = manager;
    c.x[1] = b + block::kName;
    c.x[2] = b + block::kCreateArg;
    c.x[3] = slot.handle;
    c.x[4] = 1;
    slot.phase = Phase::creating;
    slot.actor = 0;
    slot.actor_name = target.actor;
    slot.scene = input_.scene;
    slot.request = 0;
    slot.taken = 0;
    slot.vtable = 0;
    slot.take = slot.drop = false;
    slot.anim_name.clear();
    slot.anim_serial = 0;
    slot.frame_to_set.reset();
    slot.held_frame.reset();
    slot.sync_next = 0;
    slot.worn = {};
    slot.in_hand = 0;
    slot.hold_known = 0;
    slot.hold_retry_ms = 0;
    slot.hold_id = {};
    slot.far_since_ms = {};
    slot.rewear_tries = {};
    pending_ = Pending::create;
    return c;
}

namespace {
// The eight equipment slots of an actor's EquipmentUser, for the debug log:
// for each slot that holds a piece its id, actor, state, place (height over
// the wearer and distance on the ground), the request byte, and the slot
// record's in-hand byte and flags (u32 at +0xa0: "put away" 0xe34c70 sets
// 0x800, "in hand" 0x6b6820 clears it; read from the dump, not run).
std::string SlotsText(Guest& guest, std::uint64_t actor, std::optional<std::uint8_t> wanted) {
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    if (!ReadValue(guest, actor + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
        return "no equipment user";
    }
    Vec3 own{};
    ReadValue(guest, actor + addr::kActorPosition, own);
    std::string text = "eu " + Hex(eu);
    for (std::uint32_t s = 0; s < 8; ++s) {
        const std::uint64_t link = eu + addr::kEquipSlots + addr::kEquipSlotStride * s;
        std::uint32_t id = 0xFFFFFFFFu;
        std::uint64_t holder = 0;
        std::uint64_t piece = 0;
        std::uint32_t state = 0;
        std::uint8_t request = 0xff;
        std::uint8_t hand = 0xff;
        std::uint32_t flags = 0;
        Vec3 at{};
        ReadValue(guest, link + addr::kEquipSlotId, id);
        ReadValue(guest, eu + addr::kEquipSlotBits + s, request);
        if (id == 0xFFFFFFFFu && (request == 0 || request == 0xff)) {
            continue;  // nothing in this slot
        }
        ReadValue(guest, link + addr::kEquipSlotHolder, holder);
        if (holder != 0) {
            ReadValue(guest, holder + addr::kHolderActor, piece);
        }
        if (piece != 0) {
            ReadValue(guest, piece + addr::kActorState, state);
            ReadValue(guest, piece + addr::kActorPosition, at);
        }
        const std::uint64_t record = eu + addr::kEquipRecords + addr::kEquipRecordStride * s;
        ReadValue(guest, record + addr::kEquipRecordInHand, hand);
        ReadValue(guest, record + addr::kEquipRecordFlags, flags);
        char place[64];
        std::snprintf(place, sizeof(place), "up %.2f off %.2f", static_cast<double>(at[1] - own[1]),
                      static_cast<double>(std::hypot(at[0] - own[0], at[2] - own[2])));
        text += "; slot " + std::to_string(s) + ": id " + Hex(id) + " actor " + Hex(piece) +
                " state " + std::to_string(state) + " " + place + " request " + Hex(request) +
                " record " + Hex(hand) + " flags " + Hex(flags);
        if (wanted) {
            text += std::string(" want ") + (((*wanted >> s) & 1u) != 0 ? "hand" : "away");
        }
    }
    return text;
}
}  // namespace

// One equip call for a live puppet whose player's gear differs from what it
// wears, or none. An empty name (nothing in that slot) is not taken off yet:
// how to clear a slot is not proven (0x2e01a90 with "" is a guess).
std::optional<Call> Director::Equip(Slot& slot, const Target& target, Guest& guest,
                                    std::uint64_t now_ms) {
    if (anim_ && now_ms - slot.gear_log_ms >= 2000) {
        slot.gear_log_ms = now_ms;
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        ReadValue(guest, slot.actor + addr::kActorParts, parts);
        if (parts != 0) {
            ReadValue(guest, parts + addr::kPartsEquipmentUser, eu);
        }
        std::string line = "P" + std::to_string(slot.number) + " gear: eu " + Hex(eu);
        for (std::size_t i = 0; i < target.gear.size(); ++i) {
            line += std::string(", ") + kGearNames[i] + " " +
                    (target.gear[i] ? "'" + *target.gear[i] + "'" : std::string("?")) +
                    (slot.worn[i].empty() ? "" : " (worn)");
        }
        Say(line);
        // Every equipment slot as the game has it, and the local player's
        // beside it: what differs between the two is what does not show.
        Say("P" + std::to_string(slot.number) + " slots (puppet " + Hex(slot.actor) +
            "): " + SlotsText(guest, slot.actor, target.drawn));
        if (const std::uint64_t player = CoopPose::FindPlayer(guest, input_.main_base)) {
            Say("P" + std::to_string(slot.number) + " slots (local player " + Hex(player) +
                "): " + SlotsText(guest, player, std::nullopt));
        }
    }
    if (target.show == Show::hide) {
        return std::nullopt;  // parked: a piece made now would stay down there
    }
    if (target.show == Show::show && now_ms >= slot.rewear_ms) {
        LeftBehind(slot, guest, now_ms);
    }
    for (std::size_t i = 0; i < target.gear.size(); ++i) {
        const auto& want = target.gear[i];
        if (!want || want->empty() || *want == slot.worn[i] ||
            want->size() >= block::kGearCapacity) {
            continue;
        }
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
            !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
            return std::nullopt;  // not set up (yet)
        }
        const std::uint64_t text = slot.block + block::kGearText + block::kGearCapacity * i;
        const std::uint64_t cell = slot.block + block::kGearPtr + 8 * i;
        std::array<char, block::kGearCapacity> name{};
        std::memcpy(name.data(), want->data(), want->size());
        if (!guest.Write(text, name.data(), name.size()) || !WriteValue(guest, cell, text)) {
            return std::nullopt;
        }
        slot.equip_index = i;
        slot.equip_name = *want;
        Call c;
        c.function = input_.main_base + addr::kEquip;
        c.x[0] = eu;
        c.x[1] = kGearKinds[i];
        c.x[2] = cell;
        c.x[3] = 2;
        c.x[4] = 0xFFFFFFFF;
        c.x[5] = 1;
        c.x[6] = 0;
        pending_ = Pending::equip;
        return c;
    }
    return std::nullopt;
}

// A worn sword or shield far from its puppet does not follow it: the game
// made it while the puppet was parked under the floor (Show::hide), or it
// stayed behind when the puppet came back up. It is worn again, so the game
// makes it anew at the puppet (and the in-hand state is set again for the
// new piece, Hold).
void Director::LeftBehind(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    Vec3 own{};
    if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0 ||
        !ReadValue(guest, slot.actor + addr::kActorPosition, own)) {
        return;
    }
    for (std::size_t i = 0; i < kGearSlots.size(); ++i) {
        const int s = kGearSlots[i];
        if (s < 0 || slot.worn[i].empty()) {
            continue;
        }
        if (GearIsCarried(i) && (slot.in_hand & (1u << s)) == 0) {
            continue;  // put away, the game keeps a held item elsewhere (state 6)
        }
        const std::uint64_t link = eu + addr::kEquipSlots + addr::kEquipSlotStride * s;
        std::uint32_t id = 0xFFFFFFFFu;
        std::uint64_t holder = 0;
        std::uint64_t piece = 0;
        std::uint32_t state = 0;
        Vec3 at{};
        if (!ReadValue(guest, link + addr::kEquipSlotId, id) || id == 0xFFFFFFFFu ||
            !ReadValue(guest, link + addr::kEquipSlotHolder, holder) || holder == 0 ||
            !ReadValue(guest, holder + addr::kHolderActor, piece) || piece == 0 ||
            !ReadValue(guest, piece + addr::kActorState, state) || state != addr::kStatePresent ||
            !ReadValue(guest, piece + addr::kActorPosition, at)) {
            continue;
        }
        const float far = std::sqrt((at[0] - own[0]) * (at[0] - own[0]) +
                                    (at[1] - own[1]) * (at[1] - own[1]) +
                                    (at[2] - own[2]) * (at[2] - own[2]));
        if (!(far > kGearFar)) {
            slot.far_since_ms[i] = 0;
            slot.rewear_tries[i] = 0;  // where it belongs: a later miss may try again
            continue;
        }
        // Far on one reading may be a piece one frame behind a puppet that
        // jumped: it must still be far a moment later.
        if (slot.far_since_ms[i] == 0) {
            slot.far_since_ms[i] = now_ms;
            continue;
        }
        if (now_ms - slot.far_since_ms[i] < kFarForMs) {
            continue;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(far));
        const std::string what =
            "P" + std::to_string(slot.number) + " " + kGearNames[i] + " '" + slot.worn[i] + "'";
        if (slot.rewear_tries[i] >= kRewearTries) {
            if (slot.rewear_tries[i] == kRewearTries) {
                Say(what + " still left behind (" + text + " away) after " +
                    std::to_string(kRewearTries) + " tries: given up");
                ++slot.rewear_tries[i];
            }
            continue;
        }
        ++slot.rewear_tries[i];
        Say(what + " left behind (" + text + " away): worn again");
        slot.worn[i].clear();
        slot.far_since_ms[i] = 0;
        slot.rewear_ms = now_ms + kRewearMs;
    }
}

// One change of an equipment slot's state for a live puppet: a worn piece
// its player holds in the hand while the puppet's is put away, or the other
// way round. A piece just worn is put away, so only "in hand" is asked then.
std::optional<Call> Director::Hold(Slot& slot, const Target& target, Guest& guest,
                                   std::uint64_t now_ms) {
    if (!target.drawn || now_ms < slot.hold_retry_ms || target.show == Show::hide) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kGearSlots.size(); ++i) {
        const int s = kGearSlots[i];
        if (s < 0 || slot.worn[i].empty()) {
            continue;
        }
        const auto bit = static_cast<std::uint8_t>(1u << s);
        const bool want = (*target.drawn & bit) != 0;
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        std::uint32_t id = 0;
        if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
            !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0 ||
            !ReadValue(guest,
                       eu + addr::kEquipSlots + addr::kEquipSlotStride * s + addr::kEquipSlotId,
                       id) ||
            id == 0xFFFFFFFFu) {
            continue;  // the piece is not there (yet)
        }
        if (id != slot.hold_id[i]) {
            // Another piece than the one last set: the game makes a worn
            // piece again a moment after the equip call (seen 2026-11-09),
            // and the new one is put away whatever the old one was.
            slot.hold_id[i] = id;
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known & ~bit);
            slot.in_hand = static_cast<std::uint8_t>(slot.in_hand & ~bit);
        }
        if ((slot.hold_known & bit) != 0 && want == ((slot.in_hand & bit) != 0)) {
            continue;
        }
        if ((slot.hold_known & bit) == 0 && !want) {
            // Worn put away, as wanted: nothing to ask.
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known | bit);
            continue;
        }
        slot.hold_slot = s;
        slot.hold_in_hand = want;
        Call c;
        c.function = input_.main_base + addr::kEquipChangeState;
        c.x[0] = eu;
        c.x[1] = static_cast<std::uint64_t>(s);
        c.x[2] = want ? addr::kEquipStateInHand : addr::kEquipStateAway;
        pending_ = Pending::hold;
        return c;
    }
    return std::nullopt;
}

// The game's own setter, once a frame, after Pose wrote the transform.
std::optional<Call> Director::Move(Slot& slot, Guest& guest) {
    (void)guest;
    if (slot.moved_frame == frames_) {
        return std::nullopt;
    }
    slot.moved_frame = frames_;
    if (slot.vtable != input_.main_base + addr::kActorVtable) {
        return std::nullopt;  // not the actor class the setter is known for
    }
    Call c;
    c.function = input_.main_base + addr::kSetMatrix;
    c.x[0] = slot.actor;
    c.x[1] = 0;
    pending_ = Pending::move;
    return c;
}

// The AS controller of an actor; 0 when the chain is not there.
std::uint64_t Director::Controller(Guest& guest, std::uint64_t actor) {
    std::uint64_t p = actor;
    for (const std::uint64_t offset : addr::kControllerPath) {
        if (p == 0 || !ReadValue(guest, p + offset, p)) {
            return 0;
        }
    }
    return p;
}

// Slot 0's frame, when the controller has a slot 0 and its frame is a number.
std::optional<float> Director::Frame(Guest& guest, std::uint64_t controller) {
    std::int32_t count = 0;
    std::uint64_t slots = 0;
    std::uint64_t slot0 = 0;
    float frame = 0.0f;
    if (!ReadValue(guest, controller + addr::kSlotCount, count) || count <= 0 ||
        !ReadValue(guest, controller + addr::kSlots, slots) || slots == 0 ||
        !ReadValue(guest, slots, slot0) || slot0 == 0 ||
        !ReadValue(guest, slot0 + addr::kSlotFrame, frame) || !std::isfinite(frame)) {
        return std::nullopt;
    }
    return frame;
}

std::optional<Call> Director::SetFrame(Slot& slot, std::uint64_t controller, float frame) {
    Call c;
    c.function = input_.main_base + addr::kSetAsFrame;
    c.x[0] = controller;
    c.x[1] = 0;  // slot 0
    c.s0 = frame;
    slot.frame_set_in = frames_;
    pending_ = Pending::frame;
    return c;
}

// One animation call for a live puppet in its scene, or none.
// - show: play the received command when it changes, then set its frame;
//   after that, set the frame only when a new sample finds it more than
//   kFrameSlack off, so the game's own playback runs in between.
// - hold: the frame it had when the hold began, every frame.
// - hide: nothing (it is out of sight).
std::optional<Call> Director::Animate(Slot& slot, const Target& target, Guest& guest) {
    if (target.show == Show::hide) {
        return std::nullopt;
    }
    const std::uint64_t controller = Controller(guest, slot.actor);
    if (controller == 0) {
        return std::nullopt;
    }
    const std::string who = "P" + std::to_string(slot.number);
    if (target.show == Show::hold) {
        if (!slot.held_frame) {
            slot.held_frame = Frame(guest, controller);
            if (!slot.held_frame) {
                return std::nullopt;
            }
            if (anim_) {
                Say(who + " held");
            }
        }
        if (slot.frame_set_in == frames_) {
            return std::nullopt;  // once a frame
        }
        return SetFrame(slot, controller, *slot.held_frame);
    }
    if (slot.held_frame && anim_) {
        Say(who + " moves again");
    }
    slot.held_frame.reset();
    if (!target.anim || target.anim->name.empty() ||
        target.anim->name.size() >= block::kAnimCapacity) {
        return std::nullopt;
    }
    const Anim& anim = *target.anim;
    if (anim.name != slot.anim_name) {
        const std::uint64_t text =
            slot.block + block::kAnimText + block::kAnimCapacity * slot.anim_buffer;
        std::array<char, block::kAnimCapacity> name{};
        std::memcpy(name.data(), anim.name.data(), anim.name.size());
        if (!guest.Write(text, name.data(), name.size())) {
            return std::nullopt;
        }
        slot.anim_buffer ^= 1;
        const float rate = std::isfinite(anim.rate) ? anim.rate : 1.0f;
        std::array<std::uint8_t, 0x30> request{};
        const float unset = -1.0f;
        const std::int32_t index = 0;
        std::memcpy(request.data() + 0x00, &slot.actor, 8);
        std::memcpy(request.data() + 0x08, &text, 8);
        std::memcpy(request.data() + 0x10, &index, 4);
        std::memcpy(request.data() + 0x14, &rate, 4);
        std::memcpy(request.data() + 0x18, &unset, 4);
        std::memcpy(request.data() + 0x1c, &unset, 4);
        // +0x20 skip-if-same stays 0: we only play a new name.
        const std::uint64_t at = slot.block + block::kPlayRequest;
        if (!guest.Write(at, request.data(), request.size())) {
            return std::nullopt;
        }
        slot.play_name = anim.name;
        slot.play_frame = std::isfinite(anim.frame) ? anim.frame : 0.0f;
        slot.play_serial = target.anim_serial;
        Call c;
        c.function = input_.main_base + addr::kPlayAs;
        c.x[0] = at;
        pending_ = Pending::play;
        return c;
    }
    if (slot.frame_to_set) {
        return SetFrame(slot, controller, *slot.frame_to_set);
    }
    if (slot.sync_next > 0) {
        std::int32_t count = 0;
        ReadValue(guest, controller + addr::kSlotCount, count);
        if (slot.sync_next < std::min(count, addr::kMaxAsSlots)) {
            Call c;
            c.function = input_.main_base + addr::kSyncAsSlot;
            c.x[0] = slot.actor;
            c.x[1] = 0;
            c.x[2] = static_cast<std::uint64_t>(slot.sync_next);
            pending_ = Pending::sync;
            return c;
        }
        slot.sync_next = 0;
    }
    if (target.anim_serial != slot.anim_serial) {
        slot.anim_serial = target.anim_serial;
        const auto now = Frame(guest, controller);
        if (std::isfinite(anim.frame) &&
            (!now || std::fabs(*now - anim.frame) > kFrameSlack)) {
            if (anim_) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " frame %.1f -> %.1f", now.value_or(-1.0f),
                              anim.frame);
                Say(who + " '" + anim.name + "'" + buf);
            }
            return SetFrame(slot, controller, anim.frame);
        }
    }
    return std::nullopt;
}

// The puppet's pose: +0x31c/+0x328 and the engine's pending transform
// (+0x358/+0x364), which the actor's calc applies over +0x31c. With the
// debug anim log, once a second, what the game left there since the last
// write, so a writer that undoes ours shows up.
void Director::Pose(Slot& slot, Guest& guest, const Vec3& position, const Mat3* rotation,
                    std::uint64_t now_ms) {
    if (anim_ && now_ms - slot.pose_log_ms >= 1000) {
        slot.pose_log_ms = now_ms;
        Vec3 at{};
        Vec3 asked{};
        ReadValue(guest, slot.actor + addr::kActorPosition, at);
        ReadValue(guest, slot.actor + addr::kActorPositionRequest, asked);
        Say("P" + std::to_string(slot.number) + " pose: at " + VecText(at) + ", request " +
            VecText(asked) + ", now to " + VecText(position));
    }
    WriteValue(guest, slot.actor + addr::kActorPosition, position);
    WriteValue(guest, slot.actor + addr::kActorPositionRequest, position);
    if (rotation != nullptr) {
        WriteValue(guest, slot.actor + addr::kActorRotation, *rotation);
        WriteValue(guest, slot.actor + addr::kActorRotationRequest, *rotation);
    }
}

// The actor taken from the request, while its vtable is the one it had then
// (an actor the engine freed and reused is not written to).
std::uint64_t Director::Resolve(Slot& slot, Guest& guest) {
    std::uint64_t vtable = 0;
    if (slot.taken == 0 || !ReadValue(guest, slot.taken, vtable) || vtable != slot.vtable) {
        return 0;
    }
    return slot.taken;
}

// While creating, before the actor is taken: what the request says.
void Director::Poll(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    std::uint64_t request = 0;
    std::uint32_t word = 0;
    if (slot.taken != 0 || slot.take || slot.drop || slot.handle == 0 ||
        !ReadValue(guest, slot.handle, request) || request == 0 ||
        !ReadValue(guest, request, word)) {
        return;
    }
    const std::uint32_t state = word & 0xffff;
    slot.request = request;
    if (state == addr::kRequestReady) {
        slot.take = true;
    } else if (state == addr::kRequestDrop) {
        slot.drop = true;
    } else if (state == 5 || state == 6) {
        Say("P" + std::to_string(slot.number) + " " + slot.actor_name + ": request " +
            Hex(request) + " failed (state " + std::to_string(state) + "); trying again in 5 s");
        WriteValue(guest, slot.handle, std::uint64_t{0});
        slot.phase = Phase::idle;
        slot.retry_ms = now_ms + 5000;
    }
}

void Director::Status(Guest& guest, std::uint64_t now_ms) {
    if (!verbose_) {
        return;
    }
    if (status_ms_ == 0) {
        status_ms_ = now_ms;
        frames_at_status_ = frames_;
        return;
    }
    if (now_ms - status_ms_ < 1000) {
        return;
    }
    const double seconds = double(now_ms - status_ms_) / 1000.0;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f cave frames/s, scene '%s', %zu wanted",
                  double(frames_ - frames_at_status_) / seconds, input_.scene.c_str(),
                  input_.targets.size());
    Say(buf);
    status_ms_ = now_ms;
    frames_at_status_ = frames_;
    for (const auto& slot : slots_) {
        std::string line = "P" + std::to_string(slot.number) + " " + PhaseText(slot.phase);
        if (slot.actor != 0) {
            std::uint32_t state = 0;
            Vec3 at{};
            ReadValue(guest, slot.actor + addr::kActorState, state);
            ReadValue(guest, slot.actor + addr::kActorPosition, at);
            line += " actor " + Hex(slot.actor) + " state " + std::to_string(state) + " at " +
                    VecText(at);
        }
        if (!slot.anim_name.empty()) {
            const auto frame = slot.actor ? Frame(guest, Controller(guest, slot.actor))
                                          : std::nullopt;
            char buf[32];
            std::snprintf(buf, sizeof(buf), " frame %.1f", frame.value_or(-1.0f));
            line += " plays '" + slot.anim_name + "'" + buf;
        }
        if (const Target* t = Wanted(slot.number)) {
            line += ", wanted at " + VecText(t->position);
            line += t->show == Show::show ? " (show)" : t->show == Show::hold ? " (hold)" : " (hide)";
        }
        // While it is being made, what the handle holds, to see it fill in.
        if (std::uint64_t h = 0; slot.phase == Phase::creating && slot.handle != 0 &&
                                 ReadValue(guest, slot.handle, h) && h != 0) {
            std::array<std::uint64_t, 8> words{};
            guest.Read(h, words.data(), sizeof(words));
            line += "; handle holds " + Hex(h) + ":";
            for (auto w : words) {
                line += " " + Hex(w);
            }
        }
        Say(line);
    }
}

Gear GearOf(const coop::protocol::Equipment& e) {
    Gear gear{e.weapon, e.shield,     e.head,         e.armor, e.boots,   e.held,
              e.hand,   e.sub_weapon, e.third_weapon, e.tool,  e.sub_tool};
    // What is really in the sword hand (a Deku stick) goes before the
    // equipped sword's name; an empty slot leaves the flag's name.
    if (e.weapon_slot && !e.weapon_slot->empty()) {
        gear[0] = e.weapon_slot;
    }
    return gear;
}

Input Planner::Plan(std::uint64_t main_base, const std::optional<std::string>& scene,
                    const std::optional<Vec3>& position, bool local_event,
                    const std::vector<Remote>& remotes, std::uint64_t now_ms,
                    const Gear& own_gear,
                    std::optional<std::uint8_t> own_drawn) {
    Input in;
    if (main_base == 0) {
        return in;
    }
    in.main_base = main_base;
    in.scene = scene.value_or("");
    if (in.scene != scene_) {
        scene_ = in.scene;
        scene_since_ms_ = now_ms;
        debug_spot_.reset();
    }
    if (in.scene.empty()) {
        return in;
    }
    for (const auto& r : remotes) {
        if (r.number == 0 || !(r.fade > 0.0f)) {
            continue;
        }
        const std::optional<std::string_view> theirs =
            r.scene ? std::optional<std::string_view>(*r.scene) : std::nullopt;
        Show show = Show::show;
        switch (coop::host::visibility(in.scene, local_event, theirs, r.status)) {
        case coop::host::Visibility::hide:
            if (!coop::host::same_place(in.scene, theirs)) {
                continue;  // not here: no puppet (one made is deleted)
            }
            show = Show::hide;  // our cutscene: kept, out of sight
            break;
        case coop::host::Visibility::hold:
            show = Show::hold;
            break;
        case coop::host::Visibility::show:
            break;
        }
        Target t;
        t.number = r.number;
        t.actor = ActorFor(r.adult);
        t.position = r.position;
        if (r.rotation) {
            t.rotation = QuatToMatrix(*r.rotation);
        }
        t.show = show;
        t.anim = r.anim;
        t.anim_serial = r.anim_serial;
        t.gear = r.gear;
        t.drawn = r.drawn;
        in.targets.push_back(std::move(t));
    }
    if (settings_.debug && position && now_ms - scene_since_ms_ >= settings_.debug_delay_ms) {
        if (!debug_spot_) {
            debug_spot_ = Vec3{(*position)[0] + settings_.debug_offset[0],
                               (*position)[1] + settings_.debug_offset[1],
                               (*position)[2] + settings_.debug_offset[2]};
        }
        Target t;
        t.number = 0;
        t.actor = ActorFor(false);
        t.position = *debug_spot_;
        t.gear = own_gear;
        t.drawn = own_drawn;
        in.targets.push_back(std::move(t));
    }
    return in;
}

std::string Planner::ActorFor(bool adult) const {
    if (!settings_.actor.empty()) {
        return settings_.actor;
    }
    return adult ? "CoopPuppet_Adult" : "CoopPuppet_Child";
}

namespace {

std::once_flag g_once;
std::atomic<bool> g_configured = false;
std::unique_ptr<Director> g_director;
std::unique_ptr<Planner> g_planner;  // the present thread only

// Each remote player's newest animation, from the network thread.
struct Received {
    Anim anim;
    std::uint32_t serial = 0;
};
std::mutex g_anim_mutex;
std::array<std::optional<Received>, 5> g_anims;  // by player number; guarded
std::array<Gear, 5> g_gear;  // the same; guarded
std::array<std::optional<std::uint8_t>, 5> g_drawn;                // the same; guarded

} // namespace

void OnEvent(const coop::net::Event& event) {
    if (!g_configured) {
        return;
    }
    using coop::net::EventKind;
    if (event.kind == EventKind::disconnected) {
        std::lock_guard lock(g_anim_mutex);
        g_anims = {};
        g_gear = {};
        g_drawn = {};
    } else if (event.kind == EventKind::player_left && event.player < g_anims.size()) {
        std::lock_guard lock(g_anim_mutex);
        g_anims[event.player].reset();
        g_gear[event.player] = {};
        g_drawn[event.player].reset();
    } else if (event.kind == EventKind::player_equip && event.player != 0 &&
               event.player < g_gear.size()) {
        const auto& e = event.equipment;
        g_director->Note("P" + std::to_string(event.player) + " PLAYER_EQUIP: weapon '" +
                         e.weapon.value_or("?") + "', shield '" + e.shield.value_or("?") + "', held '" +
                         e.held.value_or("?") + "', slots 0 '" + e.weapon_slot.value_or("?") +
                         "' 1 '" + e.sub_weapon.value_or("?") + "' 2 '" +
                         e.third_weapon.value_or("?") + "' 5 '" + e.tool.value_or("?") + "' 6 '" +
                         e.sub_tool.value_or("?") + "', hand '" + e.hand.value_or("?") + "'");
        std::lock_guard lock(g_anim_mutex);
        g_gear[event.player] = GearOf(e);
        g_drawn[event.player] = e.drawn;
    } else if (event.kind == EventKind::message) {
        const auto* sample = std::get_if<coop::protocol::PlayerSample>(&event.message);
        if (sample == nullptr || !sample->animation || sample->player_number == 0 ||
            sample->player_number >= g_anims.size()) {
            return;
        }
        std::lock_guard lock(g_anim_mutex);
        auto& slot = g_anims[sample->player_number];
        const std::uint32_t serial = slot ? slot->serial + 1 : 1;
        slot = Received{{sample->animation->name, sample->animation->frame,
                         sample->animation->rate},
                        serial};
    }
}

void Configure(PlannerSettings settings, bool verbose, Director::Log log) {
    std::call_once(g_once, [&] {
        g_director = std::make_unique<Director>(std::move(log), verbose, settings.debug_anim);
        g_director->SetPoseCopy(true);
        g_planner = std::make_unique<Planner>(std::move(settings));
        g_configured = true;
    });
}

Director& TheDirector() {
    Configure({}, false, {});
    return *g_director;
}

void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us) {
    if (!g_configured) {
        return;  // puppets are off
    }
    std::vector<Remote> remotes;
    Gear own_gear;
    std::optional<std::uint8_t> own_drawn;
    if (session != nullptr) {
        if (const auto& e = session->sampler().last_equipment()) {
            own_gear = GearOf(*e);
            own_drawn = e->drawn;
        }
    }
    if (session != nullptr && server_now_us != 0) {
        session->with_presence([&](coop::presence::Presence& p) {
            for (const std::uint8_t n : p.players()) {
                // The same time as the markers' collect() this frame.
                const auto pose = p.shown(n, server_now_us);
                if (!pose) {
                    continue;
                }
                Remote r;
                r.number = n;
                r.position = pose->position;
                r.rotation = pose->rotation;
                r.fade = pose->fade;
                if (const auto* player = p.player(n)) {
                    r.scene = player->scene();
                    r.status = player->flags();
                    r.adult = player->age() == coop::protocol::Age::adult;
                }
                remotes.push_back(std::move(r));
            }
        });
        std::lock_guard lock(g_anim_mutex);
        for (auto& r : remotes) {
            if (r.number < g_anims.size() && g_anims[r.number]) {
                r.anim = g_anims[r.number]->anim;
                r.anim_serial = g_anims[r.number]->serial;
            }
            if (r.number < g_gear.size()) {
                r.gear = g_gear[r.number];
                r.drawn = g_drawn[r.number];
            }
        }
    }
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    auto input = g_planner->Plan(main_base, scene, position, local_event, remotes, now_ms, own_gear, own_drawn);
    // The camera as the hook's markers judge it (kCameraStale there).
    static bool camera_seen = false;           // the present thread only
    static std::uint64_t drawn_since_ms = 0;
    const auto camera = CoopCamera::Latest();
    const bool drawn = camera && std::chrono::steady_clock::now() - camera->at <
                                     std::chrono::milliseconds(250);
    if (!drawn) {
        drawn_since_ms = 0;
    } else if (drawn_since_ms == 0) {
        drawn_since_ms = now_ms;
    }
    camera_seen = camera_seen || drawn;
    input.camera = camera_seen;
    input.drawn_since_ms = drawn_since_ms;
    input.settle_ms = 1000;
    g_director->SetInput(std::move(input));
}

} // namespace CoopPuppet
