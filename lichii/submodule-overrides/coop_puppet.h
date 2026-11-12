// Puppets: a remote player shown as an actor in the local game (plan stage 1
// step 2, docs/puppet.md). No emulator code here, so it is tested on its own.
//
// Two threads:
// - the present thread (CoopHook::OnFrame) calls Publish: it decides what
//   should be shown (Planner) and hands it to the Director;
// - the game's calc thread runs the code cave once per game frame
//   (coop_svc.s). Each `svc #0xf4` from it reaches Director::Step, which
//   reads and writes guest memory directly and returns at most one game
//   function for the cave to call: malloc for a puppet's memory block, the
//   create request, the delete request.
// The pose is written straight into the actor every game frame.
#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coop::protocol {
struct Equipment;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopPuppet {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;  // x, y, z, w
using Mat3 = std::array<float, 9>;  // rows of 3, as the actor holds it

// Offsets into main of build e0f3ccfaf50065ae (docs/puppet.md).
namespace addr {
inline constexpr std::uint64_t kCreate = 0x372a70;          // requestCreateActorAsync
inline constexpr std::uint64_t kActorManagerPtr = 0x4e91888;
inline constexpr std::uint64_t kMalloc = 0x14c6c90;
inline constexpr std::uint64_t kRequestDelete = 0xa6490;    // (actor, 0)
// What the create puts in the handle slot is a request: its low 16 bits at
// +0 are its state, +8 the actor. SeqExecuteCreateActor's check (0x32bcb30)
// waits for state 3, then takes the actor with 0x36fbd0(request, 1, 0),
// which wakes it (0x94f30) and returns it, and clears the slot. State 4 it
// hands to 0x36fde0(request); 5 and 6 are failures.
inline constexpr std::uint64_t kTakeCreated = 0x36fbd0;     // (request, 1, 0) -> actor
inline constexpr std::uint64_t kDropCreate = 0x36fde0;      // (request)
inline constexpr std::uint64_t kRequestActor = 0x08;
inline constexpr std::uint32_t kRequestReady = 3;
inline constexpr std::uint32_t kRequestDrop = 4;
// The GOT entry the preload walker reads for create argument +0x40.
inline constexpr std::uint64_t kGotCreateDefault = 0x4c7dad0;

inline constexpr std::uint64_t kActorState = 0x2c;
inline constexpr std::uint64_t kActorPosition = 0x31c;
inline constexpr std::uint64_t kActorRotation = 0x328;
// The pending transform the engine's own setter 0xc6fd60(actor, mtx34, 0)
// (the run of OneShotActorRequestSetMtx, 0x28b2050) leaves on an actor with
// no physics body: position at +0x358, rotation rows at +0x364. The actor's
// calc applies it to +0x31c and +0x328; written with them every frame.
inline constexpr std::uint64_t kActorPositionRequest = 0x358;
inline constexpr std::uint64_t kActorRotationRequest = 0x364;
inline constexpr std::uint32_t kStatePresent = 4;
inline constexpr std::uint64_t kLinkHolder = 0x08;
inline constexpr std::uint64_t kLinkId = 0x10;
inline constexpr std::uint64_t kHolderActor = 0x40;
inline constexpr std::uint64_t kHolderId = 0x48;

// Animation (docs/puppet.md, Animation). The AS controller is actor +0x270,
// then +0x40, +0x18, +0x18; its slot count at +0x18, slots at +0x20 (0x48
// bytes each, the first 8 point to the slot); slot +0x10 the command name,
// +0xe8 the frame.
inline constexpr std::uint64_t kPlayAs = 0x2022a0;      // (request)
inline constexpr std::uint64_t kSetAsFrame = 0x37b5d70; // (controller, slot, s0 = frame)
inline constexpr std::array<std::uint64_t, 4> kControllerPath{0x270, 0x40, 0x18, 0x18};
inline constexpr std::uint64_t kSlotCount = 0x18;
inline constexpr std::uint64_t kSlots = 0x20;
inline constexpr std::uint64_t kSlotName = 0x10;
inline constexpr std::uint64_t kSlotFrame = 0xe8;
// ExecuteASSyncPartialAnimation's call: 0x3abec10(actor, from slot, to slot,
// node 0, 0) plays the command of one slot on another (request skip-if-same
// 1, the source's rate) through 0x37b59c0. Link's upper body and other
// partial slots are filled this way from slot 0.
inline constexpr std::uint64_t kSyncAsSlot = 0x3abec10;
inline constexpr std::int32_t kMaxAsSlots = 8;  // slots synced from slot 0

// Gear (docs/puppet.md, Equipment): the EquipmentUser is [[actor + 0x270] +
// 0x200]; 0x2df7b20(eu, kind, &char*, state 2, -1, 1, 0) wears the named
// piece in a slot kind, as the game's own GameData path does.
inline constexpr std::uint64_t kEquip = 0x2df7b20;
inline constexpr std::uint64_t kActorParts = 0x270;
inline constexpr std::uint64_t kPartsEquipmentUser = 0x200;
// In hand or put away: 0xe34c40(eu, slot, state) is the game's own change of
// an equipment slot's state (the shield node 0x2797850 calls it with slot 4,
// state 1). State 1 is in hand (0x6b6820), 2 put away on the back
// (0xe34c70, the state a piece is worn in). Each sets a bit in the slot's
// byte at EquipmentUser + 0x13bc + slot (2 in hand, 4 put away), which is how
// a call that was taken is told from one that was not. The slot's link is at
// EquipmentUser + 0x20 + 0x18 * slot (u32 id at +0x10, ~0 empty).
inline constexpr std::uint64_t kEquipChangeState = 0xe34c40;
inline constexpr std::uint32_t kEquipStateInHand = 1;
inline constexpr std::uint32_t kEquipStateAway = 2;
inline constexpr std::uint64_t kEquipSlots = 0x20;
inline constexpr std::uint64_t kEquipSlotStride = 0x18;
inline constexpr std::uint64_t kEquipSlotHolder = 0x08;
inline constexpr std::uint64_t kEquipSlotId = 0x10;
inline constexpr std::uint64_t kEquipSlotBits = 0x13bc;
inline constexpr std::uint8_t kEquipBitInHand = 0x2;
inline constexpr std::uint8_t kEquipBitAway = 0x4;
// The byte is a request the game clears when the change is done (seen in a
// two-player run, 2026-11-09), and a setter does nothing when the piece is in
// that state already. The user's slot records (EquipmentUser + 0x4b0, 0xa8
// each) are only logged (LICHII_COOP_DEBUG_ANIM): their byte at +0xa4 read 1
// with the sword out and put away alike (autotest, 2026-11-09), so it is not
// the state.
inline constexpr std::uint64_t kEquipRecords = 0x4b0;
inline constexpr std::uint64_t kEquipRecordStride = 0xa8;
inline constexpr std::uint64_t kEquipRecordInHand = 0xa4;
inline constexpr std::uint64_t kEquipRecordFlags = 0xa0;  // u32, logged only
// The game's own way to move an actor that has a physics body, as the AI
// node ExecuteActorForceSetMtx (run 0x2622e50): write +0x31c..+0x34b, then
// call vtable slot 0x218, which is 0x4c6580(actor, 0) for the vtable every
// actor seen has (docs/objects.md). A plain write is undone within a frame.
inline constexpr std::uint64_t kActorVtable = 0x4b25e18;
inline constexpr std::uint64_t kSetMatrix = 0x4c6580;
}  // namespace addr

// The slot kinds the puppet wears, in Target::gear order: weapon, shield,
// head, armor, boots (reports/s2-equip-create: kinds 0, 6, 7, 8, 9), and the
// item in the hand slot (kind 5, the bow kind: 0x2df7b20 tests slot 3's link
// for it; the slingshot is a bow to the game).
//
// The rest (branch held-items-2) follows the game's own two enums, read from
// rodata. The 18 slot kinds the equip call takes (0x3ef94a8): 0 Weapon,
// 1 SubWeapon, 2 ThirdWeapon, 3 Sheath, 4 Quiver, 5 Bow, 6 Shield,
// 7 ArmorHead, 8 ArmorUpper, 9 ArmorLower, 10 ArmorHand, 11 Tool, 12 SubTool,
// 13 to 16 Accessory1 to 4, 17 Extra. The 8 hand slots (0x3ef2033): 0 Weapon,
// 1 SubWeapon, 2 ThirdWeapon, 3 Bow, 4 Shield, 5 Tool, 6 SubTool, 7 Helmet.
// So the pieces after the held item are: the gauntlets (kind 10, no hand
// slot), and whatever sits in hand slots 1, 2, 5 and 6 (kinds 1, 2, 11, 12:
// which of the Deku stick, boomerang, hookshot, bottle and ocarina uses
// which is for a game run to show; all four are sent and worn the same way).
inline constexpr std::size_t kGearCount = 11;
inline constexpr std::size_t kGearHeld = 5;  // index of the held item
inline constexpr std::size_t kGearHand = 6;  // the gauntlets
inline constexpr std::size_t kGearSubWeapon = 7;
inline constexpr std::size_t kGearThirdWeapon = 8;
inline constexpr std::size_t kGearTool = 9;
inline constexpr std::size_t kGearSubTool = 10;
using Gear = std::array<std::optional<std::string>, kGearCount>;
inline constexpr std::array<std::uint32_t, kGearCount> kGearKinds{0, 6, 7, 8, 9, 5,
                                                                  10, 1, 2, 11, 12};
inline constexpr std::array<const char*, kGearCount> kGearNames{
    "weapon", "shield",     "head",         "armor", "boots",   "held",
    "hand",   "sub weapon", "third weapon", "tool",  "sub tool"};
// Pieces the game keeps elsewhere while they are put away (seen for the
// slingshot: state 6), so the left-behind check looks at them only in hand.
constexpr bool GearIsCarried(std::size_t i) {
    return i == kGearHeld || i >= kGearSubWeapon;
}
// The equipment slot each of them lands in (seen in the game: the sword in
// slot 0, the shield in slot 4, the slingshot in slot 3); -1: not a piece
// that is drawn or put away.
inline constexpr std::array<int, kGearCount> kGearSlots{0, 4, -1, -1, -1, 3, -1, 1, 2, 5, 6};

// A puppet's memory block, from the game's malloc, kept for the whole run.
namespace block {
inline constexpr std::uint64_t kSize = 0x680;
inline constexpr std::uint64_t kCreateArg = 0x000;  // 0x100 bytes
inline constexpr std::uint64_t kName = 0x100;       // {char*, u32 capacity}
inline constexpr std::uint64_t kHandles = 0x110;    // kHandleCount slots of 8 bytes
inline constexpr std::uint64_t kHandleCount = 6;
inline constexpr std::uint64_t kNameText = 0x140;   // kNameCapacity bytes
inline constexpr std::uint32_t kNameCapacity = 0x40;
// The play request for 0x2022a0: +0 actor, +8 name, +0x10 slot, +0x14 rate,
// +0x18 and +0x1c -1.0, +0x20 skip-if-same, +0x28 node (0, as 0x2a95c4).
inline constexpr std::uint64_t kPlayRequest = 0x200;  // 0x30 bytes
// Two name buffers, used in turn: the slot may keep the pointer it was
// given, and a new name in the same buffer would look like the same one.
inline constexpr std::uint64_t kAnimText = 0x240;
inline constexpr std::uint64_t kAnimCapacity = 0x50;  // 64 characters and the 0
// One name cell per gear kind (kGearKinds order), and the char* to each that
// the equip call is given.
inline constexpr std::uint64_t kGearText = 0x300;
inline constexpr std::uint64_t kGearCapacity = 0x40;
inline constexpr std::uint64_t kGearPtr = 0x5c0;  // kGearCount x 8 bytes
}  // namespace block

// Guest memory, as the director sees it. Failures are quiet.
class Guest {
public:
    virtual ~Guest() = default;
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
};

// One game function for the cave to call.
struct Call {
    std::uint64_t function = 0;          // absolute
    std::array<std::uint64_t, 8> x{};    // x0 to x7
    std::array<std::uint64_t, 4> stack{};// what the function sees at sp
    float s0 = 0.0f;                     // the first float argument
};

// A remote player's animation, as PLAYER_SAMPLE field 7 carries it.
struct Anim {
    std::string name;  // the AS command
    float frame = 0.0f;
    float rate = 1.0f;
};

// coop/host/scene.hpp's Visibility, for a puppet that exists.
enum class Show { show, hold, hide };

// What one puppet should show.
struct Target {
    std::uint8_t number = 0;  // the player; 0 is the debug puppet
    std::string actor;        // the actor pack name
    Vec3 position{};
    Mat3 rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Show show = Show::show;   // hold: frozen as it is; hide: parked out of sight
    std::optional<Anim> anim;
    std::uint32_t anim_serial = 0;  // goes up with each received animation
    // What the player wears, in kGearKinds order; unset: not known yet,
    // "" nothing (not taken off yet; the puppet keeps what it has).
    Gear gear;
    // PLAYER_EQUIP's drawn bits: bit i set when the player's equipment slot i
    // is in hand (sword out, shield on the arm). Unset: not known.
    std::optional<std::uint8_t> drawn;
};

struct Input {
    std::uint64_t main_base = 0;  // 0: no game, nothing is done
    std::string scene;            // the local scene; "" not known
    std::vector<Target> targets;
    // The local game's drawing, from the renderer's camera (coop_camera.h).
    // A puppet created while the game loads a scene (the scene is already
    // readable) is made but never drawn, so a create waits for the drawing
    // (Director::Drawn). `camera`: a camera has been seen since start; false
    // (no camera, or a test) creates at once, as before.
    bool camera = false;
    std::uint64_t drawn_since_ms = 0;  // the current unbroken run of drawn frames; 0 none
    // A create waits until the wanted actor has stayed the same this long:
    // while the other game loads, its age reads adult for a moment, and a
    // wrong-age puppet is made and dropped. 0 (a test) creates at once.
    std::uint64_t settle_ms = 0;
};

Mat3 QuatToMatrix(const Quat& q);  // rows; normalises q, identity if it is 0

enum class Phase { no_block, allocating, idle, creating, live, deleting };
const char* PhaseText(Phase phase);

class Director {
public:
    using Log = std::function<void(const std::string&)>;
    // `verbose`: a status line per puppet every second (LICHII_COOP_DEBUG_PUPPET).
    // `anim`: a line for each animation played or frame set (LICHII_COOP_DEBUG_ANIM).
    explicit Director(Log log = {}, bool verbose = false, bool anim = false);
    // The Link puppets' director runs the pose copy (coop_pose.h); others, such
    // as the fairies', do not.
    void SetPoseCopy(bool on) { pose_copy_ = on; }
    // Move each puppet with the game's own setter as well (addr::kSetMatrix),
    // once a frame: for an actor with a physics body, such as the fairy.
    void SetMatrixCall(bool on) { matrix_call_ = on; }

