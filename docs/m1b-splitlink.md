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

The PlayerRef create check was tested for the obvious causes:

- `ActorNameRef`/ActorInfo row `ActorName = Player` (`LICHII_ACTOR_NAME=Player`)
  still returns state 4;
- restoring the child's 5,692,448-byte `InstanceHeapSize`
  (`LICHII_ROW_HEAP=5692448`) still returns state 4;
- the `PlayerParam` resource is present in the pack
  (`Component/PlayerParam/Player.game__component__PlayerParam.bgyml`).

The PlayerRef type descriptor is in main at module offset `0x2d7c930`: the
path getter `0x2d7c9a0` returns the PlayerParam path, the type id is
`0xccd658d3` and the component size `0x48`. Its factory at module `0x9b5d30`
constructs the parameter object without an obvious singleton branch. The
refusal is therefore in the actor/component/player-manager registration, not
the name, resource or instance heap.

A private copy of the PlayerComponent blackboard table was also tried: the
PlayerParam's `BlackboardTableRef` was changed to a pack-local
`?Component/Blackboard/BlackboardParamTable/PlayerComponentCopy...` member.
The create still returned state 4. That rules out the shared global blackboard
table as the cause.

The static PlayerRef type object at module `0x4974a48` was also inspected. Its
method table (`0x260d1c0`, `0x260d4b0`, `0x260cb50`) is the PlayerComponent
blackboard parameter mapping: it compares and writes the input keys by hash
(`0x448fe587`, `0x27dd9fdc`, `0xd3619015`, `0x675c1b00`). It contains no
actor-creation or singleton logic. The refusal is therefore outside the
param/blackboard layer, in the actor/player-manager registration path.

The player manager's `+0x18` field (observed value 1) is not a simple player
count: writing 2 there before the create crashes the actor list with null
writes. It is a pointer/state field, not the registration limit.

### GDB stub trace

The emulator's GDB stub was enabled with `-d 6543` and driven over RSP. The
guest is suspended at startup until a client continues it; a write watchpoint
on the create handle slot caught the live request pointer
(`0x2147912fc8`). A watchpoint on the request state word then stopped
repeatedly in the BYML parser at `main+0x668de4` while the state stayed `2`:

```text
stop state 0x2 request 0x2147912fc8 pc 0x8105ede4
```

The state-4 writer was not reached in the traced window: the parser loop
dominates while the request is waiting for the actor resources. The stub and
watchpoints work; the next trace needs a breakpoint on the actor-manager
error/return path or a longer window after the parser returns.

A follow-up trace tried to break at the parser's LR. The repeated parser stop
was `pc 0x811dede4, lr 0x811dee8c`; the state-watch removal did not take effect
in that client, so the parser loop continued and the caller breakpoint was not
reached.

The next trace did capture the state word `0x60004`:

```text
stop state 0x60004 pc 0x811df950 lr 0x811df904
returned to caller pc 0x811df9c4
```

With main base `0x80b76000`, those are `main+0x669950` and `main+0x6699c4`.
Disassembly of `main+0x669800` shows a path/string helper: it allocates a
0x168-byte object, copies a name into it, scans for `/`, and stores the path
ends. It is not an explicit PlayerRef branch. The request memory is being read
as a string/path buffer at this point, so the state-4 write is occurring in the
actor path/name handling. This shifts the failure hypothesis from a PlayerRef
singleton check to request/name/path buffer handling or the actor path load.
The next step is to isolate the name/path lengths (short actor name, short
component paths) or inspect the request/name buffer layout.

A first short-path test renamed the PlayerRef parameter and its blackboard
member to `?P.bgyml`/`?B.bgyml`. The game then hung at boot: no `M1 game
build` or `M1b player actor` log appeared, so it never reached the main
module or the save. The pack-local `?B.bgyml` blackboard ref is not accepted
by the loader; the path-buffer hypothesis remains unverified.

A safer alias test kept the original `Work/...` blackboard ref and added a
`?P.bgyml` copy of the PlayerParam, pointing only PlayerRef at the alias. The
game also hung at boot (no `M1 game build`), so the canonical PlayerParam path
is required.

A short-actor-name attempt used an ad hoc `LICHII_COOPLINK_NAME=C`. The knob
applied to both variants, so the child and adult builds both wrote
`Pack/Actor/C.pack.zs` and the adult row overwrote the child. The boot never
reached the main module in the test window, so the test is invalid; a
per-variant name is required for a meaningful result.

The builder now has a child-only `LICHII_COOPLINK_CHILD_NAME` knob for that
test. A clean run with `LICHII_COOPLINK_CHILD_NAME=C`, canonical
PlayerParam/blackboard paths and a cleared Vulkan pipeline cache stalled after
pipeline prebuild:

```text
Prebuild: background done ... 0 taken by draws
```

No `M1 game build` or `M1b player actor` appeared. A no-mod baseline launch
stalled at the same point, so the hang is the emulator/host GPU state, not the
mod pack. The child-only short-name result is pending a fresh host/GPU state
(Mac reboot or fresh session). Normal packs were rebuilt.

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

## Clean C test: PlayerRef, not the actor name

