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