    // Any thread. The next frame of the cave uses it.
    void SetInput(Input input);
    // Any thread. Run one game function on the calc thread before the puppet
    // slots (the request memory is the caller's).
    void QueueCall(const Call& call);

    // The game's calc thread, at each `svc #0xf4`. `after_call`: the cave
    // made the call returned last time and `result` is its x0. Returns the
    // next call, or nothing for "done for this frame".
    std::optional<Call> Step(Guest& guest, bool after_call, std::uint64_t result,
                             std::uint64_t now_ms);

    struct SlotView {
        std::uint8_t number = 0;
        Phase phase = Phase::no_block;
        std::uint64_t block = 0;
        std::uint64_t actor = 0;
        std::string actor_name;
        std::string anim_name;  // what was last played on it
    };
    std::vector<SlotView> Slots() const;  // the calc thread, or tests
    std::uint64_t Frames() const { return frames_; }
    void Note(const std::string& text) const { Say(text); }  // any thread

    static constexpr int kMaxCallsPerFrame = 8;      // the cave allows 16
    static constexpr std::uint64_t kCreateTimeoutMs = 10000;
    // A create the game refuses (it returns 0: the scene is still being
    // unloaded or loaded) made nothing; it is asked again after this, not
    // after the create timeout and the 5 s wait.
    static constexpr std::uint64_t kRefusedRetryMs = 1000;
    // After this many refusals in a row (a long load or cutscene), every 5 s.
    static constexpr int kRefusedFast = 3;
    static constexpr std::uint64_t kRefusedSlowMs = 5000;
    // A live puppet whose player drops out of the plan in the same scene
    // (a stalled or late peer fades to 0 for a moment) is kept this long
    // before it is deleted, so a hiccup does not delete and create it again.
    static constexpr std::uint64_t kLeaveGraceMs = 2000;
    // A received frame further than this from the puppet's is set on it.
    static constexpr float kFrameSlack = 3.0f;
    // A hidden puppet is moved this far down, under the floor.
    static constexpr float kHideDrop = 500.0f;
    // A worn piece this far from its shown puppet was left behind (made while
    // the puppet was parked under the floor, seen 2026-11-09): worn again.
    static constexpr float kGearFar = 5.0f;
    static constexpr std::uint64_t kRewearMs = 1000;  // at most once a second
    static constexpr std::uint64_t kFarForMs = 100;   // far on two readings this far apart
    static constexpr std::uint8_t kRewearTries = 3;   // then given up, with one line
    // A create waits until the game has drawn this long without a break,
    // begun after the last scene change or puppet loss (a load drops the
    // camera)...
    static constexpr std::uint64_t kDrawnMs = 750;
    // ...or, when the drawing never broke, this long after it.
    static constexpr std::uint64_t kSteadyMs = 3000;

private:
    struct Slot {
        std::uint8_t number = 0;
        Phase phase = Phase::no_block;
        std::uint64_t block = 0;
        std::uint64_t handle = 0;   // the handle slot of the current create
        std::uint64_t handle_index = 0;
        std::uint64_t actor = 0;    // resolved this frame; 0 none
        std::uint64_t request = 0;  // what the handle slot held
        std::uint64_t taken = 0;    // the actor 0x36fbd0 gave
        std::uint64_t vtable = 0;   // its vtable then, to notice reuse
        bool take = false;          // call 0x36fbd0 next
        bool drop = false;          // call 0x36fde0 next
        std::string actor_name;     // what was created
        std::string scene;          // where it was created
        std::uint64_t since_ms = 0; // when the phase began
        std::uint64_t retry_ms = 0; // no new malloc or create before this
        std::uint64_t missing_ms = 0; // when its player left the plan; 0 wanted
        std::uint64_t lost_ms = 0;    // when the engine deleted it (a load, often)
        bool wait_said = false;       // "waiting for the game to draw" logged
        std::string wanted_actor;     // the actor its target asked for, last seen
        std::uint64_t wanted_since_ms = 0;  // since when (Input::settle_ms)
        int refused = 0;                    // creates refused in a row
        std::string anim_name;        // played on it; "" none yet
        std::uint32_t anim_serial = 0;
        std::uint64_t anim_buffer = 0;  // which name buffer is next
        std::optional<float> frame_to_set;  // after a play
        std::optional<float> held_frame;    // while held
        std::uint64_t frame_set_in = 0;     // the cave frame of the last frame set
        std::string play_name;              // the play being made
        float play_frame = 0.0f;
        std::uint32_t play_serial = 0;
        std::int32_t sync_next = 0;         // next slot to sync from slot 0; 0 none
        std::uint64_t pose_log_ms = 0;      // last read-back line (debug anim)
        std::array<std::string, kGearCount> worn;    // equipped on it, by gear index
        std::size_t equip_index = 0;        // the equip being made
        std::string equip_name;
        std::uint64_t gear_log_ms = 0;      // last gear line (debug anim)
        std::uint8_t in_hand = 0;           // equipment slots asked into the hand (bits)
        std::uint8_t hold_known = 0;        // slots whose state we have set (bits)
        int hold_slot = 0;                  // the change being made
        bool hold_in_hand = false;
        std::uint64_t hold_retry_ms = 0;    // no new change before this
        std::array<std::uint32_t, kGearCount> hold_id{};  // the piece each change was made for
        std::uint64_t rewear_ms = 0;        // no new "left behind" check before this
        std::array<std::uint64_t, kGearCount> far_since_ms{};  // first far reading of each piece; 0 near
        std::array<std::uint8_t, kGearCount> rewear_tries{};   // re-wears since the piece was last near
        std::uint64_t moved_frame = 0;      // the cave frame of the last matrix call
    };
    enum class Pending {
        none, malloc, create, take, drop, remove, play, frame, sync, equip, hold, move
    };

