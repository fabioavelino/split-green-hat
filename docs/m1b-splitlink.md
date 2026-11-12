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

## CoopLink with Life and Physics (dropped)

A `CoopLink_Child/Adult` pack adds `LifeRef`, `PhysicsRef`, `DeathRef`,
`ConditionRef` and `ActorPositionCalculatorRef` to the draw-only puppet,
still without `PlayerRef`. The game drops its create request too (state 4
within a second), so the body/life components are not usable through the
normal actor request. The draw-only `CoopPuppet` remains the working actor.

## Real-body fallback: `Dm_Player_Child_Op`

The full `Player`/`SplitLink` actor and the custom `CoopLink` actor are both
refused by the create path. The game's own cutscene actor
`Dm_Player_Child_Op` does create and is a real player-model actor with a
`PhysicsRef`: it renders, plays Link's AS commands (`Wait`, `MoveRun`,
`Rolling`, `Jump`) and holds the target pose.

```sh
LICHII_SPLIT_ACTOR=Dm_Player_Child_Op LICHII_SPLIT_INPUT=1 LICHII_SPLIT_MATRIX=1 LICHII_SPLIT_P2TEST=1
```

`LICHII_SPLIT_INPUT=1` turns off the pose copy so the director writes P2's
pose. `LICHII_SPLIT_MATRIX=1` enables the game's matrix setter for actors
with the common vtable; the cutscene actor has its own vtable, so its pose
writes are what move it. It stays at the target ground position.

Limitations: `Dm_Player_Child_Op` is the child model, and there is no adult
cutscene player body. The adult path still uses the draw-only
`CoopPuppet_Adult`. The cutscene `PhysicsRef` is not the player collision
controller, so this is a real body but not the full player collision.

## Shared health and game over

`BridgeChildLife` writes Link 1's Life component pointer into Link 2's empty
Life slot (`[[actor + 0x270] + 0x2b8]`). The log confirms the transition:

```text
split: life bridge slot 0 actor ... components ... life 0x0
split: life bridge slot 0 actor ... components ... life 0x214b399278
```

Both actors now point at one Life object; the shared health overlay reads that
object. `LICHII_SPLIT_DRAIN=1` drains it by one heart a second for testing and
reaches:

```text
split: debug drain health to 0
split: shared game over (health 0/32), hiding Link 2
```

The drain **does** raise the game's own Game Over screen; the death event and
fade simply take about ten minutes of wall time at this emulator's frame rate.
The presented frame is then the game's own screen:

```text
GAME OVER
  Try Again      Continue from last save.
  Quit Game
```

(Captured in `docs/img/game-over.png`.) Pressing A on `Try Again` reloads the
checkpoint. With the one-shot drain and the real-body actor the clean run
(`LICHII_SPLIT_ACTOR=Dm_Player_Child_Op LICHII_SPLIT_INPUT=1
LICHII_SPLIT_MATRIX=1 LICHII_SPLIT_DRAIN=1`) logs:

```text
split: shared health restored (16/32), showing Link 2
split: coop: puppet: P0: create Dm_Player_Child_Op requested (returned 0x1)
split: coop: puppet: P0: took actor 0x214c123c00 from request 0x2147311d68
split: coop: puppet: P0 Dm_Player_Child_Op spawned after 6643 ms:
      actor 0x214c123c00, state 4, at (134.42, 105.61, -34.05)
split: life bridge slot 0 actor 0x214c123c00
      components 0x214a2617a8 life 0x214a31c508
```

A live read after the cutscene confirms both Links and one shared Life:

```text
Link 1 actor 0x2149e046c8 state 4 pos (132.46 105.58 -34.00)
Link 2 actor 0x214c123c00 state 4 pos (134.46 105.58 -34.00)
Link 1 life 0x214a31c508 value 16/32
Link 2 life slot 0x214a31c508
```

So Continue restarts Link 1, recreates Link 2 and re-applies the shared Life
pointer.

### Mod actor create: state 4 and the instance heap

The draw-only `CoopPuppet_*` packs were built with a **minimal** ActorInfo row
(`pp.info_row`: no `ActorName`, `ELinkUserName` or `SLinkUserName`). The actor
manager drops that request before creation with state 4:

```text
P0 CoopPuppet_Adult: request 0x2147712ff8 word 0x60004: 0x60004 0 0x101 ...
P0: request 0x2147712ff8 dropped (state 4); trying again in 5 s
```