The child-only short-name build (`LICHII_COOPLINK_CHILD_NAME=C`) keeps the
canonical PlayerParam/blackboard paths and adds only `PlayerRef` to the
31-component `CoopLink_Child` body. `C.pack.zs` has 282 members and 32
components, with the canonical PlayerParam member present.

With the emulator booting normally and the co-op cave applied, the create is
refused in under a second:

```text
coop: puppets on, actor C, test puppet beside you
coop: puppet: P0: create C requested (returned 0x1), handle slot 0x210701f110
coop: puppet: P0 C: request 0x2147712f08 word 0x60004: 0x60004 0 0x101 0 ...
coop: puppet: P0: request 0x2147712f08 dropped (state 4); trying again in 5 s
```

So a short actor name with canonical paths does not help: the second
`PlayerRef` component is refused by the actor/player-manager registration
itself.

## CLI startup fixes

Two CLI-only integration bugs were found while bringing the persistent build
back:

- `~/.config/eden/sdl2-config.ini` had `use_gdbstub=true`, so every launch
  suspended the guest waiting for a debugger. The symptom is FPS 0 with all
  guest cores idle and no log lines after startup. Set it to false.
- The CLI has no Qt split panel, so nothing called `SplitHook::Start()`. Patch
  0002 now calls it from `PatchManager::PatchNSO` **before**
  `CoopSvc::PatchMain`, because Start sets the co-op environment that decides
  whether the puppet cave is installed. Without this the cave is skipped and
  the director never runs; `LICHII_COOP_*` env cannot repair it afterwards.

With both fixed, the rebuilt `eden-cli` boots, loads the Temple of Time save
and renders gameplay (see `docs/img/cli-gameplay.png`). `split: loaded` and
`split: on` appear at 0.8 s in the boot log.

## PlayerRef: affiliation and type-vtable checks

The `C` rows used for the PlayerRef tests had `DefaultAffiliation` and
`MaxLife` removed (the row isolation that made the draw-only packs create).
A fresh build keeps them (`LICHII_ROW_KEEP_AFFILIATION=1`):

```text
C  ActorName Dm_Npc_Base  ELink/SLink C  InstanceHeapSize 0
   GameActorInfo: DefaultAffiliation Player, MaxLife 12, MainRigidBodyLayer Player
   ActorParam: 32 components, PlayerRef ?Component/PlayerParam/Player...
```

The create still returns state 4 in about a second:

```text
coop: puppet: P0: create C requested (returned 0x1)
coop: puppet: P0 C: request ... word 0x60004 ...
coop: puppet: P0: request ... dropped (state 4)
```

So the player affiliation and max-life fields are not the cause either.

The PlayerRef component type has a vtable at image `0x4b424c8` (methods are
pointers into `.text`):

```text
+0x00 0x2d7c9a0  path getter -> "Component/PlayerParam/..."
+0x08 0x2d7c9b0  type id 0xccd658d3
+0x10 0x2d7c9c0  size 0x48
+0x18 0x2d7c9d0  returns 0
+0x50 0x2d7c9e0  guid 0x734ff7e7d7b7fb33
```

The flag getters return 0, so the type itself is not marked unique. That
points at the player-manager registration, not the component type.

Correction to the earlier trace: `main+0x9b5d30`, previously called the
PlayerRef factory, is a per-frame player-parameter helper. A GDB breakpoint
there hit 201 times during normal play (same LR `main+0x968c14`), freezing the
guest. The factory is not at that address.

## The second-PlayerRef blocker is EquipmentUserRef

The failed create request carries a live object at word 7. Reading it gave
the missing piece: an XLink/equipment user whose name is
`Weapon_Shield_Hylian`. The create was not refused by `PlayerRef`; it was
refused while binding the player's equipment links, which are singletons.

Removing the equipment user from the CoopLink body fixes the create. With
`LICHII_COOPLINK_DROP=EquipmentUserRef` and `PlayerRef`+`BSARef`:

```text
coop: puppet: P0: create C requested (returned 0x1)
coop: puppet: P0: took actor 0x214a575fc0 from request 0x2147512ed8
coop: puppet: P0 C spawned after 1928 ms: actor 0x214a575fc0, state 4
coop: puppet: P0 live actor 0x214a575fc0 state 4 at (134.46, 105.58, -34.00) (show)
```

The C ActorParam has 32 components: `PlayerRef`, `BSARef`, `LifeRef`,
`PhysicsRef`, the actor plumbing and the `Player_Child` model/AS, with no
`EquipmentUserRef`. This is the first Link 2 with a real player component set
(not just the cutscene body).

The adult variant with the same recipe (`CoopLink_Adult`, `Player_Adult`
model/AS) still returns state 4, so the remaining adult blocker is the adult
model/AS resource, not PlayerRef.

Registering the C actor's own holder in `manager+0x40` still crashes the
manager's player update:

```text
Unmapped read 0x99 / 0xb0 / 0xa0
Cannot execute instruction at unmapped address 0x0
```

Using the registered `Player_Adult` link name does not change it. The missing
object is the player-state object the manager expects behind the link, not
the actor's components.