    void NewFrame(Guest& guest, std::uint64_t now_ms);
    void PoseFrame(Guest& guest, std::uint64_t now_ms);
    void Finish(Guest& guest, std::uint64_t result, std::uint64_t now_ms);
    std::optional<Call> Next(Slot& slot, Guest& guest, std::uint64_t now_ms);
    std::optional<Call> Create(Slot& slot, const Target& target, Guest& guest);
    std::uint64_t Resolve(Slot& slot, Guest& guest);
    std::optional<Call> Animate(Slot& slot, const Target& target, Guest& guest);
    std::optional<Call> Equip(Slot& slot, const Target& target, Guest& guest,
                              std::uint64_t now_ms);
    std::optional<Call> Hold(Slot& slot, const Target& target, Guest& guest,
                             std::uint64_t now_ms);
    void LeftBehind(Slot& slot, Guest& guest, std::uint64_t now_ms);
    std::optional<Call> Move(Slot& slot, Guest& guest);
    std::optional<Call> SetFrame(Slot& slot, std::uint64_t controller, float frame);
    static std::uint64_t Controller(Guest& guest, std::uint64_t actor);
    static std::optional<float> Frame(Guest& guest, std::uint64_t controller);
    void Pose(Slot& slot, Guest& guest, const Vec3& position, const Mat3* rotation,
              std::uint64_t now_ms);
    void Poll(Slot& slot, Guest& guest, std::uint64_t now_ms);
    const Target* Wanted(std::uint8_t number) const;
    bool Drawn(const Slot& slot, std::uint64_t now_ms) const;
    Slot& SlotFor(std::uint8_t number);
    void Say(const std::string& text) const;
    void Status(Guest& guest, std::uint64_t now_ms);

