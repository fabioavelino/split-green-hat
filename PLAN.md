# Ocarina of Time — Single-Instance Two-Player Plan

Status: **Phase B approved — single running game instance, two real Links**
Repository: `https://github.com/fabioavelino/split-green-hat`
Target game: *The Legend of Zelda: Ocarina of Time* (Switch 2), title `0400486024DEC000`, update `v131072`, game build `e0f3ccfaf50065ae`, display version 1.1.1
Emulator base: `Frizti/Fritchii`, branch `oot-gather-fix`, tag `fritchii-v5.6` (`696a456641ae83c9f8b6564c57ac311c7ef56799`)
Build target: **Windows x64, ready-to-run portable kit**
Reference code: `daurosk/oot-remake-multiplayer` (GPL-3.0) for memory reading, actor creation, camera tracking and code caves

Current milestone: **M2.** The Windows x64 kit patches the game, spawns a controllable second Link and renders a two-camera split-screen (left/right or top/bottom) with a chase camera for Link 2. The camera override and presenter are confirmed on macOS. Shared health, game over, cutscene takeover and enemy HP scaling are implemented; M3/M4 need full-speed Windows validation.

---

## 1. Goal

One running copy of the game, two real Links, one shared world, one shared inventory and one shared health pool.

The player must be able to:

- control Link 1 with gamepad 1 and Link 2 with gamepad 2;
- play in a horizontal or vertical split-screen layout;
- share every inventory item, equipment piece, song, bottle, dungeon item, rupee, ammo count, magic value and health value;
- see a cutscene once, full-screen, with the first player who entered it taking over the whole screen;
- get a game over when either Link dies, with the game-over screen appearing once and Continue restarting both Links;
- fight enemies whose HP is multiplied by a configurable factor (default 2×, range 1×–4×).

There is no network co-op in this design. Both players are inside the same emulated game.

## 2. Why single-instance

Running two Fritchii instances costs two full guest memories, two GPU contexts and two copies of the game logic. A single instance:

- runs the game logic, physics, AI, world and save state once;
- shares inventory/health/flags naturally because there is only one game state;
- shows one cutscene and one game-over screen naturally;
- still pays roughly twice the 3D scene draw cost for split-screen, but not twice the RAM or CPU logic cost.

This is the resource-conscious architecture the user asked for.

## 3. Locked decisions

| Decision | Choice |
|---|---|
| Architecture | **Single game instance**, two real Links inside it |
| Players | Two Links (`Player_Child` / `Player_Adult`), not literal Zelda models |
| Inventory | **Everything shared**, including rupees, ammo, magic, bottles, health |
| Health | One shared pool. Damage to either Link reduces the same bar. Either Link reaching zero triggers the same game over |
| Cutscenes | Shown once, full-screen. **First player** who enters the cutscene owns the screen |
| Game over | Triggered when the game’s **game-over screen** appears; Continue restarts both Links |
| Enemy HP | Configurable 1×–4×, default **2×**, bosses included |
| Split layout | User-selectable **horizontal or vertical** |
| Controllers | Two gamepads. Lichii already supports two controllers; controller 2 feeds Link 2 |
| Emulator branch | `oot-gather-fix` (`fritchii-v5.6`) |
| Build | Windows x64, portable, ready-to-run kit |
| Hardware | No artificial limit. Aim for the best frame rate the machine can deliver |
| Game file | User’s own `.nup`; no game files, keys, firmware or saves are distributed |

## 4. High-level architecture

```
┌─────────────────────────────────────────────────────────────┐
│ Fritchii (oot-gather-fix v5.6) + split patches              │
│                                                             │
│  ┌─────────────────────┐   ┌─────────────────────────────┐  │
│  │ Game (one instance) │   │ Split module (emulator)     │  │
│  │                     │   │                             │  │
│  │ Link 1 (original)   │   │ player 2 actor manager      │  │
│  │ Link 2 (SplitLink)  │◄──┤ input router                │  │
│  │ one inventory       │   │ camera 2 / viewport layout  │  │
│  │ one health pool     │   │ second scene render pass    │  │
│  │ one event system    │   │ enemy HP scaler             │  │
│  │ one game over       │   │ shared-health bridge        │  │
│  └─────────────────────┘   └─────────────────────────────┘  │
│                                                             │
│  Vulkan renderer: scene pass 1 (top/left), scene pass 2     │
│  (bottom/right), HUD once, cutscene full-screen             │
└─────────────────────────────────────────────────────────────┘
```

No co-op server, no ENet, no second guest. The split module lives inside the emulator process and is driven once per presented frame, as the existing co-op hook is.

## 5. Technical design

### 5.1 Emulator base and patch series

We build on `Fritchii` at `oot-gather-fix` / `fritchii-v5.6`.

Existing co-op patches:

- `0001-coop-hook.patch` applies cleanly to v5.6.
- `0002-coop-camera.patch` must be ported because `vk_rasterizer.cpp` moved.
- `0003-coop-puppet.patch` must be ported because `patch_manager.cpp` and `svc.cpp` moved.

Our patch series will be kept small and rebased on v5.6:

1. `0001-split-hook.patch` — adds `LICHII_SPLIT`, includes the split module in `core` and the Qt frontend, calls `SplitHook::OnFrame` from both present paths.
2. `0002-split-camera.patch` — hands the game’s main camera to the split module from the Vulkan draw path, and provides the hook point for the second scene pass.
3. `0003-split-actor.patch` — exposes the actor-creation path and the calc-thread code cave needed for a second Link, input routing and HP scaling.
4. `0004-split-render.patch` — adds the second scene pass/viewport support in the Vulkan backend.

The rest of the code lives under `split/` and is built into the emulator, so changing it does not require a new patch.

### 5.2 Second Link

Preferred approach: **build a playable `SplitLink` actor from the game’s own player pack**, not clone the singleton blindly.

The reference project already creates a `CoopPuppet_Child` actor from the game’s model/animation files. We extend that idea:

- Merge the game’s `Player_Child`/`Player_Adult` actor pack with the components a real player needs:
  - `Player_Base` movement/physics/collision,
  - animation/AS controller,
  - `Life` component,
  - `EquipmentUser`,
  - `PlayerCameraTarget`,
  - `XLink`/`SLink` as needed,
  - the player’s AI/BSA graph or a minimal custom controller.
- Give it a unique actor name (`SplitLink_Child` / `SplitLink_Adult`) and a custom actor parameter pack.
- Create it through the game’s own `requestCreateActorAsync` (`main+0x372a70`) at the player’s position.
- Keep it out of the single-player save/player-manager path, so the original Link remains the story owner.

Fallback if the custom actor cannot run full player logic:

- Clone the original player actor and its components, redirect input and health, and keep the story/player-manager tied to Link 1.
- This is riskier because components may share pointers, but it is the backup path.

The second Link must support, at minimum:

- walking/running/turning/jumping;
- collision with the world;
- sword/shield/item use;
- picking up items (which feed the shared inventory);
- taking damage (which feeds the shared health pool);
- being hidden/frozen during cutscenes.

### 5.3 Input routing

Lichii already exposes two emulated controllers. Player 1 uses `NpadIdType::Player1`; player 2 uses `NpadIdType::Player2`.

The game normally polls player 1 only. We add a split input router:

- Read controller 2’s state from Lichii’s HID core once per frame.
- Store it in a shadow input block.
- Hook the second Link’s input read so it sees controller 2, while Link 1 still sees controller 1.
- Preferred hook point: the game’s player input component/blackboard; write the shadow state into it on the calc thread.
- Fallback hook point: a code cave around the game’s controller read, using a “current player” flag set while the second Link’s update runs.

Acceptance: moving controller 2 moves only Link 2; controller 1 moves only Link 1; no input leaks between them.

### 5.4 Split-screen rendering

The game renders one 3D scene with one camera. We render it twice:

- **View 1** uses the game’s existing camera for Link 1.
- **View 2** uses a second camera for Link 2.
- The render target is split horizontally or vertically.
- The HUD/UI is drawn once after both views, because inventory and health are shared. A divider line is drawn by the emulator overlay.
- During a cutscene, only one view is drawn, full-screen.

Implementation path in the Vulkan backend:

1. Identify the main 3D scene draws and the camera uniform/matrix. The reference co-op camera tracker already finds the camera matrix in the draw path.
2. Render the scene once for view 1 into its viewport.
3. Replay the same scene draws for view 2 with:
   - a second camera matrix (from the Link 2 camera),
   - the second viewport/scissor,
   - a cleared depth range for that viewport.
4. Skip or adapt full-screen post-processing that assumes one camera. Start with scene-only split and keep the game’s tonemap/AA applied to the combined image; if seams appear, add per-view post-processing later.
5. Draw the HUD/UI once over the combined image.
6. During cutscenes, disable view 2 and set view 1 to full-screen.

Known limitations for the first version:

- The game’s culling/LOD is computed for Link 1. Link 2 may see missing distant objects or wrong LOD. Mitigation: expand culling or disable it for the second view; accept minor pop-in initially.
- Some effects (reflections, shadow cascades, screen-space effects) may be camera-specific. Mitigation: reuse view 1’s buffers where possible; fix the worst offenders after the first playable build.
- The second view may cost ~2× GPU for the 3D scene. The user has no hardware limit; we still add a half-resolution second-view option for weaker machines.

### 5.5 Link 2 camera

First version: a simple third-person chase camera computed by the split module:

- position = Link 2 position − forward × distance + up × height;
- look at Link 2’s head;
- same FOV/projection as the game camera;
- collision handling added after the first playable build.

Later version: create or reuse a second `PlayerCamera` actor and let the game’s own chase/lock-on/collision logic run for Link 2. This is the better long-term solution but not required for the first playable build.

During cutscenes, the game’s event camera is used and view 2 is disabled.

### 5.6 Shared inventory and health

Because there is one game instance, the inventory and most flags are already shared. The split module ensures:

- Link 2’s pickups write to the same `PouchData`/`GameData` flags as Link 1.
- Link 2’s equipment uses the same `PlayerStatus.Equip*` flags.
- Link 2’s `Life` component mirrors Link 1’s `Life` component, or both point at the same health pool.
- Damage to either Link reduces the shared health.
- If either Link’s health reaches zero, the shared health becomes zero and the game’s game-over flow runs.
- Rupees, ammo, magic and bottle contents are shared because they are the same player status.

The HUD shows one health bar and one inventory, which is correct for a shared-everything run.

### 5.7 Cutscenes

- The game’s event system remains single-player. The first player who triggers a cutscene owns it.
- The split renderer switches to one full-screen view for the duration.
- Link 2 is frozen and hidden, or teleported to Link 1 if the cutscene expects both characters; start with frozen/hidden.
- When the cutscene ends, split-screen resumes with both Links at their last valid positions.
- If a cutscene teleports Link 1, Link 2 is moved to a safe position near Link 1 after the event.

### 5.8 Game over and Continue

- The game over is detected from the game’s own game-over screen/state, not only from life reaching zero.
- When the game-over screen appears, both Links are considered dead.
- Continue/Retry restarts both Links at the game’s checkpoint:
  - Link 1 uses the game’s normal respawn.
  - Link 2 is respawned next to Link 1 with full shared health.