A caveat on the earlier conclusion: `PlayerRef` alone on the 31-component
body failed because the body still carried `EquipmentUserRef`; it was the
equipment XLink, not the PlayerRef registration.

## Native adult menu body

`PouchPlayer_Adult` is a native adult player-model actor:

```text
FmdbName Player_Adult, ASRef ?AS/Player_Pouch.root.asb,
SLinkUserName PouchPlayer_Adult, 19 components
```

It has no `PlayerRef`, `BSARef` or `LifeRef` (it is a `MenuActorParam`), but it
**creates** in the world:

```text
P0 PouchPlayer_Adult spawned after 3262 ms: actor 0x2149f11830, state 4
P0 live actor 0x2149f11830 state 4 at (134.46, 105.58, -34.00) (show)
```

Its own AS root (`Player_Pouch`) is distinct from Link 1's
`Player_Adult.root.asb`, so the adult AS conflict can potentially be broken by
merging the player components onto this pack. As a menu actor it does not draw
in the world; the Temple frame shows only Link 1 (`docs/img/pouch-adult.png`).

## Split presenter with the C body

The new binary's split presenter is active:

```text
present: split true blit true view 1 extent 2560x1440
[splitcam] rig[0] eye=(134.46 107.72 -38.48) fwd=(0.00 -0.14 0.99)
           link2=(134.46 105.58 -34.00) yaw=-1.57
```

The C player-component body is live and shown at the target while the split
camera tracks it. The equipment-less C body's model composition is still
unconfirmed; the frame is saved as `docs/img/split-c.png`.

## Pouch AS adult merge

`PouchLink_Adult` merges the Pouch adult model/AS/equipment with the player
components: 34 components including `PlayerRef`, `BSARef`, `LifeRef`,
`PhysicsRef=Player.engine...`, `ASRef=?AS/Player_Pouch.root.asb` and
`EquipmentUserRef=PouchPlayer_Adult`.

It still returns state 4:

```text
P0: create PouchLink_Adult requested (returned 0x1)
P0 PouchLink_Adult: request ... word 0x60004 ...
dropped (state 4)
```

Switching `PhysicsRef` to the working child body's
`Player_Child_Op` physics also returns state 4. The Pouch AS root is therefore
not the blocker; the adult model (`ModelInfo/Player`) combined with
`PlayerRef`/`BSARef`/`LifeRef` is what the player manager refuses. The
`PouchPlayer_Adult` body alone creates (it has no PlayerRef/BSA/Life), but it
is a menu actor and does not draw in the world.

The remaining adult routes are a private model project (the Fmdb internal name
is still `Player_Adult`) or accepting the child `C` player-component body /
a draw-only adult body.

## Shared Life for the C body; PlayerRef state copy

`BridgeChildLife` now includes the `C` actor. The C body's Life slot starts
empty, then points at Link 1's Life object:

```text
life bridge slot 0 actor 0x214ccc9b70 components 0x214d2d6a10 life 0x0
player state bridge child 0x214ccc9b70 PlayerRef 0x214d2d6f10
    <- Link1 PlayerRef 0x214aed7868
shared life child 0x214ccc9b70 -> Link1 life 0x214aed78b8
life bridge slot 0 ... life 0x214aed78b8
```

The C body's PlayerRef also had all `0xffffffff` embedded state fields at
`+0x58`. `LICHII_SPLIT_PLAYERSTATE=1` copies Link 1's initialized `+0x58`
state bytes into the C PlayerRef and points its actor field at the C body.
The copy succeeds (log above), but registering the C holder in `manager+0x40`
still crashes with the same `0x99/0xb0/0xa0` null reads, so the manager's
missing object is not the PlayerRef component state alone.

A clean before/after shared-health sample with the C body alive is still
pending; the one-shot drain completed before the C body spawned in the test
run.

## Clean shared-Life test with the C body alive

`LICHII_SPLIT_DRAIN_WAIT_CHILD=1` holds the debug drain until the C actor
exists. The C body spawns, receives Link 1's Life pointer, and only then the
pool is drained:

```text
[103.332] P0 C spawned after 2952 ms: actor 0x214cb863c0
[103.584] life bridge slot 0 ... life 0x0
[103.584] shared life child 0x214cb863c0 -> Link1 life 0x214a5345d8
[104.580] life bridge slot 0 ... life 0x214a5345d8
[114.996] debug drain health to 12
[116.189] debug drain health to 8
[117.417] debug drain health to 4
[118.728] debug drain health to 0
[118.728] debug drain finished at 0
[119.384] shared life child 0x214cb863c0 -> Link1 life 0x214a5345d8
```

After the drain, Link 1's Life value reads 0 and the C body still points at
the same Life object. This is the shared pool working with the C body alive,
not only a pointer bridge.

## Adult bisection after the equipment finding

An adult `CoopLink_Adult` with `PlayerRef` but **no `BSARef`** and no
`EquipmentUserRef` (31 components, cutscene physics) still returns state 4.
The word-7 object exposed on one retry was an unrelated `Obj_Heart` actor, not
an adult model/AS resource. BSA is therefore not the adult blocker; the adult
`Player_Adult` model plus PlayerRef remains the refusal.

## Private adult model path: still refused

