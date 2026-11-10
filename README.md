# Split Green Hat

[![Windows x64](https://github.com/fabioavelino/split-green-hat/actions/workflows/windows-x64.yml/badge.svg)](https://github.com/fabioavelino/split-green-hat/actions/workflows/windows-x64.yml)

Two real Links in one running copy of *The Legend of Zelda: Ocarina of Time* (Nintendo Switch 2), with split-screen, shared inventory and shared health.

This is not network co-op and it does not run two emulator instances. The split module lives inside Fritchii and drives a second Link actor, a second camera and a second viewport in the same game world.

**Status:** M1a — the Windows x64 workflow builds a kit that patches the game and spawns a debug Link puppet beside player 1. The puppet is not controllable yet; M1b adds the real `SplitLink` actor and controller 2. See [PLAN.md](PLAN.md) and [docs/m1a-debug-puppet.md](docs/m1a-debug-puppet.md).

## What it will do

- Two Links, controlled by two gamepads.
- Horizontal or vertical split-screen.
- One shared inventory, one shared health pool.
- One cutscene, full-screen, owned by the first player who enters it.
- One game-over screen when either Link dies; Continue restarts both.
- Enemy HP multiplier, default 2×, configurable 1×–4×.
- Windows x64 portable build.

## What it will not do

- It will not include the game, keys, firmware or saves. You must own and dump your own copy.
- It will not turn Link into Zelda. The game's playable actor is Link; the two players are two Links.
- It will not be a network mod.

## Testing M1a

The current kit performs the M1a experiment: it patches the game, loads a
puppet pack and spawns a second Link beside player 1. The puppet is not
controllable yet.

1. Download the `split-green-hat-windows-x64` artifact from the latest
   successful [Windows x64 run](https://github.com/fabioavelino/split-green-hat/actions/workflows/windows-x64.yml).
2. Extract it.
3. Run `make-puppet-pack.cmd` and give it your own `.nup`. This builds the
   LayeredFS pack under `user\load\0400486024DEC000\split-puppet`.
4. Run `lichii.exe`, load the game and wait for the scene.
5. About three seconds later, a second Link should appear about 2 m to
   Link's right.

Send `user\log\eden_log.txt` and a screenshot. The exact test checklist is in
[docs/m1a-debug-puppet.md](docs/m1a-debug-puppet.md).

## Building

The emulator is Windows x64, MSVC, Qt 6.8.3 and Vulkan. The GitHub Actions workflow builds it on a Windows runner:

```
.github/workflows/windows-x64.yml
```

For a local Windows build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
```

The script fetches Fritchii at the commit in `lichii/PIN`, applies `lichii/patches`, configures with `LICHII_SPLIT=ON`, builds `lichii.exe`, and packages the portable folder.

## Repository layout

| Path | What |
|---|---|
| `PLAN.md` | The full design, decisions, milestones, risks and test plan |
| `split/` | Settings, viewport math and the split library |
| `split/hook/` | Code built into the emulator: frame hook, second-player manager, renderer, Qt panel |
| `lichii/patches/` | Small patch series applied to `oot-gather-fix` |
| `lichii/PIN` | The exact Fritchii tag and commit |
| `tests/` | Unit tests for settings and viewport layout |
| `scripts/` | Windows fetch, build and packaging scripts |

## Credits and licence

GPL-3.0. Built on:

- **Fritchii** by Frizti and the Lichii/Eglantine/Eden/yuzu/Sudachi contributors.
- **oot-remake-multiplayer** by daurosk and contributors for the co-op research, memory readers, actor creation and camera tracking.
- The Ocarina of Time decompilation and reverse-engineering community.

No game files, keys, firmware or saves are distributed. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