- There is no independent revive. The run is shared.

### 5.9 Enemy HP scaling

- Setting: `enemy_hp = 2.0` in `split.ini`, range `1.0`–`4.0`.
- At enemy spawn/first sight, multiply its `Life` max and current value by the factor.
- Apply once per enemy instance; track by placement hash and actor address.
- Apply again after a respawn/reset.
- Bosses and minibosses included.
- The game’s damage/death logic continues to work because the Life component is the game’s own.

### 5.10 HUD and UI

- One HUD, one pause menu, one inventory, because the run is shared.
- Link 2 does not get a separate HUD.
- A small debug overlay lists: frame rate, split mode, Link 2 actor state, Link 2 camera, shared health, enemy HP factor.
- The existing Fritchii performance overlay and debug server remain available.

### 5.11 Settings

`split.ini` in the emulator config folder:

```ini
enabled = 1
layout = vertical          # vertical or horizontal
enemy_hp = 2.0             # 1.0 to 4.0
player2_controller = 2     # Lichii controller index
cutscene_mode = first      # first player to enter owns the screen
game_over = shared         # shared game over and Continue
second_view_scale = 1.0    # optional performance scale, 0.5 to 1.0
```

Invalid values are logged and fall back to safe defaults. A hotkey can toggle split-screen for debugging.

## 6. Milestones

Each milestone has a go/no-go. The project does not move on until the previous milestone is seen working in the real game.

### M0 — Baseline and instrumentation

- Port the reference co-op hooks to `oot-gather-fix` v5.6.
- Add `LICHII_SPLIT` build option and a `split/` module skeleton.
- Add `SplitHook::OnFrame` to both present paths.
- Add debug logs and a debug overlay.
- Add a Windows x64 CI workflow and a local build script.
- Build and boot the game with split disabled; verify the emulator is unchanged.

### M1 — Second real Link

- Build the `SplitLink` actor pack from the game’s player assets.
- Spawn it at Link 1’s position.
- Verify it renders, animates and collides.
- Add input routing from controller 2.
- Verify independent movement and no input leak.
- Go/no-go: if a second real Link cannot run player logic, try the clone fallback. If both fail, stop and report before touching the renderer.

### M2 — Split-screen rendering

- Capture the main scene camera and draw range. **Done** (176-byte scene camera UBO).
- Add the second camera and second viewport. **Done** (alternate-frame cameras, two history images).
- Render scene view 1 and view 2 into the split layout. **Done**.
- Verify horizontal and vertical modes. **Both done on macOS**: player 1 is centred in the left/top half; player 2's chase view fills the right/bottom half. Top/bottom is a centre crop, not a half-height viewport.
- Verify the HUD and post-processing do not break. **Partial**: the HUD stays at the screen edges, but its left half comes from view 0 and its right half from view 1.
- Go/no-go: if the scene cannot be replayed without breaking the frame, report and reassess. **Passed**.

### M3 — Shared systems

- Shared health bridge and damage propagation. **Shared pool done**: the
  real-body fallback (`Dm_Player_Child_Op`) gets Link 1's Life pointer in its
  empty Life slot, so both actors share one Life object, and the shared pool
  reaching 0 raises the game's own Game Over screen. Damage still has to come
  to Link 1: Link 2 is a puppet/cutscene body and cannot take damage itself.
- Game-over detection and shared Continue. **Done for the shared pool**: the
  actor `Life` drain to 0 starts the game's own death flow and the Game Over
  screen (`Try Again` / `Quit Game`) appears after the fade. `Try Again`
  reloads the checkpoint and restores Link 1; the clean real-body run then
  logs `shared health restored (16/32), showing Link 2`, recreates
  `Dm_Player_Child_Op` (actor `0x214c123c00`, state 4) and re-applies the
  shared Life pointer (both Links at `0x214a31c508`, value 16). The
  `PlayerStatus.Life` flag queue is only a save/status copy, not the live
  death trigger. The BSA action graph still
  identifies `InterruptDie` (`0x60a72e82`) and the action-request interface
  is unmapped, but it is not needed for the shared-pool game over.
- Cutscene full-screen takeover and Link 2 freeze/hide. **Done** (split is off
  during events).