`LICHII_PRIVATE_ADULT_MODEL=1` builds a private adult actor:

```text
Model/Player_Adult2.Player_Adult2.bfres.zs   (copy of the 3.58 MB model)
Component/ModelInfo/Player_Adult2...         FmdbName/ModelProjectName Player_Adult2
ActorInfo CoopLink_Adult: FmdbName Player_Adult2, ModelProjectName Player_Adult2
```

The create still returns state 4, and the word-7 object on the failing request
is an unrelated `Obj_Heart`, not the private model. A pure model path/Fmdb
copy therefore does not break the refusal.

The results now line up with an **adult player-slot** limit rather than a
model-asset conflict:

- child `Player_Child` + PlayerRef + BSA (no equipment) creates;
- adult `Player_Adult` + PlayerRef fails with or without BSA, equipment,
  physics, AS root, or a private model path/name;
- adult `PouchPlayer_Adult` without PlayerRef creates but is a menu actor.

The engine appears to allow the second PlayerRef only on the **child player
slot**, which is free while the real player is adult. If that is the case,
adult Link 2 requires engine patching of the adult slot, not another pack.

## Adult Link 2 body via the child C slot

The adult refusal is the **actor identity/row**, not the model. Building the
`C` actor from the adult `Player_Adult` source
(`LICHII_PUPPET_SOURCE=Adult`) with `PlayerRef`+`BSARef` and no equipment
creates:

```text
P0: took actor 0x214a309af0 from request 0x2147112f98
P0 C spawned after 2720 ms: actor 0x214a309af0, state 4,
    at (134.46, 105.58, -34.00)
life bridge slot 0 ... life 0x0
life bridge slot 0 ... life 0x214a5c1648
```

The C ActorParam now has the **adult** model/AS:

```text
ModelInfoRef ?Component/ModelInfo/Player.engine__component__ModelInfo.bgyml
ASRef        ?AS/Player_Adult.root.asb
PlayerRef    ?Component/PlayerParam/Player...
BSARef       ?Component/BSAParam/Player...
```

The ActorInfo row is still `C` (`FmdbName/ModelProjectName Player_Adult`,
ELink/SLink `C`). So the second PlayerRef is accepted on the C identity/slot
with the adult model, while the `CoopLink_Adult` row is refused. The player
slot is keyed by the actor row/link identity, not by the model asset.

The split presenter remains active (`docs/img/adult-model-split.png`), and the
adult C body's Life is bridged to Link 1. The holder registration still
crashes on the manager player state; collision, independent damage, HUD,
enemy HP and the Windows kit remain.

## Adult C body with real player physics

With the C identity and the adult `Player_Adult` model, setting
`LICHII_COOPLINK_PHYSICS=Player` puts the real player physics on the body:

```text
PhysicsRef ?Component/Physics/Player.engine__component__PhysicsParam.bgyml
```

The body still creates and is shown:

```text
P0: took actor 0x214cb439c0 from request ...
P0 C spawned ... actor 0x214cb439c0, state 4, at (134.46, 105.58, -34.00)
P0 live actor 0x214cb439c0 state 4 ... (show)
```

So the adult-model C body can carry PlayerRef, BSARef, the adult model/AS and
the real player physics. The remaining collision test is to move the body and
observe the player collider; the manager holder is not required for creation.

The holder field match (`+0x28`/`+0x38` cleared, active `+0xb0` copied) still
crashes the manager player update with the same `0x99/0xb0/0xa0` null reads,
so those holder fields are not the missing player-state pointer.

## Hit record on the C body: no damage without the player state

`LICHII_SPLIT_HITCHILD=1` applies the game-native player hit record to the C
actor once it is at gameplay height (not hidden under the floor):

```text
debug hit child 0x214a7ecf48 at 134.5 105.6 -34.0
```

The C Life slot is the shared Link 1 object (`0x214af31958`), but the shared
value remains **16** after the hit. The hit record is not processed on the C
body; the same missing player-state object that breaks the manager holder also
leaves the C body's damage path inert.

So shared health is verified as a pointer/drain, but independent Link-2 damage
is not: applying a hit to Link 2 does not lower the shared pool. The remaining
route is the manager player-state clone, not another body-level write.

## Holder trace: +0x158 and the next null vtable

The player-update function at `main+0xbbeb0` reads
`[[actor+0x270]+0x158]` and calls into the player state with it. The C
component table has zero at `+0x158` (only its own PlayerRef at `+0x2b0`), so
the player bridge is essential.

With `LICHII_SPLIT_PLAYERBRIDGE=1` the C table receives 42 player components,
including `+0x158`:

```text
player bridge child 0x214a6babc0 copied 42 components mask 0x214a886e216f7d7f
```

The `0x99/0xa0/0xb0` reads from the first update path are handled diagnostics;
the fatal failure moved to `main+0x32a70f0`:

```text
Unmapped read 0x38 (8 bytes) at pc 0x83a380f0 lr 0x83a380f0
```

At `main+0x32a7180` the code does `ldr x8,[x0]; ldr x8,[x8,#0x38]; blr x8`
with `x0 = [sp+0x1e0]`. The object at `[sp+0x1e0]` has a **zero first qword**
(no vtable), so the second load faults at `0x38` and the call jumps to 0.

