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
