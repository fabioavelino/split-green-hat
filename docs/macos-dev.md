# Running the split build on macOS

This is the development smoke-test path. On a 16 GB M1 Air the game runs at
about 1–2 fps; a Windows PC with a discrete GPU is the real target.

## Build

The persistent Fritchii checkout used for the M2 tests is
`/Users/fabio/Downloads/zelda/Fritchii/fritchii-src` — a worktree at the
`fritchii-v5.6` pin with the patches in `lichii/patches` applied. The build
directory is
`/Users/fabio/Downloads/zelda/split-green-hat/fritchii-build`.

Configure once:

```sh
cmake -S /Users/fabio/Downloads/zelda/Fritchii/fritchii-src \
  -B /Users/fabio/Downloads/zelda/split-green-hat/fritchii-build \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=OFF \
  -DLICHII_SPLIT=ON \
  -DLICHII_SPLIT_DIR=/Users/fabio/Downloads/zelda/split-green-hat \
  -DVulkanHeaders_FORCE_BUNDLED=ON \
  -DVulkanUtilityLibraries_FORCE_BUNDLED=ON
```

Rebuild the command-line emulator with:

```sh
cd /Users/fabio/Downloads/zelda/split-green-hat/fritchii-build
PATH=/Users/fabio/.local/share/fritchii-build-shims:$PATH ninja yuzu-cmd -j 8
```

The shim directory contains `libtoolize -> /opt/homebrew/bin/glibtoolize`,
which the bundled libusb requires. The binary is `bin/eden-cli`.

Current Xcode/libc++ also needs one local compatibility fix in the pinned
source before the build: `src/audio_core/sink/input_processing.h` uses
`std::min(u64, size_t)`; change it to
`std::min<std::size_t>(static_cast<std::size_t>(...), ...)`.

## MoltenVK

```sh
MOLTEN=/Users/fabio/Downloads/zelda/Fritchii/fritchii-src/.cache/cpm/moltenvk/v1.4.1-ryujinx/MoltenVK/dynamic/dylib/macOS
export VK_ICD_FILENAMES="$MOLTEN/MoltenVK_icd.json"
export VK_DRIVER_FILES="$MOLTEN/MoltenVK_icd.json"
export DYLD_LIBRARY_PATH="$MOLTEN"
```

## Split settings

`~/.config/eden/split.ini` is read at startup:

```ini
enabled = 1
layout = vertical
enemy_hp = 2.0
player2_controller = 2
cutscene_mode = first
game_over = shared
```

`layout = vertical` is the left/right split; use `horizontal` for top/bottom.
Invalid keys or values are reported in the log and the default is kept.

## Run

```sh
cd /Users/fabio/Downloads/zelda/split-green-hat/fritchii-build
./bin/eden-cli -u 0 \
  -g "/Users/fabio/Downloads/zelda/The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup"
```

The CLI frontend has no split panel, so `SplitHook::Start()` is called from
`PatchManager::PatchNSO` before the co-op NSO patch (patch 0002). Start has to
run before `CoopSvc::PatchMain`: it sets the co-op environment that decides
whether the cave is installed.

### GDB stub

The CLI reads `~/.config/eden/sdl2-config.ini`. If

```ini
use_gdbstub=true
```

is left over from a debug run, every launch suspends the guest until a
debugger connects on port 6543. The symptom is FPS 0 with every guest core
idle and no further log lines after startup. Keep it false for normal runs:

```ini
use_gdbstub=false
```

(`qt-config.ini` has the same key for the Qt frontend.)

Split-screen is on by default. Environment switches:

- `LICHII_SPLIT_SCREEN=0` — single-camera two-Link mode.
- `LICHII_SPLIT_CAMERA_RIG=0` — translation-only second camera (debug).
- `LICHII_SPLIT_VIEW2=1` — full-screen player 2 camera (debug).
- `LICHII_SPLIT_TEST_SHIFT=<units>` — fixed world-x camera shift (debug).

## Debug input

The emulator's debug server is enabled with:

```sh
EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545 EDEN_NX2_DEBUG_DIR=/tmp/eden-debug
```

Load a save with single A presses (from the Fritchii checkout):

```sh
python3 -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
  --call '{"method":"input","params":{"buttons":["A"],"frames":2,"timeout_s":40}}'
```

Use `frames:2`. Holding a sequence for longer times out on the slow Mac.

## Expected log lines

```text
split: loaded ... layout vertical ...
split: screen split on, layout vertical
[splitcam] publish ...
[splitcam] rig[0] eye=(...) fwd=(...) link2=(...) yaw=...
[splitcam] pre[0] addr=... base=(...) col3=(...) read=(...)
present: split true blit true view 1 extent 2560x1440
```

`present: split false` appears during cutscenes and loading; player 1 owns the
full screen then.

## What to expect

- The left half is player 1's camera; the right half is player 2's chase
  camera with Link 2 visible.
- The HUD is part of the game frame, so its left half comes from view 0 and
  its right half from view 1 (known limitation).
- Each half updates every other presented frame.

## Stop

```sh
pkill -9 -f eden-cli
```