- Verify both Links can use doors, pick up items and fight. **Blocked**: the
  real body is the cutscene `Dm_Player_Child_Op`; the full Player/CoopLink
  actor is refused by the create path. The mod-actor state 4 is now
  understood: a minimal ActorInfo row is dropped before creation. The full
  isolated row creates the child `CoopPuppet_Child` (after the create timeout
  was raised from 10 s to 60 s; it needs 19.9 s at ~2 fps), but the adult pack
  still returns state 4 because the name `CoopPuppet_Adult` is refused and the
  full player component set (PlayerRef/BSARef) is refused even under the
  working child name. The adult model/AS alone creates under the child name
  (draw-only, no physics). There is no adult cutscene player body. A player
  component bridge copies 20 player-only pointers into the real child body,
  but the empty-link holder clone still crashes because the holder is a
  shallow copy. The child's own game-created holder (`[actor+0x1b0]`, main
  actor/id `+0x40/+0x48`, sub-holder `+0xa0/+0xa8`) can be registered in
  `manager+0x40` directly, but the manager's player update still crashes on a
  missing player-specific state object; the actor holder alone is not enough.
  The create refusal was **not** PlayerRef: it was `EquipmentUserRef` (the
  player's equipment XLink user, `Weapon_Shield_Hylian`). Removing the
  equipment user lets a child `C` actor with `PlayerRef`+`BSARef` create
  (`C spawned after 1928 ms`, actor `0x214a575fc0`). The adult
  `CoopLink_Adult` with the same recipe still returns state 4, so the adult
  `Player_Adult` model/AS is the remaining adult blocker. `PouchPlayer_Adult`
  is a native adult model with a separate AS root (`Player_Pouch.root.asb`)
  and creates as a menu actor, but does not draw in the world. The split
  presenter and second chase camera are confirmed with the C body
  (`present: split true blit true view 1`). A clean child-only build named
  `C` with the canonical PlayerParam/blackboard paths and only PlayerRef added
  is refused in under a second (`word 0x60004`), so the actor name and path
  length are not the cause. Keeping the player affiliation and max-life
  (`DefaultAffiliation Player`, `MaxLife 12`) still returns state 4, so those
  row fields are not the cause either. The PlayerRef type vtable
  (`main+0x4b424c8`) has zero flag getters; the type is not unique. Bridging 42 component pointers into that body and
  registering its own holder still crashes in the player update; the player
  holder state itself is the remaining piece. The
  PlayerRef create check is not the actor name, the instance heap or a
  missing PlayerParam resource (all tested, still state 4); it is in the
  actor/component/player-manager registration. The type descriptor is at
  main+0x2d7c930 (path getter 0x2d7c9a0, type id 0xccd658d3, size 0x48) and
  the factory at main+0x9b5d30 has no visible singleton branch. A private
  pack-local PlayerComponent blackboard table (PlayerParam BlackboardTableRef
  copy) also returns state 4, so the shared table is not the cause either.
  The static PlayerRef type object at main+0x4974a48 is the blackboard
  parameter mapping (input-key hashes), not the registration check. The
  remaining check is in the actor/player-manager registration path.

### M4 — Enemy HP and polish

- Enemy HP multiplier, configurable, applied on spawn/respawn.
- Boss checks.
- Second-view half-resolution option.
- Performance tuning.
- Ten-minute stability run.

### M5 — Ready-to-run kit

- Windows x64 build.
- Portable folder with `lichii.exe`, `split.ini`, README and launcher.
- The user supplies their own `.nup`, keys and firmware.
- Smoke test on the user’s machine.

## 7. Testing plan

The game also runs on the development Mac (M1, about 1–2 fps) for smoke tests of the camera, split presenter, puppet and settings. Full-speed in-game validation is done on the user’s Windows PC.

Tools:

- Fritchii’s debug server (`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545`) and the existing MCP adapter for screenshots, logs and memory reads.
- The GDB stub for live memory writes during experiments.
- `split` debug logs prefixed with `split:` and an on-screen overlay.
- Unit tests for settings, viewport math, camera math, HP scaling and health sharing.

In-game checklist:

- [ ] Game boots with split disabled, unchanged.
- [ ] Link 2 spawns and is visible.
- [ ] Controller 2 moves Link 2 only.
- [ ] Controller 1 moves Link 1 only.
- [ ] Link 2 animates, collides, jumps and turns.
- [ ] Split-screen works horizontally.
- [ ] Split-screen works vertically.
- [ ] Shared inventory: item picked by Link 2 appears for Link 1.
- [ ] Shared health: damage to Link 2 reduces Link 1’s bar.
- [ ] Health zero triggers one game-over screen.
- [ ] Continue restarts both Links.
- [ ] Cutscene takes over the whole screen once.
- [ ] Enemy HP is 2× and configurable.
- [ ] Boss HP is 2×.
- [ ] No crash over a ten-minute run.

## 8. Build and CI

The build is Windows x64. The emulator uses MSVC, Qt 6.8.3, Ninja, CMake and `glslangValidator`.

Deliverables:

- `.github/workflows/windows-x64.yml` — GitHub Actions Windows runner that:
  - checks out Fritchii at `oot-gather-fix`,
  - applies the split patch series,
  - installs Qt 6.8.3 with `qtcharts`,
  - configures with `-DLICHII_SPLIT=ON`,
  - builds `lichii.exe`,
  - packages the portable kit.
- `build-windows.ps1` — same build locally on a Windows PC.
- `tools/release/make-kit.ps1` — assembles the portable zip.

The development machine here is macOS, so the actual Windows binary must be produced by a Windows runner. If the project is hosted on GitHub with Actions enabled, the workflow produces the artifact. Otherwise the user runs `build-windows.ps1` on their Windows PC. This is a hard dependency, not an optional step.

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| The game assumes exactly one player actor | Build `SplitLink` from the game’s player pack; fallback to actor cloning; stop at M1 if both fail |
| Player input is global and hard to redirect | Hook the player input component/blackboard; fallback code cave with a current-player flag |
| The scene cannot be replayed cleanly for a second camera | Start with scene-only split; keep HUD full-screen; disable post-processing for view 2 if needed |
| Culling/LOD computed for Link 1 only | Expand culling for the second view; accept minor pop-in in the first build |
| Post-processing seams | Apply post-processing to the combined image first; add per-view post-processing later |
| Second view is expensive | Half-resolution second view option; user has no hardware limit but weaker PCs can still play |
| Game over/Continue respawns only Link 1 | Hook the Continue path and respawn Link 2 next to Link 1 |
| Game updates change offsets | Pin game build `e0f3ccfaf50065ae` / 1.1.1; log a clear error on mismatch |
| Windows build infrastructure | GitHub Actions or local `build-windows.ps1`; do not promise a binary without a Windows runner |
| Save/load with Link 2 | Link 2 is transient; save only the original player; respawn Link 2 after load |

## 10. Repository layout

```
split-green-hat/
├─ PLAN.md                         ← this file
├─ README.md
├─ CMakeLists.txt
├─ config/split.ini                ← default settings shipped in the kit
├─ cmake/trim_paths.cmake
├─ split/
│  ├─ include/split/               ← settings, viewport, version
│  ├─ src/
│  └─ hook/                        ← code built into the emulator
├─ lichii/
│  ├─ PIN                          ← Fritchii tag and commit
│  └─ patches/                     ← small patch series applied to Fritchii
├─ scripts/
│  ├─ fetch-fritchii.ps1
│  └─ build-windows.ps1
├─ tests/                          ← settings and viewport unit tests
├─ docs/                           ← experiment logs and design notes
└─ .github/workflows/windows-x64.yml
```

The user’s `.nup` stays outside the repository and is never committed.

## 11. Definition of done

The project is done when a Windows x64 portable build:

- boots the user’s `.nup` with no game files bundled;
- shows two independent Links in one game instance;
- accepts two gamepads;
- supports horizontal and vertical split-screen;
- shares inventory, equipment, rupees, ammo, magic, bottles and health;
- shows one cutscene full-screen, owned by the first player who entered it;
- shows one game-over screen when either Link dies;
- restarts both Links on Continue;
- doubles enemy HP by default, configurable 1×–4×;
- runs a ten-minute session without a crash.

## 12. Credits and licensing

This project builds on:

- **Fritchii** by Frizti and the Lichii/Eglantine/Eden/yuzu/Sudachi contributors, GPL-3.0.
- **oot-remake-multiplayer** by daurosk and contributors, GPL-3.0, for the co-op research, memory readers, actor creation and camera tracking.
- The Ocarina of Time decompilation and reverse-engineering community for public format and engine knowledge.

All distributed source and binaries must comply with GPL-3.0. No game files, keys, firmware or saves are included.

- PouchLink_Adult (Pouch AS + PlayerRef/BSA/Life) still state 4; adult model+PlayerRef is the blocker.

- C Life bridged to Link 1 (0x0 -> 0x214aed78b8); PlayerRef state copy runs; holder still crashes.

- C shared Life alive test passed (12/8/4/0 on one Life object); adult PlayerRef without BSA still state 4.

- Private Player_Adult2 model path still state 4; adult player-slot limit is the working hypothesis.

- Adult model via C identity creates (LICHII_PUPPET_SOURCE=Adult); adult row rejected; Life bridged.

- Adult C body with real player physics creates; holder field match still crashes.

- Hit record on the C body does not damage (shared Life stays 16); player state required.

- Player bridge fills +0x158; fatal null is the vtable of the object at [sp+0x1e0] (main+0x32a70f0).

- Actor-state block copy removes the null vtable but deadlocks (guest cores idle); private player state needed.

- Actor-state copy deadlocks without the holder at the first BridgeChildLife; private state construction required.

- Manager links: two preallocated slots; active name Player_Adult; child holder under C still crashes at 0x0. Second link needs a per-player state object.

- Enemy HP validated on Keese and Enemy_Dekubaba (2 -> 4, 2.00x).
- Binder + shared state (`BIND=1` + `PLAYERBRIDGE=1`) also crashes; the state
  slot stays populated but the manager update fails. Every non-engine route to
  the second link is now exhausted: raw holder, game binder, spare state,
  shared state, and ActorParam additions all fail.
- Collision probe: with `OFFSET_Y=-1.0` + `HOLD_MS=2000`, the C body stays
  1.0 m inside the floor with no correction (`104.581` vs Link 1's `105.581`).
  Player collision is not active without the player state.
- Bisection of the remaining candidates: 37-component group, 35-component
  camera/audio group (`Item_Bombchu`) and `PlayerLifterRef` alone all return
  state 4. Every player-state component initializes the player's inventory/item
  links, so the missing `+0x158` state and the refusal share the save/inventory
  singleton root. Pack-level bisection is exhausted; a private player
  save/inventory is required.
- `SLinkRef` alone is refused (state 4) by the item-link singleton
  (`Item_Bomb`), like `EquipmentUserRef`/`XLinkRef`. The type-registry probe
  was inconclusive (mixed table, entry 43 = 0xffffffff).
- Crash stop registers show the C actor (0x214c938380) and the attached state
  (0x214bf31660) on the manager thread's stack, while the update call reads
  `components+0x158 == 0`. The table slot is transient/rebuilt; host injection
  does not survive. Next: minimal ActorParam addition to make the component
  system instantiate the state.
- Holder check: child holder `+0x40` is the C actor (verified), so the
  holder is correct. The crash still shows `components+0x158 == 0` while C's
  table holds the attached state; the manager must read a different table or
  the game clears the slot. Next: dump `x0`, `[x0+0x270]`, `[table+0x158]` at
  the crash with thread-selected registers.
- Crash LR `main+0xbbf04` shows the manager hits `components+0x158 == 0`
  for the actor it updates; C's table was populated, so the manager is updating
  a different actor. Link names C/Player_Adult/Dm_Npc_Base all crash; the
  holder's `+0x40` actor is the next suspect.
- The state class has 7 instances after load; only 2 are referenced by the 84
  actors. `LICHII_SPLIT_ATTACH_FREE=1` + `WAIT_STATE` + `BIND` attaches a spare
  state and the game binder binds the C holder, but the manager still crashes
  at the null lock (`0x99/0xa0/0xb0`). Sharing Link 1's state `+0x100` does not
  help: the state must be created/bound during the real player spawn.
- Split composite verified with both Links visible
  (`docs/img/split-both-links.png`); translation-only camera is the default.
  The game culls actors against the primary camera, so a Link 2 farther than
  the primary frustum (about 2-3 m at the Temple) is culled from view 1
  (`docs/img/split-moved-cull.png`). A second scene pass or cull override is
  needed for far-apart co-op.
- The synthetic chase rig can place the eye through a wall
  (`docs/img/split-rig-wall.png`); `LICHII_SPLIT_CAMERA_RIG=1` opts in.
- Composite HUD: hearts from view 0 at the left edge, items from view 1 at
  the right edge; the two edges update on alternating view frames.

- The game binds a player link with `main+0x2bc440(link, actor, flags=0)`
  (holder = actor+0x1b0, id = actor+0x18, link id must be -1). The caller is
  `main+0x211154` with `x0 = player_slot + 0xb38`. `LICHII_SPLIT_BIND=1`
  queues this call through the director's guest-call cave; the game then binds
  the C holder (`link0 holder 0x21472dd7c8 id 0x174`) itself. The manager
  still crashes because the actor-side `components+0x158` state is created by
  the player spawn path, not by link binding. The full component set
  (54/51/49) still returns state 4.

---

# Engine-level second-player implementation plan (funded)

Decision: build a real second player in the engine instead of shipping the
current limited co-op. This file is the working plan; milestones are ordered
and each has a concrete success test.

## Objective

One game instance, two adult Links, split-screen rendering, controller 2 drives
Link 2, independent damage and collision for Link 2, shared inventory and
health, and the game's own Game Over / Continue for either Link.

## Already verified (do not redo)

- Split rendering with both Links visible (`docs/img/split-both-links.png`),
  translation camera (synthetic rig is opt-in).
- The game's own Game Over / Continue with the shared Life pool.
- Adult `Player_Adult` model via the `C` identity with `PlayerRef` + `BSARef`
  and real `Player` physics.
- Shared Life pointer and the 12/8/4/0 drain with both actors on one object.
- Controller-2 puppet movement and animation.
- Enemy HP 2x on Keese and Dekubaba.
- The game's player-link binder: `main+0x2bc440(link, actor, flags)`, called by
  the player entry at `main+0x211154` with `x0 = player_slot + 0xb38`.
- The player update at `main+0xbbeb0` locks `[[actor+0x270]+0x158]+0x98`;
  a zero state is the crash (0x99/0xa0/0xb0 reads, then jump to 0).
- A spare/spoofed state at `+0x158` is not enough: attach + bind still crashes.
- Adding player-state ActorParam components makes creation return state 4;
  the word-7 object is a scene actor, not the cause.

## Core problem

The game is single-player. Link 1 is created by the player-spawn path, which:

1. creates the player actor with the full player component set;
2. builds the player-component registry at `actor+0x270` (PlayerRef at `+0x2b0`,
   Life at `+0x2b8`, player state at `+0x158`, ...);
3. binds the player link at `player_slot+0xb38` with `main+0x2bc440`.

The state class behind `+0x158` (vtable `main+0x4b47608`) owns a lock at `+0x98`
and a per-player object at `+0x100`, and is tied to the player save/inventory
singleton. Spare instances and injected pointers do not survive to the update.
A second player needs its own state and save/inventory binding, created the way
the game creates Link 1.

## Architecture

### Phase A - Instrumentation and state identification

- A1. Breakpoint `main+0xbbeb0` in the crash configuration; for each call log
  `x0`, `[x0+0x270]`, `[[x0+0x270]+0x158]`. Identify which actor has state 0
  and which link entry feeds it.
- A2. Identify the `+0x158` class exactly: constructor `main+0x2dc6f90`, type
  object, and the ActorParam component whose init creates it (use the
  one-component bisection with creation allowed only when possible).
- A3. Find the player-component registry builder: the routine that writes table
  slots at actor creation, and confirm why `+0x158` is absent for the C actor.
- A4. Find the per-player save/inventory object: the data behind the state's
  lock/`+0x100` init and the item XLinks; determine whether the game supports
  multiple instances (save slots).

Success: a logged call where `[actor+0x270]+0x158` is the C state, and the
creator/builder functions are identified.

### Phase B - Private player state

- B1. Allocate a private state through the game allocator (guest call via the
  coop cave), size from the class allocator call.
- B2. Construct with `main+0x2dc6f90` and the game's init; initialize the lock
  with the SDK mutex init (or let the class init do it).
- B3. Bind owner `+0x18` to the C actor and create/bind the per-player object
  at `+0x100`.
- B4. Register it through the component system (the registry builder from A3),
  not a raw table write.

Success: the C state survives the update and the manager processes the C link
without crash or deadlock.

### Phase C - Private save / inventory

- C1. Clone the player save/inventory object, or use an unused save slot if the
  game allocates more than one.
- C2. Point the C player components at the clone so item/equipment XLinks
  resolve there; if the game refuses duplicates, add a share/existing-user path
  for the second player's item links.
- C3. Keep shared health by having both states reference Link 1's Life object
  (already proven).

Success: player-state components instantiate for C without the state-4 refusal.

### Phase D - Manager integration

- D1. Bind the C holder with `main+0x2bc440(link, actor, 0)`.
- D2. Verify `main+0xbbeb0` runs for C with the private state; fix any remaining
  missing fields iteratively.
- D3. Ensure the manager's two link slots are independent and both survive
  scene changes and respawns.

Success: both Links are processed by the player update for several minutes,
including a scene change.

### Phase E - Gameplay verification

- E1. Independent damage: a hit record on Link 2 lowers the shared Life and
  shows the damage overlay; either Link can die.
- E2. Collision: the C body resolves floor/wall penetration and the character
  controller moves it; the floor-penetration probe passes.
- E3. Death flow: either Link at 0 health triggers the game's Game Over;
  Continue restores both.

Success: recorded runs and screenshots for E1-E3.

### Phase F - Polish and packaging

- F1. HUD edge synchronization between the two view halves.
- F2. Second-view culling: second scene pass or a cull override so a distant
  Link 2 is rendered.
- F3. Windows x64 kit validated on a Windows build host.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| State class is a hard singleton | Private clone with reinitialized locks, or patch the singleton accessor. |
| Save/inventory is globally keyed | Clone it, use an unused save slot, or share Link 1's item links with a duplicate-tolerant registration patch. |
| Registry rebuild clears slots | Register through the game's builder (A3), never a raw table write. |
| Manager needs more per-link fields | Iterate with the A1 breakpoint logging until both links update cleanly. |
| Performance of a second scene pass | Defer to F2; measure on the persistent build. |

## Immediate next step

A1: with the GDB stub in the crash configuration (`BIND` + `PLAYERBRIDGE` or
`BIND` + spare state), break at `main+0xbbeb0`, select the stopped thread
(`H<thread>`), and log `x0`, its table and `+0x158` for every call. This names
the actor that reaches the update with a zero state and tells us whether the
link entry, the table pointer or the registration is wrong.

## Working commands and files

- Build: `PATH=~/.local/share/fritchii-build-shims:$PATH ninja yuzu-cmd -j 8`
  in `split-green-hat/fritchii-build`.
- Hook: `split/hook/split_hook.cpp` (env-gated experiments).
- Pack builder: `tools/splitlink_pack/build.py`.
- Working C body: 32 components, `LICHII_PUPPET_SOURCE=Adult`,
  `LICHII_COOPLINK_PHYSICS=Player`, `LICHII_COOPLINK_DROP=EquipmentUserRef`.
- GDB stub patterns and the thread-selection lesson: `docs/macos-dev.md`,
  `docs/m1b-splitlink.md`.

---

## A1 progress (engine-level plan)

The player-update breakpoint is working at the exact lock call:

- Breakpoint: `main+0xbbf00` (the `bl` whose return is `main+0xbbf04`,
  the crash LR).
- At the stop, `x19` is the update object and `x20` is the state
  (`[[x19+0x270]+0x158]`); `x20 == 0` is the crash.
- Thread selection must use `Hg<thread>` before `g`; `H<thread>` leaves the
  active thread wrong.
- GDB breakpoints must be stepped over (`z0`, `s`, `Z0`) or the guest loops at
  the breakpoint.

Observed Link 1 updates:

```text
HIT actor 0x2149e288d0 state 0x214a70e8a8 table 0x214a70d840 tbl158 0x214a70e8a8
```

(actor, state and table+0x158 agree). The zero-state C update was not captured
in the same run because the breakpoint slowed the load and the puppet did not
spawn before the wait expired. Next A1 attempt: arm at "create C requested"
with the spawn wait extended, or set the breakpoint only after the C spawn but
before the 1 s bind tick, and log `x19/x20/table/tbl158` for the first update
whose `x19` is the C actor.

### A1 complete: zero-state read captured

With the read watchpoint on the C actor's component slot
(`Z3` on `table+0x158`) and an 8 s bind hold (`LICHII_SPLIT_BIND_HOLD_MS`):

```text
child 0x214abe6d80  table 0x214d4ac910  state158 0x0
rwatch 0x214d4aca68
STOP pc 0x83866c7c (main+0x2c1dc7c) lr 0x83866774
     x19=0x214abe6d80 (C actor)  x8=0  x20=0
```

The reader is `main+0x2c1dc7c`, inside the large function at
`main+0x2c1d740`:

```text
2c1dc7c: ldp x10, x11, [x9, #0x158]
2c1dc80: str x10, [sp, #0x110]
2c1dc88: str x11, [sp, #0x38]
```

It snapshots the C actor's component-table slots (`+0x2b0`, `+0x2b8`,
`+0x158`, `+0x160`, ...). `x19` is the C actor, so this is the C actor's table
read, and `+0x158` is the state slot that must be populated through the
component system.

A1 result: the attachment point is the **C actor's table slot +0x158**; the
zero-state read happens before the bind in the component-snapshot path. Phase B
must create/register the state for this actor/table (not the player slot), so
the snapshot and the later `main+0xbbeb0` lock both see a valid state.

### Phase B first attempt: private state clone

`LICHII_SPLIT_PRIVATE_STATE=1` now:

1. clones Link 1's real state (`0x1a0` bytes) into the puppet block's spare
   area (`block+0x700`);
2. zeroes the lock region at `+0x98` and queues the SDK lock initializer
   (`main+0x3c42490`) on `+0x98`;
3. sets the state owner `+0x18` to the C actor;
4. attaches the clone to the C actor's table `+0x158`;
5. binds with the game's `main+0x2bc440`.

Observed run:

```text
private state clone 0x2106e29700 <- Link1 state 0x214a750358, lock init queued
private state 0x2106e29700 attached to child 0x214cc067a8 table 0x214cc37508
childholder bind queued link 0x2160bea130 child 0x214cc067a8
state now 0x2106e29700
```

The manager still crashes with the same null-lock reads and `0x0` jump:

```text
Unmapped read 0x99 / 0xa0 / 0xb0
Cannot execute instruction at unmapped address 0x00000000000000
```

So the clone occupies the C actor's `+0x158`, but the crashing update still
sees a zero state. Either the crash object is not the C actor's table, or the
queued lock initializer did not run. Next: capture the crash update object at
`main+0xbbf00` with reliable step-over (or read the clone's `+0x98` lock after
the run to verify initialization), then attach the private state to the actual
crash object's table.

### Phase B: the real null is actor_table+0x358, and the lock init gap

A1's `+0x158` zero-state read is only half of the manager link. The player
update at `main+0x32a5500` does:

```text
ldr x0, [sp,#0x200]      ; = [actor_table+0x358]
...
bl  0xbbc50              ; player update, which calls main+0xbbeb0
```

The C body has no component at `actor_table+0x358`, so `x0` is null and the
update reaches the `main+0xbbeb0` state lock with `x20 = 0`. The lock call
(`main+0xbbf00`) then faults on `0x99/0xb0` and jumps to `0x0`.

New env-gated implementation in `split/hook/split_hook.cpp`:

- `LICHII_SPLIT_SHARE_358=1` copies the active player's
  `[table+0x358]` into the child's `[table+0x358]` before the bind.
- The private `+0x158` clone is now made in the same early sync at C spawn
  (`SyncChildTable358`), not on the later `DebugCopyActiveLink` 1 s tick.
- The late clone is skipped when `SHARE_358` is on, so it cannot zero the
  early clone's lock.

Observed with `LICHII_SPLIT_PRIVATE_STATE=1 LICHII_SPLIT_SHARE_358=1
LICHII_SPLIT_BIND_HOLD_MS=8000`:

```text
P0 C spawned ... actor 0x214cb1a3c8
early private state 0x2106dc1700 <- 0x214a4ead78 for child 0x214cb1a3c8
child 0x214cb1a3c8 table+0x358 <- 0x214a4f3498 (shared component)
bind queued link 0x2160bea130 child 0x214cb1a3c8 (8 s hold)
(no Unmapped read 0x99/0xb0, no Cannot execute)
fps 0
clone lock at 0x2106dc1700+0x98: all zero
source lock at 0x214a4ead78+0x98: 0101... (initialized)
```

So the null-pointer crash is gone, but the guest hangs in the zeroed lock.
The queued SDK initializer (`main+0x3c42490`) did not run even with an 8 s
bind hold. Next: execute that initializer synchronously before the bind (a
direct guest call, or copy a known-unheld initialized lock template), then
retest the manager link, damage and collision.

### Phase B: the shallow clone's null virtual dispatch

The early clone + shared `+0x358` gets past the null component and the lock,
but the first manager update after the bind then stops at:

```text
PC 0x20   LR main+0x6028fc   (main base 0x80352000)
x0 0   x8 0   x9 0   x19 0x214c9e1608 (C actor)
x22 0x214a6d7190 (shared actor_table+0x358)
```

The call site is:

```text
6028e0: mov x19, x0
6028e4: mov x0, x8
...
6028f8: blr x9
6028fc: mov x16, x23
```

`x8`/`x9` are zero: the clone's copied function objects (the state's
`+0x110/+0x138/+0x150/+0x198` callback slots and their captured pointers)
still refer to Link 1's data, and the first dispatch after the bind goes
through a null target (the reported PC advances `0x0, 0x4, 0x8, ...` on
successive attempts). Copying the source lock bytes is not enough; the clone
is a shallow copy and its callback/sub-object graph is not valid for the C
link.

