# M0 validation log

Date: 2026-11-10

## Unit tests

```
cmake -S . -B build -DSPLIT_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
```

Result: 1/1 test passed (`split_tests`).

## Fritchii patch

Base: `fritchii-v5.6`, commit `696a456641ae83c9f8b6564c57ac311c7ef56799`.

```
git apply --check lichii/patches/0001-split-hook.patch
```

Result: patch applies cleanly.

## Emulator configure

A macOS check build was configured from a clean worktree with the patch applied:

```
cmake -S . -B build-check -G Ninja \
  -DENABLE_QT=OFF -DYUZU_CMD=ON \
  -DENABLE_LIBUSB=OFF \
  -DVulkanHeaders_FORCE_BUNDLED=ON \
  -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
  -DLICHII_SPLIT=ON \
  -DLICHII_SPLIT_DIR=<split-green-hat> \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=<split-green-hat>/cmake/trim_paths.cmake
```

Result: configure succeeded, including `add_subdirectory` of this repository and
`split/hook/core.cmake`.

## Hook compile

`split/hook/split_hook.cpp` compiled as part of the emulator `core` target.

## Qt window compile

A second configure with `ENABLE_QT=ON` and the Homebrew Qt succeeded. The exact
compile command for `split/hook/split_window.cpp` was extracted with
`ninja -t commands` and run directly; it compiled successfully.

The full emulator link was not attempted on macOS in M0. The Windows workflow
is the authoritative build.

## Windows x64 build

GitHub Actions run
[42826223352](https://github.com/fabioavelino/split-green-hat/actions/runs/42826223352)
completed successfully:

- Unit tests: passed.
- Fetch Fritchii and apply `0001-split-hook.patch`: passed.
- MSVC, Qt 6.8.3, Vulkan SDK, sccache: installed.
- Configure with `LICHII_SPLIT=ON`: passed.
- Build `yuzu`: passed.
- `windeployqt` and portable kit packaging: passed.
- Artifact `split-green-hat-windows-x64`: uploaded, 90,874,378 bytes.

The artifact is available on the run page. It contains the patched
`lichii.exe`, the Qt runtime, `user/config/split.ini`, and the project notices.

