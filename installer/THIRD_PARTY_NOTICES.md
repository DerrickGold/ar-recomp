# ActRaiser Builder and distribution notices

## Embedded retail resources

The manual, cover art, and title artwork below `internal/builder/assets` are
retail media distributed for project use. They are not relicensed under the
project's MIT license. Locally extracted ROM content and generated preview
caches likewise remain subject to the original game's copyright.

## SDL3_ttf and bundled fonts

Standalone macOS and Windows distributions include the official SDL3_ttf
redistributable alongside SDL3. Steam Deck uses Valve's corresponding Steam
Runtime library. Generic Linux bundles pinned SDL3 development/runtime packages
from Valve and SDL3_ttf packages from Valve (x86_64) or Debian (ARM64), without
installing those packages into the operating system. Publisher copyright
notices are retained under `utils/licenses/SDL3` and `utils/licenses/SDL3_ttf`.
The exact resolved versions, package URLs and SHA-256 checksums are in the
distribution's `utils/licenses/sdl-sdk.lock.json`. The selected
SDK license and upstream FreeType, HarfBuzz, PlutoSVG, and PlutoVG notices are
retained under `utils/licenses/SDL3_ttf` in distributions. Source provenance is
recorded in `packaging/licenses/sdl-ttf/README.md`.

The bundled Noto font files retain their SIL Open Font License files beside the
fonts under `game-assets/fonts/noto`. Provenance and hashes are recorded in that
directory's README. Retail fonts and scripts are not distributed.

## SheenBidi

Enhanced text uses unmodified SheenBidi 3.0.0 for Unicode bidirectional
resolution and script itemization, Copyright (C) 2014–2026 Muhammad Tayyab
Akram, under the Apache License 2.0. Its source and license ship under
`utils/third_party/sheenbidi` and `utils/licenses/SheenBidi`.