So the clone approach cannot complete Phase B. The next design is to create a
real state through the game's state constructor/spawn path (the vtable
initializer at `main+0xc1d568`/`main+0x2dc6f90`), then set the owner and
attach it; the constructor initializes the callback slots and the lock for a
new instance. The queued-call path must be verified to actually execute (the
`main+0x3c42490` lock init still showed zero bytes after an 8 s hold).

### Phase B: real-constructor experiment (implemented, unverified)

`LICHII_SPLIT_REAL_STATE=1` was added at the bind tick:

1. zero `block+0x700`;
2. queue the game's state constructor `main+0x2dc6f90` on it;
3. on the next tick, restore the base vtable `main+0x4b47608`, set the owner
   to the child, and keep the child's `+0x158` pointing at it;
4. the bind is held (`LICHII_SPLIT_BIND_HOLD_MS`) so the constructor can run
   before the manager update.

The first run with this env hung during boot at 2.03 s in a Vulkan pipeline
loop, before the `M1 game build` log, so the constructor path is implemented
but not yet verified. Next: retry with a clean/warm shader cache and verify
the scratch vtable changes (zero -> constructor vtable -> base vtable) before
the bind, then check fps and the manager link.

### Phase B: real constructor verified; player-specific fields still null

With `LICHII_SPLIT_REAL_STATE=1` the constructor ran and the vtable
transition was observed:

```text
real state ctor queued at 0x21073dc700 for child 0x214a9ee540
real state ctor vtable 0x85744518 at 0x21073dc700
real state 0x21073dc700 base vtable attached to child 0x214a9ee540
bind queued link 0x21611ea130 child 0x214a9ee540 (4 s hold)
Cannot execute instruction at unmapped address 0x0
```

`0x85744518 - 0x80ae8000 = 0x4c5c518`, the constructor's derived vtable; the
hook then restores the base vtable `main+0x4b47608` and attaches the state.
So the constructor path itself works (zero -> constructor vtable -> base
vtable). The manager link still crashes 0.7 s after the bind at the same
null dispatch: the constructor initializes the callback slots and lock, but
not the player-specific sub-objects at `+0x8/+0x10/+0x20` (the real state
instances hold tagged pointers into the `0x118e...` player-manager region).
The zeroed constructor state has those fields zero, and the first update
dispatches through a null target.

Next: populate the C state's player-specific sub-objects from the C link /
holder (or create private copies of the `0x118e...` objects) before the
bind, then retest for live fps and no null dispatch.

### Phase B: constructor works; shared +0x358 dispatcher is the remaining null

Verified in a clean run:

```text
real state ctor queued at 0x21079b3700 for child 0x214d6f0b60
real state ctor vtable 0x85c7b518 at 0x21079b3700
real state player fields copied from 0x214b353d78
real state 0x21079b3700 base vtable attached to child 0x214d6f0b60
bind queued ... (4 s hold)
Cannot execute instruction at unmapped address 0x0
```

