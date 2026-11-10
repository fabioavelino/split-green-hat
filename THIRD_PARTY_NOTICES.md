# Third-party notices

This project is licensed under GPL-3.0. It builds on and reuses work from:

## Fritchii / Lichii / Eglantine / Eden / yuzu / Sudachi

- Fritchii: https://github.com/Frizti/Fritchii
- Lichii: https://github.com/zortos293/Lichii
- Eglantine: https://github.com/Djihads80/Eglantine
- Eden: https://git.eden-emu.dev/eden-emu/eden
- yuzu: https://github.com/yuzu-emu/yuzu
- Sudachi: https://github.com/sudachi-emu/sudachi

GPL-3.0. The emulator source is fetched at build time from the pinned Fritchii commit; it is not vendored in this repository.

## oot-remake-multiplayer

- https://github.com/daurosk/oot-remake-multiplayer

GPL-3.0. Used as reference and, where noted, as a source of memory-reading, actor-creation and camera-tracking code for the Ocarina of Time remake. Its copyright notices and licence are retained.

## Qt

Qt 6.8.3, used by the emulator frontend under the GPLv3 option. See the Qt licence files shipped with the emulator build.

## Other dependencies

The emulator fetches its own third-party dependencies at configure time (SDL3, FFmpeg, OpenSSL, ENet, Vulkan-Headers, etc.). Their licences are retained in the build tree and in the portable package.

## Game content

No game files, keys, firmware, saves or decrypted assets are included in this repository or in the released build. The user supplies their own legally obtained `.nup`.