So the next bridged object is whatever the manager stores at `[sp+0x1e0]`;
its vtable is the remaining null. The player bridge has already supplied the
`+0x158` player-state component, so the crash is now in the manager's virtual
call on that object, not in the player component.

## Actor-state block copy: no crash, but blocks

`LICHII_SPLIT_ACTORSTATE=1` copies Link 1's actor block
`+0x108..+0x948` (0x840 bytes) into the C actor and retargets owner pointers
to C. The `[sp+0x1e0]` object is `actor+0x108`; its vtable is now non-zero.

The holder registration then succeeds without the `0x38`/vtable fault:

```text
actor state bridge child 0x214cf15368 <- Link1 0x214a405da0 (0x840 bytes)
childholder empty 0x2160fea130 <- child 0x214cf15368 holder 0x21478d72e8 id 0xba
```

But the emulator immediately reaches FPS 0 with no `ExceptionRaised`. The host
sample shows all guest CPU cores in `Core::CpuManager::IdleThreadFunction` /
`Kernel::PhysicalCore::Idle` on a condition variable. The manager update is
**blocked**, not faulting: the shared copy contains Link 1's player-state
synchronization, and sharing it deadlocks.

So the next requirement is a **private** player-state object (fresh locks and
ownership), not a second shared copy. The vtable/null-object layer is solved;
the synchronization/ownership layer is the remaining blocker for the holder,
collision and Link-2 damage.

## The shared actor-state copy deadlocks without the holder too

Running the adult C body with the player/actor-state copies but **without**
`LICHII_SPLIT_CHILDHOLDER` still deadlocks at the first bridge after the
spawn:

```text
108.166 P0 C spawned ... actor 0x214aa46f88
108.377 BridgeChildLife: split: Bridg...
```

The log stops there, the guest cores go idle, and FPS is 0. The actor-state
copy itself causes the C body's own update to enter Link 1's shared player
state and block; the manager holder is not the trigger.

So the `0x840` block is not usable as a shared copy in any form. The remaining
option is a genuinely private player-state object with fresh synchronization
and ownership, constructed rather than copied from Link 1. The shared-state
bridge path is exhausted.

## Manager links: the second slot has no player state

The player manager's link array is two slots of 0x10 bytes at `manager+0x40`.
A live probe of an unmodified run shows:

```text
slot 0: ptr 0x21613ea130, holder 0, id 0xffffffff, name ""
slot 1: ptr 0x21613ea1c0, holder 0x2147cd72e8, id 0xba,
        name "Player_Adult", actor 0x214a9f13b8, state 4
```

So the game preallocates two link objects, but the name of the active link is
the row's **ELinkUserName** (`Player_Adult`). Registering the child holder
under that name, under `Dm_Player_Child_Op`, and under the C row's own name
(`LICHII_SPLIT_LINKNAME=C`) all fail. With `C` the failure is an immediate
jump to `0x0`:

```text
childholder empty 0x21615ea130 <- child 0x214ae72cc8 holder 0x2147edd828 id 0x175
ExceptionRaised: Cannot execute instruction at unmapped address 0x00000000000000
```

The name is therefore not the missing piece.

The crashing object's chain is now traced:

- `main+0x32a70f0` is reached with `x22 = [sp+0x2b0]`, which is
  `[components+0x158]` copied at `main+0x32a5890`.
- When `x22` is zero the code jumps to `main+0x32a70f0` and dereferences a
  stale `[sp+0x1e0]`; the object there is `x23+0x108`, where `x23` is the
  return value of the virtual call on the `+0x158` component
  (`[x22] -> main+0x4b47608`, method 0 -> `main+0x2dc6f10` ->
  `main+0x2dc7060`, a singleton/type-guard accessor).
- `actor+0x108` is **zero for both Link 1 and the C body** at frame
  boundaries (sampled 40 times), so the crashing object is not an actor field.
- Link 1's `components+0x158` is a real object (`0x214b0e2948`, first qword
  `0x858bf608 = main+0x4b47608`); the C body's `+0x158` is zero until the
  player bridge copies Link 1's pointer.

The second link therefore needs a **per-player state object** that only the
game's own player-creation path constructs. A second actor, the bridge, and
the link name cannot substitute: sharing Link 1's state either faults
(zero vtable in the derived object) or deadlocks in
`KThread::DummyThreadBeginWait`. The link object is preallocated, but its
state is not.

The best working configuration remains: adult-model C body + split presenter +
controller-2 puppet + shared Life (`docs/img/link2-in-split-a.png`, `b.png`).

## Full Player_Child component set: no +0x158 state from the pack

`LICHII_COOPLINK_ALL=1` keeps every component the real player carries. With
`EquipmentUserRef` dropped, the C actor has 54 components (PlayerRef, BSARef,
LifeRef, ELinkRef, XLinkRef, EventPerformerRef, camera/lifter/AI components).
The create is still refused:

```text
coop: puppet: P0 C: request ... word 0x60004 ... (word 7 = Weapon_Shield_Hylian XLink)
coop: puppet: P0: request ... dropped (state 4)
```