`0x85c7b518 - main(0x80ae8000) = 0x4c5c518`; the hook then restores the base
vtable `main+0x4b47608`. So the real constructor path is verified end to end.

Copying Link 1's `+0x8/+0x10/+0x20/+0x28` player fields into the constructed
state did not remove the post-bind null dispatch. Retargeting the shared
`actor_table+0x358` component's owner from Link 1 to C made Link 1 crash
before the bind:

```text
retargeted shared +0x358 owner to child 0x214a692cc8
Cannot execute instruction at unmapped address 0x0   (before bind queued)
```

So sharing the `+0x358` component is not viable. The remaining null is inside
the `+0x358` component's dispatcher (`main+0x6028f8: blr x9`, `x0=x8=0`); the
next step is to identify which element of that component is null and either
populate it or construct a private `+0x358` component for the C actor.

### Phase B: +0x358 is a script/event context, not a simple component

Post-bind stop with the constructed state and copied player fields:

```text
PC 0x4   LR main+0x6028fc
x0 0   x8 0   x9 0
x14 0x214ac09508   (shared actor_table+0x358 component)
x15 0   x19 0x214d119830 (C actor)   x22 0x214ac09508
```

The shared component is a script/event context:

```text
+0x00 vtable main+0x4b4dfc0
+0x08 0x118ec04fed
+0x10 0x118e569300
+0x18 0x214a64b0e0   (Link 1 owner)
+0x20 0
+0x28 .. +0x120      tagged script-node pointers (0x214ac10a..)
+0x180/+0x190/+0x1c0 further context pointers
```

The dispatcher's `x15` iterator is null; it walks the script-node list of
this context. Sharing Link 1's context gives the C link Link 1's nodes, and
the C-specific iterator is null. Retargeting the context owner to C breaks
Link 1 because the same context is used by Link 1's update.

So the clone/constructor route cannot build the script-node graph. Phase B
needs the game's player-spawn/initialization path for the C link (or the
Player component/system instantiated on the C actor), which creates the
`+0x358` context and its nodes for C. That is the next architectural step;
further cloning `+0x158` alone will not make the manager link work.

### Phase B: private +0x358 context experiments

Two attempts were made to give C its own script/event context:

1. **Constructor** `main+0x11a69b0` on `block+0x8a0` (puppet block enlarged
   to `0x2000`; the constructor writes a `0x14b0`-byte object and vtable
   `main+0x4b4dfc0`). It ran:
   ```text
   private context ctor queued at 0x21075e18a0
   private context 0x21075e18a0 vtable 0x858d2fc0 attached to child
   ```
   The empty context crashed **before the bind** (`0x0`), because it has no
   script nodes.

2. **Relocatable copy** of Link 1's `0x14b0` context with self-references
   relocated by the block delta:
   ```text
   private context copy 0x21069c28a0 <- 0x214a52a308 attached to child
   Cannot execute ... 0x0   (before the bind)
   ```
   The context's node pointers (`0x214ac10a..`) are external objects, not
   embedded in the `0x14b0` block; copying the context leaves them shared
   with Link 1 and the C update still hits a null target.

Sharing the context survives to the bind but then hits the null C iterator;
retargeting its owner breaks Link 1. So neither copy nor constructor is
viable. The `+0x358` script graph has to be created by the game's player
spawn/initialization path for the C link.

### Phase B: the +0x358 context belongs to the singleton Player component

The context factory is `main+0x11a6950`:

```text
mov w0, #0x1c78        ; allocation size 0x1c78, not 0x14b0
mov x1, <manager>
bl  0x3ebe0            ; allocate
bl  0x11a69b0          ; construct context
```

The constructor's `0x14b0` is only part of the object; the script nodes are
separate allocations (`0x214ac10a..`), created after construction. The C
actor's `PlayerRef` resolves to Link 1's singleton `Player` component, so the
C link shares Link 1's script context and nodes. That is why the private
context copy/constructor cannot work: the nodes belong to the singleton, and
the full player component set is refused by the actor system (state 4).

A real second player therefore needs a **second Player component instance**
(duplicate the singleton state/context/node system), not another state object
attached to the actor table. This is a larger engine change than the current
hook-level clone: the game's Player component is a singleton and the actor
spawn path refuses a second full player.

### Phase B: only the active player owns a +0x358 context

Runtime search for the player-state vtable found 6 instances in the scene:

```text
Link 1 owner 0x214a205848: table158 self, table358 0x214ab08610
owner 0x214ba805e8:        table158 self, table358 0
owner 0x214baed078:        table158 self, table358 0
owner 0x214ba80d90:        table158 self, table358 0
owner 0x214ba81538:        table158 self, table358 0
owner 0x214cc5b560:        table158 self, table358 0
```

So the Dm_Player NPCs have player states, but **only the active player has
the +0x358 script/event context**. Binding the C link to one of those NPC
states would still leave `table+0x358 = 0`. The context factory
`main+0x11a6950` has no direct BL/ADRP reference in the main module; the call
is cross-module or through a relocated dispatch table, matching a singleton
player-activation path.

A second +0x358 context therefore requires activating a second Player
component instance through that path (or duplicating the singleton), not
binding to an existing NPC state.

---

# Decision: duplicate the singleton Player component/system (Phase B)

Date: 2026-11-13. The user chose option 1: duplicate the game's singleton
Player component/system for the C link instead of sharing Link 1's state,
context, or event nodes.

The `+0x358` object is the player's script/event context; the builder drops
`EventPerformerRef` because the event system is single-player. The singleton
duplication therefore covers all three layers:

1. the private `+0x158` player-state instance (already constructed by
   `main+0x2dc6f90` and re-vtabled to `main+0x4b47608`);
2. a private `+0x358` script/event context (constructor `main+0x11a69b0`,
   allocation size `0x1c78`, vtable `main+0x4b4dfc0`);
3. the external script-node objects (`0x214ac10a..` in the active player)
   and any per-player global pointers the event system reads.

Milestones:

- **B1 (map)**: identify the singleton player/event manager globals, the
  context's external node allocations and their sizes, and the per-player
  pointers inside the context.
- **B2 (duplicate)**: allocate the private state, context and node graph
  (puppet block is 0x2000; a new guest allocation if needed), relocate
  self-references by block delta, and patch the per-player globals/owner to
  the C link.
- **B3 (verify)**: bind the C link, require live fps, no null dispatch, no
  `Cannot execute`, and no Link 1 regression; then start Phase C private
  save/inventory and Phase E damage/collision.

Do not bind C to Link 1's context, do not retarget Link 1's context owner,
and do not reuse Link 1's event nodes.

### B1: EventPerformerRef test (event-system singleton)

To test whether the `+0x358` context is the event performer, the working
32-component C body was merged with `EventPerformerRef` and its files
(33 components, 37 added files, uncompressed 301632, size-table entry
305728). Spawn result:

```text
P0: create C requested (returned 0x1), handle slot ...
P0: create C requested (returned 0x1), handle slot ...
Unmapped Write64 @ 0x0 = 0x000011e2f4a6b8
Unmapped Write64 @ 0x0 = 0x000011e2f4a70f
...
```

The second actor reaches a **null event-system global**: the game writes
event/script function pointers to address 0 and the emulator aborts. The full
54-component player set is still refused (state 4); the 33-component set is
requested but never spawns.

A follow-up test with `ActorNameRef = Dm_Player_Child_Op` (instead of
`Dm_Npc_Base`) on the 33-component body could not boot: the emulator hung at
1.77 s in the Vulkan pipeline loop, so that test is inconclusive and needs a
clean boot.

The C pack and size table were restored to the working 32-component body.
Next B1 step: capture the null-write PC or retry the `Dm_Player_Child_Op`
name on a clean boot; the event-system global/registry must be instantiated
per player before the event performer can register.

### B1: clean-boot Dm_Player test; the null component is vtable main+0x4bf9010

The emulator boot hang was the shader pipeline cache. Moving
`~/.cache/eden/shader` aside gave a clean boot (51 fps).

With the 33-component C body (`32 + EventPerformerRef`) and
`ActorNameRef = Dm_Player_Child_Op`, the create is requested and the
player-init path then runs a component-state constructor with `this = 0`:

```text
Unmapped Write64 @ 0x0   = 0x85a31010
Unmapped Write64 @ 0x8   = 0
...
Unmapped Write64 @ 0x148 = 0x84d1cf8c
Unmapped Write64 @ 0x158 = 0
Unmapped Write64 @ 0x160 = 0
```

Main base was `0x80e38000`, so `0x85a31010 - 0x80e38000 = 0x4bf9010`: this is
neither the player-state vtable (`0x4b47608`) nor the context vtable
(`0x4b4dfc0`). It is a separate component state the player-init path tries to
construct at a null pointer.

The C ActorInfo row had `InstanceHeapSize = 0`; `Player_Adult` has
`10245064`. Setting C's row to `10245064` and retesting produced the **same
null writes**. So the missing allocation is not the instance heap; it is a
singleton component instance (the event performer) that the second player
cannot allocate.

Restored: the working 32-component C pack, the original ActorInfo/size table,
and the rebuilt shader cache. Next: locate the component state slot for
vtable `main+0x4bf9010` in Link 1's table and construct/attach a private
instance in the puppet block, then retest the 33-component body.

### B1: the 0x4bf9010 class is the event/performer object; allocator is TLS+0x88

Link 1's actor table holds three instances of the `0x4bf9010` class at
slots `+0x510`, `+0x560`, `+0x770` (addresses in the `0x11ab...` script
heap). The factory is `main+0x669800`:

```text
66983c: ldr x8,[x25]        ; TLS key
669840: ldr w0,[x8,#0x48]
669844: bl  0x3c41cf0       ; get thread-local block
...
6698f0: ldr x8,[x0]
6698f4: mov w1,#0x168       ; object size
6698f8: mov w2,#0x8
6698fc: ldr x8,[x8,#0x30]   ; allocator vtable
669900: blr x8
669904: ... str vtable 0x4bf9010 at [x0]
```

The allocator object is the thread-local `+0x88` block. For the C create the
`allocate(0x168)` call returns 0, so the factory writes the object at
`this=0` and the emulator logs `Unmapped Write64 @ 0x0..0x160`.

So the missing piece is not the actor instance heap but the **script/event
allocator for the C thread**. Next: capture the TLS/allocator at
`main+0x669900` during a C create and assign a valid script heap (Link 1's or
a private one) to TLS `+0x88` before the factory allocates.

### B1: the event arena is 0x20e0 with only 0x110 free

Captured at `main+0x669900` during the C create:

```text
x0 = 0            (allocation result)
x1 = 0x168        (requested size)
x2 = 8            (alignment)
x20/x3 = 0x11e8fdc000
```

The allocator object `0x11e8fdc000`:

```text
+0x00 vtable main+0x4bf7400
+0x30 0x20e0          (arena size)
+0xb0 0x11e8fddfd0    (current)
+0xb8 0x11e8fde0e0    (end)
free = 0x110, request = 0x168 -> returns 0
```

The EventPerformer param has only `EventUniqueAin`,
`ExceptionalBindType`, `UseEventUniqueAin`; it has no heap-size field. The
actor's `InstanceHeapSize` is therefore unrelated; this `0x20e0` arena is the
per-player event context arena (`0x1c78` context + overhead) and the
`0x168` event-performer object does not fit.

