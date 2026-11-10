# M1b — the real SplitLink actor

M1a proves actor creation with the existing `CoopPuppet_Child` pack. M1b turns
that into a real second player.

## Actor pack

Start from the full `Player_Child` / `Player_Adult` actor and its `Player_Base`
parent. Keep the gameplay components that make Link a player:

- `PlayerRef` — the player component and its blackboard.
- `BSARef` — the player brain, action and scheduler graphs.
- `PhysicsRef`, `LifeRef`, `DeathRef`, `ConditionRef`.
- `PlayerCameraTargetRef`, `EquipmentUserRef`, `ShooterRef`, `LifterRef`.
- `ASRef`, `ASInfoRef`, `AnimationRef`, `ModelInfoRef`, `BoneBindRef`.
- `ELinkRef`, `SLinkRef`, `GameBalanceClientRef`.

Change or drop the parts that assume exactly one player:

- `ActorNameRef` currently points at `Player`. Replace it with a new
  `SplitLink` actor name so global "find the player" lookups still return
  Link 1.
- `EventPerformerRef` may attach the actor to the event system; keep it out
  for the first build and add it back if cutscenes need both Links.
- `GameLifeConditionRef` is `NoDelete`; keep it so the actor is not culled.

Build `SplitLink_Child.pack` and `SplitLink_Adult.pack` with a copy of the
co-op `tools/puppet_pack` builder. The pack must include the game's own
component files by path, exactly as the puppet pack does.

## Input

The player input lives in the `PlayerComponent` blackboard table:

| Key | Type | Meaning |
|---|---|---|
| `RawLeftStick` | vec3 | left stick |
| `RawRightStick` | vec3 | right stick / camera |
| `PlayerLeftStickLength` | f32 | left stick magnitude |
| `PlayerPreLeftStickLength` | f32 | previous magnitude |
| `RawLeftStickRadian` | f32 | left stick angle |
| `LeftStickRadianCameraStandard` | f32 | camera-relative angle |
| `PlayerRightStickLength` | f32 | right stick magnitude |
| `RawRightStickRadian` | f32 | right stick angle |
| `RollInput` | bool | roll request |
| `ReverseInput` | bool | reverse request |
| `PlayerVirtualKey` | u64 | virtual buttons |
| `PlayerVirtualKeyHold` | u64 | held buttons |
| `PlayerVirtualKeyRelease` | u64 | released buttons |

The game writes these for Link 1 from the global input. For Link 2:

1. Find the `PlayerRef` component and its blackboard instance.
2. Use the game's own blackboard setter for each key, or write the value
   slots directly if the layout is stable.
3. Do it every game frame before the second actor's AI runs.
4. Read controller 2 from Lichii's HID core (`NpadIdType::Player2`) and map
   it to the same virtual-key bits the game uses for controller 1.

Acceptance:

- controller 2 moves Link 2 only;
- controller 1 moves Link 1 only;
- Link 2 walks, runs, turns, jumps and rolls;
- no input leaks.

## Health and game over

- Read Link 1's `Life` component and Link 2's.
- Mirror the lower value to both, or make Link 2's Life a mirror of the
  shared pool.
- When the shared health reaches zero, let the game's own game-over flow run.
- Continue respawns Link 1 normally and then places Link 2 at Link 1's
  position.

## Camera

M2 uses a second camera for Link 2. M1b only needs Link 2 to exist and move;
the existing single camera can stay on Link 1 for the first input test.

## Experiments

1. Spawn `SplitLink_Child` with no input. Does it stand, collide and run its
   animation?
2. Write `RawLeftStick` directly. Does it move?
3. Add `PlayerVirtualKey`/`Hold`/`Release` for jump and sword. Does it jump
   and attack?
4. Log the blackboard key hashes and values every second.
5. Change `ActorNameRef`; confirm Link 1 still works and no global lookup
   picks Link 2.

## Result of the real-actor experiment (2026-11-10)

The full-player pack was built (all player components except
`EventPerformerRef`, then also without `ActorNameRef`, then with the
`Dm_Npc_Base` actor name). The game refuses it:

- `requestCreateActorAsync` queues the create, but the request returns
  state 4 (dropped) within a second, before any actor is present;
- the game's own `Player_Adult` row does the same or stays in state 2 until
  the 10 s create timeout;
- the `Player` actor is a singleton. `Dm_Player_Child_Op` has the player
  model and `PhysicsRef` but no `PlayerRef`/`BSARef`, and `Player_Event` has
  only AS/EquipmentUser/EventPerformer.

So a second `PlayerRef` cannot be created through the normal actor request.
The clone fallback in `PLAN.md` remains the only route to a true second
player; it needs the player actor and its components copied and registered
in the manager's second link slot.

## Implemented M1b: controller 2 drives the working puppet

Until the clone exists, the working `CoopPuppet_Child`/`Adult` actor is
driven by controller 2:

- `split/hook/split_hook.cpp` reads `NpadIdType::Player2` (sticks and
  buttons) every presented frame;
- the stick moves the puppet (walk 1.2 m/s, run 3.6 m/s above 75% stick)
  in the local camera frame; `0003-split-camera.patch` publishes the game
  camera from the Vulkan draw path;
- A jumps, X rolls, B plays the sword attack, ZR holds the shield;
- the puppet's own AS controller plays `Wait`, `Move`, `MoveRun`, `Jump`,
  `Rolling`, `ShieldGuard` and `Sword_Attack_Vertical1_From_L`;
- the existing director writes the position and rotation with the game's
  pose setter, so the actor is placed the way the game places actors;
- the puppet's Y follows Link 1's ground height within 2 m, so stairs and
  small ledges work in the same scene.

Turn it on with `LICHII_SPLIT_INPUT=1`. `LICHII_SPLIT_P2TEST=1` replaces
controller 2 with a repeating walk/run/jump/roll/attack cycle for testing.

This is not the final player: the puppet has no player physics, collision
against walls, pickups or its own Life component. Split rendering (M2) and
the shared-health bridge still need the real actor or host-side equivalents.