Dropping `XLinkRef`, `ELinkRef` and `EventPerformerRef` too (51 components)
still returns state 4; `InstanceHeapSize = 0x400000` also returns state 4. The
equipment XLink user (`Weapon_Shield_Hylian`) reappears in the request even
without `EquipmentUserRef`, so the player/equipment engine creates it from
other player components; it is not the only blocker.

The working body remains the 32-component build (PlayerRef, BSARef, LifeRef,
actor plumbing, adult `Player` model/AS, real `Player` physics, no equipment).
Its `components+0x158` stays zero: the per-player state is created by the
player-creation path, not by an ActorParam component, so the manager link
route needs engine-level state construction.

## Enemy HP beyond Keese

`ScaleEnemyLife` walks the actor list and scales every `Enemy_*`/`Boss_*`
actor with a Life component. A spawned `Enemy_Dekubaba` is scaled:

```text
P0 Enemy_Dekubaba spawned after 6421 ms: actor 0x2149fc9530, state 4
split: enemy hp Enemy_Dekubaba 2 -> 4 (2.00x)
split: enemy hp Enemy_Dekubaba 2 -> 4 (2.00x)
```

So the 2.00x enemy HP multiplier is now verified on Keese and Dekubaba, not
only Keese.

The Windows x64 kit still needs a Windows-built `eden.exe`; this macOS host
builds an arm64 `eden-cli`, so kit packaging cannot be revalidated here.

## Both Links in the split composite; camera modes and the frustum limit

A real window capture (not the single-view debug API) shows the split-screen
two-Link result: Link 1 in the left half (hearts at the left edge), the adult
`Player_Adult` Link 2 body in the right half with Navi, both inside the
Temple of Time (`docs/img/split-both-links.png`). The presenter log is
`present: split true blit true view 1 extent 2560x1440`.

The synthetic chase rig (`LICHII_SPLIT_CAMERA_RIG=1`) rotates the primary
camera offset around Link 2. In the Temple of Time that places the eye
**through the wall**; the right half then shows the outdoor skybox with no
Link 2 (`docs/img/split-rig-wall.png`). The translation-only camera keeps the
view inside the room and is now the default (`LICHII_SPLIT_CAMERA_RIG=1` opts
in to the rig).

The translation-only view is limited by the game's CPU-side actor culling:
the scene is culled against the **primary** camera before the second camera
matrix is applied. When the controller-2 test moved Link 2 about 6.6 m away
(`133.44 105.58 -27.40`), the model was culled from view 1
(`docs/img/split-moved-cull.png`). Both Links are visible when they stay
inside the primary frustum (roughly 2-3 m at the Temple's camera distance).
Showing a far-away Link 2 needs a second scene pass or a cull override, not a
camera matrix change.

The composite HUD places hearts from view 0 at the left edge and the item
panel from view 1 at the right edge, so the full HUD is on screen; the two
edges update on alternating view frames.

## The game's own player-link binder (watchpoint discovery)

A write watchpoint on the empty link's holder field (`link+0x8`) during the
save load caught the player-entry code:

```text
watch: 2160a1d18
x0=2160a1d178 x1=214a66ea80  x9=2147ad72e8 x10=fffffffd x11=8b
pc=80d7c498 off=0x2bc498  lr=80cd1158 off=0x211158
```

So the game binds a player with `main+0x2bc440(link, actor, flags)`:

```text
main+0x2bc440:
  w20 = flags & 0xffff
  if actor == 0: return
  if link->id != actor->+0x18:
    if link->id != -1: ...
    holder = actor->+0x1b0
    state = actor->+0x2c           // must not be 7..11 (unsigned check)
    link->+0x08 = holder
    link->+0x15 = 0
    link->+0x10 = actor->+0x18
```

The caller at `main+0x211154` passes `x0 = player_slot + 0xb38`,
`x1 = actor`, `x2 = 0`. The empty link's owner slot is therefore
`link - 0xb38`.

`LICHII_SPLIT_BIND=1` queues the same call through the director's guest-call
cave (`Call{function,x0..x7}`) instead of writing the holder/id directly. The
game then binds the C actor itself:

```text
childholder bind queued link 0x21609ea130 child 0x2149f162c0 fn 0x...
link0 0x21609ea130 holder 0x21472dd7c8 id 0x174 name b'Dm_Player_Child_Op'
link1 0x21609ea1c0 holder 0x21472d72e8 id 0xba  name b'Player_Adult'
child table 0x214cb98510 state158 0x0
```

The manager update still crashes with the same `0x99/0xa0/0xb0` null reads and
the `0x0` jump: binding the link is not enough. The actor-side per-player
state (`components+0x158`) is created by the actual player **spawn** path, not
by the link binder. The next watchpoint should be on `actor+0x270` slot
`+0x158` during Link 1's spawn to catch that creator.

The full Player_Child set remains refused at 54/51/49 components
(`word 0x60004`, state 4), so the state cannot be obtained by adding pack
components.

## Spare player-state instances exist, but are not interchangeable

A heap search for the state class vtable (`main+0x4b47608`) finds **7
instances** after the save loads, while walking all 84 actors shows only **2**
are referenced by an actor's component table. So the game creates initialized
states for other objects (cutscene bodies etc.) that are not registered
players.

```text
S1 (Link 1)  0x214a8ff0b8  +0x18 = Link 1 actor  +0x100 = 0x214a914028
spare        0x2149fa48e0  +0x18 = other actor   +0x100 = 0
spare        0x214c2a2198  ...                   +0x100 = 0
... 5 more
```

`LICHII_SPLIT_ATTACH_FREE=1` scans the heap for the state vtable, skips the
states any actor references, and attaches a spare to the child's `+0x158`.
`LICHII_SPLIT_WAIT_STATE=1` defers the bind until that slot is non-zero, and
`LICHII_SPLIT_BIND=1` then calls the game's binder.

Result: the attach and bind both succeed, but the manager still crashes with
the same null-lock reads and the `0x0` jump:

```text
split: attached free player state 0x214acac900 to child 0x214d261eb8
child 0x214d261eb8 state158 0x214acac900
Unmapped read 0x99 / 0xa0 / 0xb0
Cannot execute instruction at unmapped address 0x00000000000000
```

Copying Link 1's `+0x100` object into the spare state
(`LICHII_SPLIT_SHARE_STATE100=1`) does not change it. The state must be **bound
to the link/actor** during the real player spawn; a spare state from another
object is not interchangeable, and `+0x158` alone is not the whole
per-player state.

The next watchpoint should target the **player-spawn function that creates the
state on the actor's table** (or the state's `+0x100` object), with GDB thread
selection fixed (`H<thread>` before `g`), so the creator can be called through
the existing queued guest-call cave.