A live GDB patch (extend `+0xb8`, re-run the allocator at `0x669900`) failed
at the packet layer: `M`/`G` returned empty replies and `z0` returned `E`, so
the injection path needs a host-side hook or a proper guest code patch rather
than the stub.

Next: enlarge this event arena or redirect its bump pointers to a private
block before the `0x168` allocation. The actor instance heap and
EventPerformer param are not the control points.

### B1: 0x20-byte actor-state bridge finished; still no live manager link

Ran with `LICHII_SPLIT_ACTORSTATE=1 LICHII_SPLIT_ACTORSTATE_BYTES=0x20`:

```text
P0 C spawned ... actor 0x214cb250c8
actor state bridge child 0x214cb250c8 <- Link1 0x214a04b878
  (0x20 bytes, vtable 0x0)
childholder bind queued link 0x2160bea130 child 0x214cb250c8
MemoryRead: Unmapped read 0x99 (1 bytes)
MemoryRead: Unmapped read 0xb0 (4 bytes)
ExceptionRaised: Cannot execute instruction at unmapped address 0x0
fps 0
```

So the 0x20-byte actor-state bridge does not populate the `+0x158` state/`+0x358` context path; the manager update still reaches the null lock.

Arena control points checked:
- `PlayerParam` contains only `BlackboardTableRef`; no heap/arena size.
- The two `mov wN,#0x20e0` sites (`main+0x7055b0`, `main+0xf0f4f0`) are pointer arithmetic into a large object, not allocation sizes.
- The actor `InstanceHeapSize` change did not alter the `0x20e0` event arena.

The `0x20e0` arena size is computed/embedded elsewhere; the 0x20 bridge bisect is exhausted. Next: a host-side hook or guest patch to enlarge/redirect the event arena before the `0x168` event object allocation.

### B1: PLAYERBRIDGE/PLAYERSTATE bisect fails before the bridge

`/tmp/macos-statebisect1.log` did not exist; it was created by running both
envs (`LICHII_SPLIT_PLAYERSTATE=1 LICHII_SPLIT_PLAYERBRIDGE=1`) with the C
actor and bind.

The bridge itself ran:

```text
player state bridge child 0x214cb9d1f8 PlayerRef 0x214cc25268
  <- Link1 PlayerRef 0x214a6bafb0
player bridge child 0x214cb9d1f8 copied 42 components mask 0x214a886e216f7d7f
childholder bind queued link 0x2160bea130 child 0x214cb9d1f8
```

But the fatal event-arena writes happened **earlier**, during the create:

```text
72.944  Unmapped Write64 @ 0x0 = 0x000011e21daa8b
        Unmapped Write64 @ 0x0 = 0x000011e21da4d5
        ...
74.652  P0 C spawned after 1738 ms
74.823  player state bridge / player bridge / bind queued
fps 0
```

The bridge copies state after the C actor spawns and cannot repair the event
allocator failure that occurs during the create. The 0x20/PLAYERSTATE/
PLAYERBRIDGE bisect is exhausted.

Next: the host-side event-arena redirect at `main+0x669900` is the only
remaining Phase B path; the arena size (`0x20e0`) is computed and is not
controlled by PlayerParam, the EventPerformer param, or the actor
InstanceHeapSize.

### B2: event-arena end check bypass works; create now null-read loops

Added `LICHII_SPLIT_ARENA_OVERFLOW=1`: at startup the hook rewrites the
allocator's end check at `main+0x3472288` from `b.hs 0x3472330` to an
unconditional `b 0x3472330` (0x1400002a) and invalidates the instruction
cache. Log:

```text
split: arena end check bypassed at 0x842bd288
P0: create C requested (returned 0x1), handle slot ...
```

Result with the 33-component event body:

- the old `Unmapped Write64 @ 0x0` event-object writes are **gone**; the
  `0x168` object now fits in the page slack past the `0x20e0` logical end;
- the C actor still does not spawn/refuse within the timeout;
- the log fills with `Unmapped Read32 @ 0x0` (counter > 1.7e9) and fps stays
  ~1.9, so the create is spinning on a null event-system global/context.

So the arena bottleneck is removed; the remaining create-time blocker is the
null event context/global, not the allocator end. Restored the working
32-component C pack/size table after the test.

Next: capture the `Unmapped Read32 @ 0x0` PC (the `Core.ARM MemoryRead:
... at pc` lines) and initialize/duplicate the null event global before the
C actor create.

### B2: arena+blackboard fallback removes null loops; create times out

Enabled `LICHII_SPLIT_ARENA_OVERFLOW=1` and
`LICHII_SPLIT_BLACKBOARD_SHARE=1` with the 33-component event body.

The save loads normally; after the player is found the fallback is patched:

```text
arena end check bypassed at 0x843ed288
blackboard registry fallback patched at 0x83dead14
M1b player actor 0x214aa28ec8
P0: create C requested (returned 0x1), handle slot 0x21077a7110
```

There are **no** `Unmapped Write64` or `Unmapped Read32 @ 0x0` loops now:
the registry lookup and the `0x168` event allocation are past. But the
create never returns an actor; the director still reports `P0 creating` and
retries after the timeout:

```text
P0: create C requested (returned 0x1), handle slot 0x21077a7118
... handle holds ...: 0x2 0 0x101 0 0 0 0 0
fps 0
```

So the arena and registry nulls are fixed, but actor management still blocks
the C create (request 0x1, no actor, timeout/retry). Restored the working
32-component C pack/size table.

Next: inspect the create request/handle state at the timeout (`0x2 ... 0x101`)
and the actor-management wait; the request is accepted but no actor is
produced.

### B2: create request stays in state 2; EventPerformerRef is the trigger

The director source documents the exact failure: the mid-play create argument
uses the `SeqExecuteCreateActor` form; the preload walker form
(`+0x48 = 0x10001, +0x98 = 0`) is only taken while a scene loads, and a
request made later "stays in state 2".

Observed with the 33-component `Dm_Player_Child_Op` body plus the arena and
blackboard fallbacks:

```text
P0: create C requested (returned 0x1), handle slot ...
P0 creating ... handle holds ...: 0x2 0 0x101 0 0 0 0 0
(no actor; retries at the create timeout)
```

The 32-component body (no `EventPerformerRef`) spawns normally, so the
difference is the event performer/player event root (`Player.event.root.ain`)
waiting in the actor manager. The arena and blackboard nulls are fixed, but
the event system still does not complete the create.

The `Dm_Npc_Base` + `EventPerformerRef` variant did not reach the player in
the last run (fps 0), so it is inconclusive and needs a clean retry.

Restored the working 32-component C pack/size table.
Next: inspect the actor manager's state-2 wait for the event performer/event
root, or retry the Dm_Npc_Base variant on a clean boot.

### B2: Dm_Npc_Base event-body boot hung; preload create is the next path

The `Dm_Npc_Base` + `EventPerformerRef` 33-component body with the arena and
blackboard fallbacks did not reach the player: boot logged 59 fps, the
service log stopped around 19 s, and the status fell to 0. It is
inconclusive and needs a clean retry.

The Dm_Player 33-component body reliably reaches the create and stays in
actor-manager state 2 (handle `0x2 ... 0x101`) until the timeout. The
32-component body spawns, so the event performer/event root is the wait.

Next implementation path: use the actor manager's **preload form**
(`create arg +0x48 = 0x10001, +0x98 = 0`) while the scene is still loading,
bypassing the `Drawn`/settle wait, so the C actor is initialized with the
scene's event system instead of being a mid-play request that stays state 2.
If that still hits the player event root, the singleton event root/context
must be duplicated.

### B2: preload create path tested; target arrives after the load window

Added `LICHII_COOP_PRELOAD=1`: in `Phase::idle`, when `Drawn()` is false the
director bypasses the wait and calls `Create(..., preload=true)`, which uses
the preload form (`+0x48 = 0x10001, +0x98 = 0`) and skips the
`kGotCreateDefault` requirement.

Run with the 33-component Dm_Player body, arena bypass and blackboard share:

```text
M1b player actor 0x2149e05ad8                     (71.03)
P0: create C requested ...                         (75.48)
P0 creating ... handle holds 0x2147312ed8:
  0x2 0 0x101 0 0 0 0x2 0
(no spawn; fps 0)
```

The preload create still happens after the player/scene load; the coop target
only exists after the player actor is read, so the preload form misses the
scene-load window and the request remains in state 2.

Next: trigger the C create at the **scene transition before the player
target exists** (using the actor name and a default position), or duplicate
the player event root/context so the mid-play request can complete.

### B2: event-root duplication design (next Phase B step)

The preload form still leaves the create in actor-manager state 2 because the
event performer references the single-player event root
`Work/AI/Root/Event/Player.event.root.ain`; the player event system is
already owned by Link 1.

The next concrete duplication is therefore at the **event root resource**:

1. copy `Work/AI/Root/Event/Player.event.root.ain` from the game romfs into
   the mod romfs as `Work/AI/Root/Event/PlayerC.event.root.ain`;
2. copy `Component/EventPerformerParam/Player.engine__component__EventPerformerParam.bgyml`
   and change its `EventUniqueAin` to the `PlayerC` AIN path;
3. point the C actor's `EventPerformerRef` at the private param path;
4. add ResourceSizeTable entries for both new files and install them in the
   mod romfs;
5. retest the 33-component Dm_Player body.

This makes the event performer instantiate a distinct AI/event root instead
of waiting on Link 1's singleton. Timing-only workarounds (preload form,
arena, blackboard) are exhausted.

### B2: private event root allocates the C actor; request/actor stay 2/3

Built and installed:

- `AI/PlayerC.event.root.ainb` (257176 bytes) copied from
  `AI/Player.event.root.ainb` inside `Pack/AI.Global.Product.111.pack.zs`;
- a private `EventPerformerParam` with
  `EventUniqueAin = Work/AI/Root/Event/PlayerC.event.root.ain`;
- the C ActorParam's `EventPerformerRef` pointed at the private param;
- AI pack (750 members), C pack, and ResourceSizeTable updated.

Run with the 33-component Dm_Player body:

```text
M1b player actor 0x2149e4b230                      (65.56)
P0: create C requested (returned 0x1)              (71.41)
request 0x2147312ed8:
  0x2 0 0x101 0 0 0 0x3 0x214c9aa238
actor 0x214c9aa238: state +0x2c = 3 (not live 4)
table+0x158/+0x358 not initialized/readable
fps ~2
```

The private event root advances past the null/event-root wait and allocates
the C actor, but the actor remains in state 3 and the request's first word
stays 2 (`kRequestReady = 3`); the component state/context are not
initialized, so the manager link still does not run.

Restored the 32-component C pack, base AI pack, and original size table.
Next: inspect the actor state-3/event-AI initialization; possible control
points are the AIN's internal root/event bind data and
`EventPerformerParam.ExceptionalBindType`/`UseEventUniqueAin`.

### B2: AIN internal root-name patch breaks allocation

The private AIN's internal root name is the string `Player.event.root` at
`AI/PlayerC.event.root.ainb` offset `0x39a3c`. Patching it in place to the
unique same-length `PlayeC.event.root` and rebuilding the AI pack changed the
create result for the worse:

```text
P0: create C requested (returned 0x1)       (74.06)
handle holds 0x2147712f38: 0x2 0 0x101 0 0 0 0x2 0
(no actor pointer; fps ~2)
```

The unpatched private root (`AI/PlayerC...` with internal
`Player.event.root`) allocated actor `0x214c9aa238` (state 3). So the
internal AIN root name must stay `Player.event.root`; the name is part of the
AI root's internal lookup, not the singleton key, and changing it prevents
the root from loading.

Restored the 32-component C pack, base AI pack, and original size table.
Next: the actor state-3/event-AI initialization, testing
`EventPerformerParam.ExceptionalBindType` / `UseEventUniqueAin` variants
rather than the AIN root name.

### B2: EventPerformerParam field variants fail

Two field variants of the private event performer were tested with the private
AI root installed:

- `UseEventUniqueAin = False`:
  ```text
  handle 0x2147f12ed8: 0x2 0 0x101 0 0 0 0x2 0   (77.2)
  handle 0x2147f12d88: 0x2 0 0x101 0 0 0 0 0     (83.9, retry)
  ```
  No actor was allocated.
- `ExceptionalBindType = 0, UseEventUniqueAin = True`:
  ```text
  handle 0x2147312f38: 0x2 0 0x101 0 0 0 0 0
  ```
  No actor was allocated.

The original values (`UseEventUniqueAin = True`,
`ExceptionalBindType = 1875412497`) are required to allocate actor
`0x214c9aa238`; those values still leave it in state 3. The bind fields are
not the control point for the state-3 wait.

Restored the 32-component C pack, base AI pack, and original size table.
Next: the event-manager/AI initialization and the private Player context,
not the EventPerformerParam fields.

### B2: state-bisect1 result and the state-3 actor table

The `statebisect1` run remains the same: the player-state/player bridge
copied 42 components and queued the bind, but the event-arena null writes
occurred before the bridge, so there is no live manager link.

The private event root allocates the C actor in state 3, but its
`actor+0x270` table (`0x214ca1a968` in that run) does not have committed
`+0x158`/`+0x358` slots: the reads return unmapped/None. The component table
is built when the actor reaches the live state, so attaching the private
state/context to the state-3 table is not viable; the actor is waiting in
the event-AI/manager initialization before the table exists.

Event-root variants tested and rejected:
- internal AIN root name changed: no actor at all;
- `UseEventUniqueAin = False`: no actor;
- `ExceptionalBindType = 0`: no actor.

Only the original private root (`UseEventUniqueAin = True`,
`ExceptionalBindType = 1875412497`) allocates the actor, and it stays state 3.
The remaining Phase B work is the event-manager/AI initialization and the
private Player context, not the AIN path, bind fields, or state-3 table.

### B2: 32-pack + PLAYERBRIDGE reaches actor state 4; post-bind null remains

The `statebisect` run actually spawned the C actor live:

```text
P0 C spawned actor 0x214cc1e4e0, state 4
player state bridge child ... PlayerRef 0x214d2da818 <- Link1 0x214ae774f8
player bridge child ... copied 42 components
childholder bind queued link 0x21611ea130 child 0x214cc1e4e0
```

Then the manager link hit a null after the bind:

```text
75.520 Unmapped read 0x0 (8 bytes) at pc 0x80da6280 lr 0x80da62d8
       Cannot execute instruction at unmapped address 0x0
```

So the 32-component body + PLAYERBRIDGE/PLAYERSTATE gives a **live state-4
actor** and a valid `+0x158` path, but the manager update still nulls at the
`+0x358` event context. The 33-component private event root gets the actor
only to state 3; the bridge path gets state 4.

Next: attach a valid private `+0x358` context to the state-4 bridged actor
(combine the bridge with the private event root/context) instead of sharing
Link 1's context.

### B2: post-bind crash is an event/AI node virtual dispatch

The statebisect2 log base is `0x80bc4000`:

```text
pc 0x80da6280 = main+0x1e2280
lr 0x80da62d8 = main+0x1e22d8
Unmapped read 0x0 (8 bytes)
Cannot execute ... 0x0
```

The function at `main+0x1e2240`:

```text
1e2260: ldr x0, [x2]
1e2274: ldr x8, [x0]
1e2278: ldr x8, [x8, #0xf8]
1e227c: blr x8
1e2280: cbz x0, ...
1e22d4: bl  0x1e2240        ; recurse into the next node
```

It is a recursive event/AI-node update: the node at `x2` has a null
vtable/object, so the `vtable+0xf8` call leads to `read 0x0` and
`Cannot execute 0x0`. This is the post-bind crash for the state-4 bridged
actor; the bridge fixed the actor state/`+0x158`, but an event/AI node
component is still null.

Next: identify the `x2` component / vtable `+0xf8` and initialize the private
event/AI node (not just the `+0x358` table slot).

### B2: x2 event/AI-node capture attempts (inconclusive)

The statebisect2 crash stack has `sp 0x10e4c928f0`; the event/AI-node
function `main+0x1e2240` saves `x20/x19` at `[sp+0x20]` and `x21/x22` at
`[sp+0x10]`, so the `x2` node (`x19`) should be at `[sp+0x28]`, with the
player/context at `[sp+0x20]`.

Two capture attempts:

- GDB stub run: the create slowed to 25692 ms; the actor spawned state 4 at
  108.48, but no `NODE STOP` was recorded — the critical stop did not yield
  registers.
- No-GDB rerun: the game failed to boot (fps 0, no player), so the crash
  stack could not be read.

The x2 node/vtable+0xf8 remains unidentified. Next: retry after a clean boot
and read the crash stack `[sp+0x28]`/`[sp+0x20]` while the emulator stays
alive, then initialize the private event/AI node.

### B2: state-4 bridge is timing-dependent; held bind lock-crashes

Results across runs with the 32-component body + PLAYERBRIDGE/PLAYERSTATE:

- `statebisect2` (no bind hold): C actor state 4, 42 components copied,
  bind queued, then the **event/AI-node crash** at `main+0x1e2280`
  (`sp 0x10e4c928f0`, no `0x99`/`0xb0`).
- `stack2` (no hold): state 4 + bridge + bind, then the **`+0x158` lock
  crash** (`Unmapped read 0x99`, `0x855...`, `Cannot execute 0x0`).
- `stack3` (3 s bind hold): state 4 + bridge at 80.12, bind at 84.96, then
  the same lock crash at 88.24 (`0x99`/`0xb0`).

So whether the `+0x158` state is present at the manager update is a race;
the event/AI-node crash only occurs when it is. The held bind did not make
it deterministic.

The 0x20 actor-state bridge still lock-crashes (`0x99`/`0xb0`).

Next: make the bridge state application deterministic (apply/verify the
`+0x158` state before the manager update and bind) so the event/AI-node crash
is reproducible, then read `[sp+0x28]` (`x2`) / `[sp+0x20]` and initialize
the private event/AI node.

### B2: full-size 0x1c78 context copy attaches, still crashes on shared nodes

Measurements on the live plain 32-component C actor (no bridges):

```text
Link 1 table+0x158 0x214a9ba170  vtable main+0x4b47608
Link 1 table+0x358 0x214a62c1c0  vtable main+0x4b4dfc0
C      table+0x158 0 (none)
C      table+0x358 0 (none)
```

Reading all `0x1c78` bytes of Link 1's `+0x358` object shows:

```text
self references        39, highest self pointer at +0x1b40
code/vtable pointers   85, highest at +0x1c30
nonzero bytes beyond 0x14b0: 574 (up to +0x3124 in the block)
```

So the previous private-context copy (`0x14b0` bytes) truncated the
object's own self-references at `+0x1b40`. That truncation alone made the
old copy invalid.

The copy was corrected to the full `0x1c78` and the puppet block enlarged to
`0x3000` (context now at `block+0x1000`; the private state stays at
`block+0x700` and ends at `0xf40`). Result with
`PRIVATE_CONTEXT=1 PRIVATE_STATE=1 CHILDHOLDER=1 BIND=1`:

```text
private context copy 0x21071e0000 <- 0x214ac91388 attached to child
private state clone 0x21071df700 <- Link1 state 0x214ac88c68, lock init queued
Unmapped read 0x430675ea00000000 (8 bytes) at pc main+0x5e9e1c lr main+0x5ea188
Cannot execute ... 0x0
```

The full-size copy is now structurally complete, but the crash remains: the
context's *external* node objects (`0x11a...`/`0x11b...` script heap) are
still Link 1's, and their back-pointers target Link 1's context. The bogus
read address is ASCII-like (`0x43 0x06 0x75 0xea`), i.e. context data read as
a pointer.

Also corrected: actor-table slot `+0x2b0` is the **LifeRef/LifeParam**
component (possible inline path `?Component/LifeParam/Player...`), not
`PlayerRef`. The earlier hook uses of `+0x2b0` as "player_ref" were reading a
life component object.

