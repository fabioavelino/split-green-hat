# M1 experiments — second real Link

M1 is a go/no-go milestone. Do not start the split renderer until these
experiments work in the real game.

## Experiment 1: create an actor

Known-good path from the co-op project:

- `requestCreateActorAsync` is at `main+0x372a70`.
- The co-op puppet (`CoopPuppet_Child`) is already created through this path.
- The puppet proves actor creation, model binding, animation and pose writes.

Steps:

1. Add a debug hotkey (F8) that creates a harmless actor at Link 1's position:
   - first `Animal_Cow_LinkHouse` (sanity check),
   - then `CoopPuppet_Child` (model/animation),
   - then `Player_Child` (real player actor).
2. Log the returned actor address, its `+0x2c` state and its component table
   `+0x270`.
3. Take a screenshot and read the actor list to confirm it exists.

Go: the actor appears, renders and stays alive for 10 seconds.
No-go: the engine refuses a second `Player_*` actor, or it is deleted at once.

## Experiment 2: real player components

If `Player_Child` is created but is a statue:

1. Compare its component table with Link 1's.
2. Add the missing components in the actor pack, in this order:
   - physics/collision,
   - AS/animation,
   - Life,
   - EquipmentUser,
   - PlayerCameraTarget,
   - movement/AI or BSA graph.
3. Verify the actor animates and collides.

The preferred actor pack is a merge of `Player_Child`/`Player_Adult` with
`Player_Base`, keeping only what a second player needs. The co-op project's
`tools/puppet_pack` is the reference for building such a pack.

## Experiment 3: input isolation

Find the player input read:

- Read controller 1 and controller 2 from Lichii's HID core.
- Hook the second actor's input path with a code cave that sets a
  "current player = 2" flag while its update runs.
- Fallback: write controller 2's state into the second actor's input component
  before its update.

Acceptance:

- controller 2 moves Link 2 only;
- controller 1 moves Link 1 only;
- no button or stick leaks between them.

## Experiment 4: health bridge

- Read Link 1's Life component:
  `*(*(*(actor + 0x270) + 0x2b8) + 0x1c10) + 8`.
- Read Link 2's Life component the same way.
- Mirror the lower value to both, and expose the shared value for the HUD.
- Verify a hit on Link 2 lowers Link 1's health and vice versa.

## Experiment 5: static second camera

Before the full split renderer:

1. Capture the main camera matrix from the Vulkan draw path.
2. Compute a static offset camera 3 units behind and 2 units above Link 1.
3. Render one frame with that camera into the right half of the screen.
4. Confirm the second view is a valid 3D view and not upside down.

This proves the camera and viewport paths without needing Link 2.

## Logging

Every experiment logs with a `split:` prefix:

```
split: m1 spawn requested at ...
split: m1 actor <addr> name Player_Child state 4 components ...
split: m1 input P2 buttons ... stick ...
split: m1 health L1 16/16 L2 16/16
split: m1 camera static offset ...
```

Use `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` and the debug MCP adapter for
screenshots, memory reads and input replays.