## The manager updates a different actor than the attached C state

Mapping the crash LR from the attach/bind runs:

```text
attach3 base 0x80ad5000: lr 0x80b90f04 -> main+0xbbf04
attach4 base 0x8060d000: lr 0x806c8f04 -> main+0xbbf04
                         lr 0x806c9420 -> main+0xbc420
attach5 base 0x...:      lr 0x80213f40 -> main+0xbbf40
```

All are inside `main+0xbbeb0`, the player update:

```text
bbecc: ldr x8, [x0, #0x270]     ; component table of x0
bbed8: ldr x20, [x8, #0x158]    ; player state
bbee0: add x0, x20, #0x98       ; lock at state+0x98
bbf00: bl  ...                  ; reads 0x99/0xa0/0xb0 when x20 == 0
```

So the crash is `components+0x158 == 0` for the actor the manager updates. But C's
table was populated (`state158 0x214a138020`). The manager is therefore updating a
**different actor** than the C actor whose table we filled. The link name is not
the selector: `LICHII_SPLIT_LINKNAME` = `C`, `Player_Adult` and `Dm_Npc_Base`
all produce the same null-state crash.

The likely selector is the **holder**: the child holder we register is
`[child + 0x1b0]`, and the manager takes the actor from `holder + 0x40`. If that
holder's actor is not the C actor (e.g. a body/sub-holder), the manager updates
that actor and finds no `+0x158` state. The next check is to read
`holder + 0x40` for the child holder and compare it with the child actor before
binding; if they differ, register a holder whose `+0x40` is the C actor.

## Holder check: the binder points at the C actor

From the holder-diff run:

```text
childholder empty 0x21615ea130 <- child 0x214d56ac28 holder 0x2147edd7c8 id 0x174
slot0 holder +0x40 = 0x214d56ac28   (= the child actor)
slot0 holder +0x48 = 0x174
slot1 holder +0x40 = 0x214aafea08   (= Link 1)
```

So the holder registered for the child is correct: `holder + 0x40` is the C
actor, the same address the director created. The crash cannot be blamed on a
wrong holder actor.

Yet the crash LR maps to `main+0xbbf04`, where `x20 = [[x0+0x270]+0x158]` is
zero, while C's own table at that same slot reads the attached state
(`state158 0x214a138020`). Both cannot be true for the same actor/table, so the
manager is either reading a **different table** than the one the hook wrote, or
the game **clears/replaces** the slot between the attach and the update.

The next diagnostic is a register dump at the crash stop: `x0` (the actor being
updated) plus `[x0+0x270]` and `[table+0x158]`, read from the stopped thread
(`H<thread>` before `g`). If `x0` is not the C actor, the link resolution source
(not the holder) is wrong; if it is C, the slot is being cleared and the attach
must be re-applied per frame or through the component system.

## Crash stop registers: the state is present in the frame, zero at the call

The thread-selected crash stop (attach + bind run):

```text
STOP thread 9 pc 0x853f481c lr 0x803cbf04 (main+0xbbf04)
     x0=0x98  x1=0  x2=0x8123c150  x8=0
```

`x0 = state+0x98 = 0x98` confirms `state == 0` at the call. The caller frame
(`sp = 0x10aa09f220`) has its saved `x20` (state) and `x19` (actor) both zero,
and its saved return address is `main+0xbbcb4`.

But the same thread's stack also contains:

```text
0x21609ea130  the empty link
0x214938380   the C actor (0x214c938380)
0x214bf31660  the attached player state
```

