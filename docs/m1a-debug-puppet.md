# M1a — debug puppet test

M1a proves the hardest infrastructure before the real `SplitLink` actor exists:

- the game's main module is patched with the co-op code cave;
- the emulator receives `svc #0xf4`;
- the actor-creation path runs;
- the LayeredFS puppet pack loads;
- a second Link actor appears in the world beside player 1.

It is **not controllable yet**. It is a visual second Link that appears about
three seconds after the scene is known. Controller 2 support comes in M1b,
together with the real `SplitLink` actor pack.

## Build the puppet pack

The puppet pack contains files derived from your own game, so it is built on
your PC and is never included in the kit.

1. Extract the kit.
2. Run `make-puppet-pack.cmd` (or `make-puppet-pack.ps1`).
3. Give it the path to your `.nup` when asked.

It writes:

```
user\load\0400486024DEC000\split-puppet\romfs\...
```

Python 3 and the `zstandard` package are required; the script installs
`zstandard` if it is missing.

## Run

1. Start `lichii.exe`.
2. Load the game and a save.
3. Wait for the scene to load and stand still for about three seconds.
4. A second Link should appear about 2 metres to Link's right.

## What to send back

- `user\log\eden_log.txt`
- a screenshot showing the second Link, or the game without it

Look for these lines:

```
split: loaded ...
split: on, settings ...
split: M1 debug puppet enabled, 3000 ms after the scene is known
split: M1 game build e0f3ccfaf50065ae, main at ...
split: puppets on, actor by age
split: puppet ... (or a create/delete line)
```

If the actor does not appear, the log will say why: a patch that did not
match, a missing pack, a failed create or a delete. The most useful failure
lines contain `Skipping "coop puppet"`, `waiting for the game to draw`,
`create failed` or `pack`.
