# Enemy HP multiplier

`split.ini` holds `enemy_hp` (1.0 to 4.0, default 2.0). The split hook walks
the game's actor list every 500 ms and scales each `Enemy_*` or `Boss_*`
actor once, the first time it is seen in the current scene.

The life component is found the way the co-op object reader finds it:

```
components = [actor + 0x270]
life       = [components + 0x2b8]
value      = s32 at [life + 0x1c10]
max        = s32 at [life + 0x1c18]
```

Both are multiplied by the factor with `lround`, so `2.0` makes a 4-heart
enemy 8 hearts. The scaled addresses are remembered per scene and cleared on
a scene change, so a respawned enemy is scaled again. `1.0` disables the
walk entirely.

Breakables (`Obj_*`), animals and NPCs are not touched, so pots, signs and
cuccos keep their normal hit counts.