So the manager thread was processing our link and the attached state was in
scope, yet the update call re-read `[actor+0x270]+0x158` as zero. The component
table slot is therefore **transient/rebuilt by the game**: a host-injected
pointer does not survive to the player update.

This explains why the full ActorParam set (which would create the state via
the component system) is the only durable route, and why attach/bind/spare
states all crash. The next step is a **minimal ActorParam addition**: start
from the working 32-component body and add groups of the missing player
components until `+0x158` appears after creation, then narrow to the one
component that instantiates this state class (without the equipment/blocker
components that make the full set return state 4).

## Minimal additions: SLinkRef is blocked by the item links

Adding only `SLinkRef` to the working 32-component body (33 components,
`?Component/SLink/Player_Adult...`) still returns state 4. The request's word-7
object is another equipment/item singleton:

```text
0x214d363000 +0x20 = "Item_Bomb"
```

So `SLink` pulls the player's save/item links (bomb bag etc.), the same class
of singleton that blocked `EquipmentUserRef` and `XLinkRef`. `SLinkRef` cannot
be added alone, and it would need a private item/save link to be usable.

A type-registry probe found the PlayerRef type object (`main+0x4974a90`)
referenced once at `main+0x...` with slot-index 86 matching PlayerRef's
`+0x2b0` table entry, but the surrounding region is a mixed table (code
pointers, strings) and entry 43 is `0xffffffff`. The slot-index registry
hypothesis is unconfirmed.

This leaves the `+0x158` component either an item/save-link component like
`SLink` (needing private singletons) or a player-spawn-only object. Adding
further player components one at a time is the remaining pack-level route; the
working build is restored to the 32-component adult C body.

## Minimal component bisection: every player-state component hits the inventory

Results from adding candidates to the working 32-component body:

| added components | count | result |
|---|---|---|
| `PlayerCameraTargetRef`, `PlayerLifterRef`, `ActiveCameraAlphaRef`, `BgmAnimationSyncInfoRef`, `GameParameterTableRef` | 37 | state 4 |
| `PlayerCameraTargetRef`, `ActiveCameraAlphaRef`, `BgmAnimationSyncInfoRef` | 35 | state 4; word-7 `Item_Bombchu` |
| `PlayerLifterRef` alone | 33 | state 4 |
| `SLinkRef` alone | 33 | state 4; word-7 `Item_Bomb` |

Every candidate that touches the player state also initializes the player's
**inventory/item links**, and the create is refused on the first item
(`Item_Bomb`, `Item_Bombchu`, `Weapon_Shield_Hylian`). This is the same
singleton that blocks `EquipmentUserRef` and `XLinkRef`.

So the missing `components+0x158` state and the state-4 refusal have one root:
the player state is bound to the player's **save/inventory singleton**, and a
second player needs a private player save/inventory. Further one-by-one
component additions will keep returning state 4 for the same reason; the
pack-level bisection is exhausted.

The working 32-component adult C body is restored (no `PlayerLifterRef`, no
equipment). The remaining route is an engine-level private player
save/inventory + state construction, or shipping the current co-op build with
documented damage/collision limitations.

## Collision probe: the C body does not resolve floor penetration

Added two runtime probes:

- `LICHII_SPLIT_OFFSET_Y=-1.0` places the debug puppet 1 m below Link 1's feet;
- `LICHII_SPLIT_HOLD_MS=2000` stops posing the puppet after the delay so its
  own physics can show a response.

Run (32-component adult C body, real `Player` physics):

```text
link1 pos (132.461, 105.581, -34.0)
sample 0..10  (134.461, -394.419, -34.0)   the director parks a new puppet under the floor
sample 11..13 (134.461, 104.581, -34.0)    posed at Link 1's ground minus 1.0 m
final child   (134.461, 104.581, -34.0)
```

The body stays 1.0 m inside the floor with no upward correction over several
seconds. So the C actor's real physics does **not** resolve penetration like a
player body: player collision is not active without the player state. This
matches the inert damage path; both need the same missing state.

The probe knobs are env-gated and default off.

## Binder + shared state: still crashes

The last untested manager-link combination was the game's own binder
(`main+0x2bc440` via `LICHII_SPLIT_BIND=1`) with the player bridge filling the
child's `components+0x158` with Link 1's state:

```text
child 0x2149e79a48 table 0x214c9fed50 state158 0x214a5a1148
childholder bind queued
fps 0.0
crash true
state now 0x214a5a1148
```

The binding succeeds and the state slot stays populated, but the manager
update still crashes. Combined with the earlier results (raw holder + shared
state, game binder + spare state, full component sets refused), this exhausts
every non-engine route to the second link:

| manager-link attempt | result |
|---|---|
| raw holder write + shared state | crash in manager state |
| game binder + spare game state | crash (null lock) |
| game binder + shared Link 1 state | crash (this run) |
| full/minimal ActorParam additions | create refused (state 4) |

The game has no second-player creation path; a functional Link 2 requires an
engine-level implementation of a private player state (and its save/inventory
bindings), not a pack or hook configuration. The working co-op build remains
the 32-component adult C body with split rendering, shared Life and the game's
own Game Over/Continue.
