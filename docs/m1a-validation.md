# M1a validation log

Date: 2026-11-10

## Windows x64 build

GitHub Actions run
[42840084413](https://github.com/fabioavelino/split-green-hat/actions/runs/42840084413)
completed successfully:

- Unit tests: passed.
- Fritchii fetch and patch series `0001-split-hook` + `0002-coop-svc`: passed.
- Co-op submodule checkout: passed.
- MSVC, Qt 6.8.3, Vulkan SDK, sccache: installed.
- Configure with `LICHII_SPLIT=ON`: passed.
- Build `yuzu`: passed.
- Puppet pack tool, `split.ini`, notices and kit scripts packaged: passed.
- Artifact `split-green-hat-windows-x64`: uploaded, 90,909,936 bytes.

The artifact is the M1a test kit: it patches the game with the co-op code
cave, loads the debug-puppet pack from the user's own `.nup`, and asks the
co-op actor director to create a second Link beside player 1.

## Local checks

- `0001-split-hook.patch` and `0002-coop-svc.patch` apply cleanly in order to
  Fritchii `fritchii-v5.6`.
- The split hook and the co-op `coop_svc`, `coop_puppet`, `coop_pose` and
  `coop_camera` translation units compile against Fritchii on macOS.
- The settings and viewport unit tests pass.

The full emulator link and the in-game actor spawn were not run on macOS;
the Windows kit is the first in-game test.