    Log log_;
    bool verbose_;
    bool anim_;
    bool pose_copy_ = false;
    bool matrix_call_ = false;

    mutable std::mutex input_mutex_;
    Input next_input_;  // guarded
    std::mutex extra_mutex_;
    std::deque<Call> extra_calls_;

    // The calc thread only.
    Input input_;
    std::vector<Slot> slots_;
    Pending pending_ = Pending::none;
    std::uint8_t pending_slot_ = 0;
    int calls_ = 0;  // this frame
    std::uint64_t frames_ = 0;
    std::uint64_t frames_at_status_ = 0;
    std::uint64_t status_ms_ = 0;
    std::string scene_;               // input_.scene as last seen
    std::uint64_t scene_since_ms_ = 0;
};

// The present thread's side: what to show this frame.
// A player's PLAYER_EQUIP as the puppet's pieces, in kGearKinds order.
Gear GearOf(const coop::protocol::Equipment& equipment);

struct Remote {
    std::uint8_t number = 0;
    std::optional<std::string> scene;
    Vec3 position{};
    std::optional<Quat> rotation;
    float fade = 1.0f;
    std::uint8_t status = 0;  // PLAYER_STATUS flags
    bool adult = false;
    std::optional<Anim> anim;
    std::uint32_t anim_serial = 0;
    Gear gear;  // as Target::gear
    std::optional<std::uint8_t> drawn;               // as Target::drawn
};

struct PlannerSettings {
    // The actor pack; "" picks CoopPuppet_Child or CoopPuppet_Adult by the
    // player's age (LICHII_COOP_PUPPET_ACTOR overrides it).
    std::string actor;
    bool debug_anim = false;      // LICHII_COOP_DEBUG_ANIM
    bool debug = false;           // LICHII_COOP_TEST_PUPPET: one more, beside you,
                                  // wearing your own gear
    std::uint64_t debug_delay_ms = 3000;  // after the scene is known
    Vec3 debug_offset{2.0f, 0.0f, 0.0f};  // from you, in game units (about metres)
};

class Planner {
public:
    explicit Planner(PlannerSettings settings = {}) : settings_(std::move(settings)) {}
    // `local_event`: our own cutscene runs (it hides every puppet).
    Input Plan(std::uint64_t main_base, const std::optional<std::string>& scene,
               const std::optional<Vec3>& position, bool local_event,
               const std::vector<Remote>& remotes, std::uint64_t now_ms,
               const Gear& own_gear = {},
               std::optional<std::uint8_t> own_drawn = {});
    std::string ActorFor(bool adult) const;
    const PlannerSettings& settings() const { return settings_; }

private:
    PlannerSettings settings_;
    std::string scene_;
    std::uint64_t scene_since_ms_ = 0;
    std::optional<Vec3> debug_spot_;
};

// The process-wide pair the hook and the SVC handler share.
// Configure once, before the game starts (CoopSvc does it).
void Configure(PlannerSettings settings, bool verbose, Director::Log log);
Director& TheDirector();

// CoopHook::OnFrame, once per present, with no lock held. `session` may be
// null (not connected): then only the debug puppet is planned.
void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us);

// The session's on_event (network thread): keeps each remote player's latest
// animation from PLAYER_SAMPLE field 7 and gear from PLAYER_EQUIP.
void OnEvent(const coop::net::Event& event);

} // namespace CoopPuppet