Conclusion: `+0x1c78` is the correct object size and the copy now attaches,
but a private `+0x358` still requires duplicating the node graph, exactly as
the decision section says. The next unit of work is the node-graph
duplication (allocate a private node block, copy each external node,
relocate cross-references and vtable-independent pointers, retarget the
context's node list), not further context/state clones.

### Tooling fixes landed this session

- `LICHII_SPLIT_BLACKBOARD_SHARE` wrote an invalid A64 word: `0x5ffffe15`
  instead of `0x58fffe15` (LDR literal). The emulator raised
  `ExceptionRaised(exception = 0, pc = ..., code = 5FFFFE15)` when the patched
  lookup ran. Fixed; the fallback now executes.
- `LICHII_LOG_MODULES` dumps `Core::FindModules` bases once.
- `LICHII_LOG_LINKS` logs the scene link manager and its two slots once a
  second (`split: link manager ... slots ...`), enabling early watchpoints.
- `use_gdbstub=true` was persisted by the `-d 6543` runs and made every later
  launch hang before the CPU thread started. Config reset to
  `use_gdbstub=false`; the 24 GB crash-spam log was deleted.

### B2: Player-system arena duplication implemented; float pair used as pointer

Live layout of Link 1's `table+0x358` (vtable `main+0x4b4dfc0`):

```text
component            table+0x358 -> 0x214a75b358
component +0x08      tagged string  ?Component/PlayerParam/Player.game__component__PlayerParam.bgyml
component +0x10      0x123b3fb300    class main+0x4bf9010 ref object for PlayerParam
component +0x18      Link 1 actor
component +0x38..120 0x214a762870..0x214a7629d8  (30 blackboard value nodes)
component +0x338     blackboard registry state 0x214a75b2c0 (separate table slot)
```

The component sits `0x358` bytes into a `0x8000` Player-system block. The 30
blackboard nodes lie at `component+0x7520`, the ref/subsystem objects at
`+0x42a8`, `+0x5dd8`, etc. All of them fall inside the same `0x8000` block.

Implemented (`LICHII_SPLIT_PRIVATE_CONTEXT=1`, block enlarged to `0x10000`):

- copy the whole `0x8000` block to `block+0x3000`;
- relocate every 8-byte pointer into `[source, source+0x8000)`;
- set the copied component's owner (`+0x18`) to the child;
- attach the copied component (`block+0x3358`) to `child_table+0x358`.

It attaches correctly:

```text
private player arena copy 0x21073d1000 <- 0x214ae18300 (0x8000 bytes),
component 0x21073d1358 attached to child 0x214ad048f0
```

Verified in memory: `child_table+0x358 = 0x21071bd358`, first qword
`main+0x4b4dfc0`, `+0x18 = child actor`.

The crash remains, and the registers now identify it. With the new
`LICHII_LOG_FAULT_REGS` host dump:

```text
FAULTCTX vaddr 0x10 pc main+0x5e9e1c
  x8=0  x14=0x21073d1358  x15=0x430675ea00000000
  x19=0x21073d1358 (private Player component)  x20=0x28
  x22=0x214ad048f0 (child)  lr=main+0x5ea188
```

`0x430675ea00000000` is not a pointer: it is two floats,
`0x430675ea` = **134.46** (the child's X position) and `0.0`. The Player
component's update is loading a position vector and using it as an object
pointer at `+0x10`.

The blackboard fallback (`LICHII_SPLIT_BLACKBOARD_SHARE=1`) does not change
the crash, and the private-state clone is not required to trigger it: it
happens with the arena copy alone (`PRIVATE_CONTEXT=1`, no `PRIVATE_STATE`).

Remaining external per-player pointers outside the `0x8000` block (from the
component scan): `0x214a756e20` (component-0x4528), `0x214a781338`
(component+0x25e0), `0x214ab2ad88`, `0x214b10d378`, `0x21602cd4a8`,
`0x21603025a0`, `0x21603025d8`, `0x216034c768`, `0x21855ab8b8`,
`0x21855aba78`, `0x21e12be066`, plus the tagged
`0x212165c403`/`0x212dc9dc48`/`0x21472d97a8`. One of these supplies the
transform/position vector that the update misuses.

Next: extend the copy to the remaining per-player subsystem objects
(the `0x214a756xxx`, `0x214a78xxxx`, `0x214ab2xxxx`, `0x2160xxxx` clusters)
or retarget the specific component field that holds the position vector,
then retest the bind for fps > 0.

### B2: the crash is a dispatch/layout mismatch, not a missing external pointer

Scanned the private arena (`0x8000`) and Link 1's source arena for the failing
value `0x430675ea00000000`:

```text
private arena  1 hit  at arena+0x1c54 = component+0x18fc
source arena   0 hits
child actor    0 hits
```

The value is written *into the private copy by the game* (the child's
X position), not copied from Link 1. Comparing the field in both components:

```text
Link 1  component+0x18fc = 0x430475ea00000000  (0.0, 132.46)
private component+0x18fc = 0x430675ea00000000  (0.0, 134.46)
```

So `+0x18fc` is a position/offset field by design (Link 1 has its own X
there); it is not corruption. The private arena is being updated by the
child's own update, which is the intended effect of the copy.

The crash site (`main+0x5e9df0`, block start reported at `+0x5e9e1c`) reads
the component's list state: `ldr w22,[x19,#0x78]!`, then walks
`[x19+0x80]`/`[x19+0x88]`. In both components `+0x78` holds an *arena
pointer* (`arena+0x78d3`, relocated correctly to the private arena), and the
low 32 bits are used as a loop count. That is not a valid count for either
component, and the position pair is then used as a vtable/pointer.

Conclusion: the private Player component is now structurally complete and
being updated, but the update method invoked for the child expects a
different object layout (or a different dispatch slot) than Link 1's. This
is not fixable by copying more external objects; the next step is to
identify the caller and vtable slot that dispatches `main+0x5e9df0` for the
child and confirm the component type/slot mapping (`table+0x358`) is right.

The fault-context dump used for this analysis is the env-gated
`LICHII_LOG_FAULT_REGS` addition in the Fritchii Dynarmic MemoryRead
handler (x0..x30, sp, pc, lr on low-address unmapped reads).

### B2: corrected guest disassembly; crash is a nested registry lookup

The extracted `main.elf` has no section headers and its vaddr->file mapping is
shifted by `0x1000` for at least the `0x5e....` code region. Verified against
the guest bytes at runtime `main+0x5e9e1c`:

```text
guest instruction at main+0x5e9e1c: 0x394123e9  ldrb w9, [sp, #0x48]
objdump at vaddr 0x5e9e1c:          str w8, [x19, #0xcc]   (wrong file offset)
guest == main.elf file 0x5eae1c
```

So the earlier analysis of `main+0x5e9df0` was of the wrong bytes. Using the
guest bytes with capstone, the real caller at `main+0x5e9d30` is a component
update that does:

```text
[component+0x18] -> owner actor
[actor+0x270]    -> actor table
[table+0x1b0]    -> per-player link object
[table+0x200]    -> component map
map key 0x060890b0 -> entry 729 (sub table 0x2186328590, 82 entries,
                                 includes component key 0x2b1e8448)
then a nested hash lookup at map+0x4a0/+0x4ac
```

The map lookup succeeds; the fault (`0x430675ea00000000`, then `0x10`) is
inside the nested lookup chain, not a float-to-pointer conversion.

`table+0x1b0` was null for the child. Implemented
`LICHII_SPLIT_LINKOBJ=1`: clone Link 1's `0x200`-byte link cluster into
`block+0xc000`, relocate its self-references, set its owner (`+0x18`) to the
child and store it at `child_table+0x1b0`. It attaches:

```text
private link object 0x21071bd000 <- 0x214ac15c08 attached to child ...
child table+0x1b0 = 0x21071bd000, vtable main+0x4b48718, owner = child
```

The crash is unchanged with the link object present, so the remaining defect
is in the nested registry lookup/entry data, not the arena copy or the link
object.

Tooling note: the ELF's objdump mapping cannot be trusted for every region;
read the guest bytes with `read_memory` and disassemble with capstone.

### B2: registry inspected; nested table is not the crash

Live inspection of the component registry (`map` at `0x21860f0230`, same
guest address across runs):

```text
map+0xc10 entry count 874        map+0xc18 entries 0x21862fa390
map+0xc20 buckets     0x21862f6cf0  count (u32 +0xc2c) 1748
key 0x060890b0 -> bucket 1445, entry 729
entry 729: {0x8586a5f0, 0x2186328590, 0x29, 0x52, 0x1}
  sub table 82 entries, key 0x2b1e8448 at index 78 -> value 0x56bbe07c
map+0x4a0 buckets 0x21861a3410  count (u32 +0x4ac) 2
map+0x490 entry count 1         map+0x498 entries 0x21861a3420
nested buckets: key 0x6000f875 -> index 0; second bucket empty
nested entry 0: {0x859a8ad8, 0, 0xffffffff, 0}  byte +0x16 = 0
```

`0x56bbe07c` is **not** in the nested table; the lookup takes the fail path,
which still uses entry 0 and reads byte `+0x16 = 0`. That path is benign, so
the crash is elsewhere in the registry callee (slow/insert path or a nested
call), not a missing nested entry.

Also observed: enabling `LICHII_SPLIT_LINKOBJ` moves the run to the old
event-arena failure instead:

```text
private link object ... attached (vtable main+0x4b48718, owner = child)
Unmapped Write64 @ 0x0 = 0x00001200507433
Unmapped Write64 @ 0x0 = 0x00001200507b55
...
```

The cloned link object carries script/event fields (`+0x08/+0x10/+0x20` point
into the `0x12...` script heap), and the second player hits the singleton
event system's null global again. The link clone must either strip those
fields or the event system must be duplicated as well.

Next: trace the exact faulting instruction inside the registry callee
(single-step from the return address or per-memory-op logging) and decide
whether the cloned link object's event fields are the real trigger.

### B2: exact fault chain found; table+0x50 clone removes the 0x430675ea fault

The faulting function is `main+0x602100` (called from `main+0x5ea184` with
`x0 = child actor`), not the registry callee. Its chain:

```text
0x602100 ldr x8,[x0,#0x270]     ; actor table
0x602108 ldr x14,[x8,#0x358]    ; private Player component (checked)
0x602138 ldr x8,[x8,#0x50]      ; per-player chain head
0x602140 ldr x8,[x8,#0x20]
0x602144 ldr x8,[x8,#0x10]
0x602148 ldr x15,[x8,#0x58]     ; child: 0x430675ea00000000 (position)
0x60214c ldr x8,[x15]           ; fault here
```

Live comparison:

```text
Link 1 table+0x50 = 0x214a6acec8 (vtable main+0x4c66070)
  +0x20 = 0x214a9b8090 (vtable main+0x4c2e450)
  +0x10 = 0x214a9c27b0 (vtable main+0x4c34538)
  +0x58 = 0x214a9ca680 (data)
child table+0x50 = 0 (null)
```

Implemented `LICHII_SPLIT_CHAIN50=1`: copy the three objects (0x80/0x80/0x100
bytes) to `block+0xd000/0xd100/0xd200`, fix pointers equal to the originals,
and attach the head to `child_table+0x50`. It attaches:

```text
private chain50 0x2107803000(0x214b27dcf8) -> 0x2107803100(0x214b588ed0)
  -> 0x2107803200(0x214b5935f0) attached to child ...
```

The `0x430675ea00000000` / `0x10` fault is **gone**. The next failure is a
one-byte write loop:

```text
Unmapped Write8 @ 0x0000010000005e = 0
Unmapped Write8 @ 0x0000010000005f = 0
Unmapped Write8 @ 0x00000100000060 = 0
```

i.e. a buffer/container base of `0x100000000` plus an offset, from a pointer
field in the cloned chain (likely a vector/string whose base was not copied or
whose size field was copied without its backing store).

Next: inspect the data reached from `o3+0x58` and the code after the type
check to find which container is initialized at `0x100000000 + n`, then copy
or fix that backing store.

### B2: the tagged write is a manager entry, not a container copy

With `LICHII_SPLIT_CHAIN50=1` the `0x430675ea` fault is gone and the child's
`table+0x50` points at the clone (`0x2106bf0000`). The next failure is:

```text
FAULTWRITE vaddr 0x100000049 val 0 size 8 pc main+0x573cdc0 lr main+0x2eb7f4
  x0=0x100000049 x1=stack x2=0x24 x19=0x100000001 x24=0x214ac8aaa8
FAULTWCODE pc ...: 00000000 8b020024 8b020005 f102005f  (block start)
```

The caller is `main+0x2eb7cc` (guest bytes):

```text
2eb7cc ldr x19,[x24,#0x198]
2eb7d0 cbz x19, ...
2eb7d4 add x0, x19, #0x48
2eb7e0 mov w2, #0x24
2eb7f0 bl memcpy
```

`x24 = 0x214ac8aaa8`; its slots `+0x198..+0x1b0` are all `0x100000001`
(tagged empty), so `x19+0x48 = 0x100000049`. x24 is not the child actor
(`0x214c926bc8`), its table (`0x214c9ec5f0`) or its holder (`0x21472dd7c8`);
its first qword points to a descriptor containing `0x214c557260`.

So this is another manager-side entry that is empty for the child, not a
container whose backing store needs copying. Populating/inheriting
`[x24+0x198]` (and the three sibling slots) is the next manager-registration
step.