The builder now gives every mod actor the full player row with its own
ELink/SLink users, no `DefaultAffiliation`/`MaxLife`, and
`InstanceHeapSize = 0` (the player row's 10 MB is not needed here). With that,
the request state is 3 and the actor is created. The remaining difference is
the variant's model/AS set: the child uses `Player_Child` and creates, while
the adult uses the generic `Player` model/AS bound to the singleton adult
player project `Player_Adult` and is still refused with state 4. There is no
`Dm_Player_Adult_Op` cutscene body.

The director's create timeout was also too short for this host: at about 2
fps the child create completed after **19.9 s**, after the old 10 s timeout
had already dropped it and started over. Raised to 60 s, the child now
spawns:

```text
P0: took actor 0x214ab7f3f8 from request 0x2147d12f68
P0 CoopPuppet_Child spawned after 19922 ms: actor 0x214ab7f3f8, state 4
```

(`LICHII_SPLIT_ACTOR=CoopPuppet_Child LICHII_SPLIT_INPUT=1
LICHII_SPLIT_MATRIX=1`.) The draw-only child has no physics and falls through
the floor; the working real body remains `Dm_Player_Child_Op`.

### Adult model A/B

Building all output names from one player source separates the actor name from
the model/AS set:

| source | output name | result |
| --- | --- | --- |
| Player_Child | CoopPuppet_Child | creates after 19922 ms |
| Player_Child | CoopPuppet_Adult | state 4 in ~4 s |
| Player_Adult | CoopPuppet_Child | creates after 18153 ms |
| Player_Adult | SplitLink_Child (full player components) | state 4 |

So the adult model/AS alone can create under the working child name; the name
`CoopPuppet_Adult` is specifically refused, and the full player component set
(PlayerRef/BSARef) is still refused under the child name. The adult-model
`CoopPuppet_Child` actor is draw-only and falls through the floor (no physics);
the real body remains the child `Dm_Player_Child_Op`.

### Bridged player components and the holder

`LICHII_SPLIT_PLAYERBRIDGE=1` copies every non-zero Link 1 component pointer
into a zero child slot (the Life slot stays the shared bridge):

```text
player bridge child 0x214a155a80 copied 20 components mask 0x2142886620091028
shared life child 0x214a155a80 -> Link1 life 0x214a71f470
```

The 20 components are the player-only slots the cutscene pack lacks
(PlayerRef/BSARef among them). The empty-link holder clone with a populated
link name (`+0x20` points at the name at `+0x2c`) still crashes with a wild
jump into the sdk string table (`Unmapped read 0xa0/0xb0`), so a shallow
holder/sub-holder copy is not safe.

The child does have its own game-created holder: `[actor + 0x1b0]` is the
holder, with the actor/id at `+0x40/+0x48` and the sub-holder/sub-id at
`+0xa0/+0xa8`. On the live child the holder was `0x2147cdd7c8`, id `0x174`,
sub-holder `0x214c730da0`, sub-id `0x175`. Registering that holder directly
(`LICHII_SPLIT_CHILDHOLDER=1`, no copy) works at the link level:

```text
childholder empty 0x2160dea130 <- child 0x214a5d8680
    holder 0x21476dd7c8 id 0x174
```

`manager+0x40` then reads the child holder, but the game still crashes with
the same `Unmapped read 0xa0/0xb0` wild jump. Using the active player link
name (`LICHII_SPLIT_LINKNAME=Player_Adult`) and sharing Link 1's active
sub-holder object (`LICHII_SPLIT_SUBSHARE=1`) did not change it. The child
holder is a generic actor holder; the manager's player update expects a
player-specific state object (the player holder/PlayerRef owner), which the
cutscene child does not have. The remaining piece is that player holder
state, not the actor holder.

### PlayerRef is the state-4 trigger

The 31-component `CoopLink_Child` body (puppet base + actor plumbing +
`PhysicsRef`/`LifeRef`, no `PlayerRef`/`BSARef`) creates and holds the target:

```text
CoopLink_Child spawned after 6635 ms: actor 0x214ca77808, state 4,
    at (134.46, 105.58, -34.00)
```

Adding only `PlayerRef` to the same pack returns state 4:

```text
CoopLink_Child: request 0x2147912f08 word 0x60004: 0x60004 0 0x101 ...
dropped (state 4)
```

So the actor-name/pack/row system is fine; the full-player create refusal is
the second `PlayerRef` component. The CoopLink body also has the actor holder
and a real physics body. With the post-create bridge it receives **42**
player-only component pointers (mask `0x214a886e216f7d7f`) and its own holder
is registered in `manager+0x40` (id `0x174`), but the manager's player update
still crashes with `0xa0/0xb0`. Bridging the component pointers is not enough;
the player holder state itself must be cloned or created.

The direct actor-Life drain is therefore enough to start the game's death
flow. A second attempt routed the drain through the game's own
`PlayerStatus.Life` flag queue (`GameFlags::set_int`, hash `0x31ab5580`). The
queue accepts the write but the value read back stays 16:

```text
split: debug drain flag life 16 -> 12 (queued true)
split: debug drain flag life 16 -> 12 (queued true)
```

So `PlayerStatus.Life` is a save/status flag, not the live player death
trigger; the actor `Life` value is the one the death flow watches.

Two more attempts were made to drive the player's own death path:

- **Whole hit record** (`LICHII_SPLIT_HIT=1`): the writer uses the game's
  proven enemy hit layout (header at Life+0x1f8, amount +0x224/+0x228, hit
  record +0x240, present last). The log shows the hit being written, but the
  player's life stays 16; the Player Life component does not consume the
  generic enemy hit record.
- **Death AS** (`LICHII_SPLIT_DEATH_AS=Dead`): a one-shot `kPlayAs` request is
  queued through the puppet director's code cave and played on Link 1. The
  log confirms `split: death AS 'Dead' queued on Link1 ...`, but the AS alone
  only plays the animation; it does not set the Life value to 0, so it does
  not start the death flow.

The deaths that reached the Game Over screen came from the Life value
reaching 0, not from the status flag, the enemy hit record or the AS
animation. The player
AS graph does contain `Dead`, `DeadSelf`, `Die`, `DieState` and the `Damage*`
reactions, so the animation is not the trigger. The BSA action graph is the
next layer: `AI/Player.action.root.ainb` names `InterruptDie`,
`reactionpredie` and `InterruptFallDamage`; the Murmur3 x86-32 hash of
`InterruptDie` is `0x60a72e82`, present in the player's
`ActionVerbAttributesSetting` table. The live BSA component
`[[actor + 0x270] + 0xb0]` did not contain that hash in its first 0x2000
bytes while Link was idle, so the current verb lives in its
blackboard/subobject. The missing interface is the BSA action request that
selects `InterruptDie`.

`Pack/AI.Global.Product.111.pack.zs` exposes the BSA action API node names:

```text
AI/ActorCommon.SetActionByActionVerbContainerSelectorCommon.module.ainb
  ActionVerb
  ActionVerbContainerSelectorKey
  ActionVerbVariation
  ActionSlot
  PickedActionVerbContainerIndex
  ExecuteBSASetActionVerb
  QueryBSAGetActionVerbFromContainer
  ExecuteBSAInvokeActionVerbWithArgsWithResultPolicyPlug
  SelectorActionVerbContainerStateSelector
```

`AI/Player.action.common.module.ainb` adds
`OneShotPlayerResetMainActionStatus`; `AI/ActorCommon.OnInterruptActionEnter`
adds the interrupt/action-verb enter path.

The global BSA blackboard table is in
`Pack/ResidentCommon.pack.zs`, member
`Component/Blackboard/BlackboardParamTable/BSA.engine__component__BlackboardParamTable.bgyml`.
It holds only:

```text
ActionSeq_IsWaitingForValidPath
SmallDamageIgnoreFromActionVerbContainer
ForceGuardNoReaction
IsPathfindingUserNoCalc
OneShotActionSeqKey
```

There is no action-verb key there. The player's input blackboard is the
component at table offset `0xb0`; its instance contains the proven input keys
(`RawLeftStick`, `RawRightStick`, `PlayerVirtualKey*`, etc.) but not the
action verb. Live dumps of the table+0xb0 PlayerRef component during the
`Dead`-AS death event still do not contain the `Wait`/`Damage`/`InterruptDie`
verb hashes. The native `ExecuteBSASetActionVerb` dispatch/node-function
table (or the actual action-slot object) is the remaining missing interface.

A true second player still needs the clone fallback or a registered second
link slot, because the engine refuses a second full `Player`/`CoopLink`
actor.

## Live player-manager link slots

The manager path is `main+0x4e97648` -> `scene+0x1f0` -> `table+0x58` ->
`array+0x78` -> `manager`; `manager+0x30` must be set. On the current save:

```text
manager+0x40 link  holder 0            id 0xffffffff   empty
manager+0x50 link  holder 0x21478d7408 id 0xbb         actor 0x214aa6e358, state 4
holder+0x40        actor Link 1
holder+0x48        id 0xbb
holder+0xa0        sub-holder 0x214aa6eb00, id 0xbc
```

So the **active Link is in slot 0x50**, and the empty slot is **0x40**, the
opposite of the usual `manager+0x50` second-player assumption. The holder is
a chain (`+0xa0` sub-holder id `0xbc`), so populating the empty link is not a
single pointer write: it needs a complete player holder/actor with
`PlayerRef`, `BSARef`, `PhysicsRef` and `LifeRef`. `Dm_Player_Child_Op` has
none of the player components, so pointing the empty link at it would not make
a real second player.

A copy experiment (`LICHII_SPLIT_LINKCOPY=1`) writes the active link's
holder/id into the empty link:

```text
split: linkcopy empty 0x21615ea130 <- active 0x21615ea1c0 holder 0x2147ed72e8 id 0xba
```

The player manager accepts the link (the log appears once, then both slots
read valid), but the game immediately goes black at FPS 0. A second link must
carry a **distinct complete holder/actor**, not the active holder; the
manager then runs player logic on it and cannot share the same holder. This
confirms the clone fallback needs a full player holder/actor with its own
`PlayerRef`, `BSARef`, `PhysicsRef` and `LifeRef`, not just a second link
entry.

A distinct-copy experiment (`LICHII_SPLIT_LINKCLONE=1`) goes further: it
copies the active holder and its sub-holder into the puppet block, points the
copied holder at the `Dm_Player_Child_Op` actor, and writes id `0xcc` into
the empty link. The write is accepted:

```text
split: linkclone empty 0x21615ea130 -> holder 0x21075ec700 sub 0x21075ec800
       child 0x214c81cdb8 id 0xcc
```

The game then faults immediately:

```text
Unmapped read 0x99 (1 bytes) at pc 0x85f5a7f0
Unmapped read 0xb0 (4 bytes) at pc 0x85f5ef60
Unmapped read 0xa0 (4 bytes) at pc 0x85f5a824
```

The manager dereferences a null player component (the cutscene child has no
`PlayerRef`), so a distinct holder with the cutscene body is not enough. The
second link needs a real player component set, which the create path still
refuses. `LICHII_SPLIT_LINKCLONE` remains an experiment only.

The director now has a `QueueCall` path for one-shot game functions on the
calc thread; `coop_puppet.h`/`coop_puppet.cpp` are packaged in
`lichii/submodule-overrides` and the fetch script copies them. The debug
knobs are `LICHII_SPLIT_HIT`, `LICHII_SPLIT_DEATH_AS` and
`LICHII_SPLIT_DRAIN`.

The old note below still applies to the draw-only puppet:

- Link 2 cannot lose health on its own without a real player death flow;
  damage to Link 1 is the shared damage until the clone exists.

## M2 split-screen

Split-screen is on by default. Set `LICHII_SPLIT_SCREEN=0` to use the
single-camera two-Link mode.

- Frames alternate between player 1's and player 2's camera.
- The presenter keeps one history image per view and blits the left half
  from view 0 and the right half from view 1 (top/bottom when
  `layout = horizontal` in `split.ini`).
- `0004-split-present.patch` is the Vulkan present-manager part; the camera
  override is our submodule patch.

The camera override targets the game's 176-byte scene camera uniform
(stage 0/2, index 3):

```text
+0x00  3x4 view
+0x30  4x4 view-projection
+0x70  4x4 projection
```

Before the draw's buffer upload, the renderer rewrites the scene camera:

- player 1's projection is shifted so Link 1's projected chest position
  lands at the centre of the left/top half;
- player 2's chase camera keeps the primary camera's distance and height
  relative to Link 1, rotates that offset by Link 2's yaw, aims at Link 2's
  chest and shifts its projection into the right/bottom half.

The shift keeps each player in the centre of their half while the HUD (drawn
with a different camera) stays at the screen edges. The update is applied once
per render frame per camera buffer. The cache remembers the unshifted primary
and the last matrices it wrote, so a buffer the game has not refreshed is
never mistaken for a new primary.

Confirmed on macOS: the left/right layout centred Link 1 in the left half
with the HUD intact; the top/bottom layout centred Link 1 in the top half and
showed player 2's chase view in the bottom half.
`LICHII_SPLIT_CAMERA_RIG=0` keeps the simpler translation-only camera.

The 16 GB M1 test host renders at about 1–2 fps, so the split is half rate
per side; a discrete GPU is needed for a smooth test. The presenter logs the
state once:

```text
present: split true blit true view 1 extent 2560x1440
```

Known limitations:

- The HUD is part of the game frame, so the left half of the HUD comes from
  view 0 and the right half from view 1. The hearts and item slots therefore
  come from different frames.
- The top/bottom layout is a centre crop of the full projection, not a
  half-height viewport, so each view keeps the full-frame vertical FOV but
  loses the outer horizontal field. Player 1's framing is centred; player 2's
  centring depends on the puppet being drawn at the rig position.
