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
