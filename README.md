# ActRaiser Recomp

**A native port of ActRaiser (SNES, USA release) that recompiles your own
cartridge dump into a standalone executable.**

![ActRaiser Recompiled title screen with the optional high-resolution logo](/assets/title-hd.png)

The 1990 SNES game from Quintet and Enix alternates between side-scrolling
action stages and a top-down town simulation. This project targets the USA
cartridge dump and converts its 65816 machine code into C, which is then linked
with a hand-written SDL3 runtime. Running the game as native code allows
widescreen rendering, layered 3D action stages, height-mapped towns, GPU and
CRT effects, replacement art and music, rebindable controls, save states, and
an in-game settings menu and manual.

**[Quick start](#quick-start)** · **[Features](#features)** ·
**[Manual](docs/manual.md)** · **[Game documentation](docs/README.md)** ·
**[Status](#progress-at-a-glance)** · **[Development](#development)**

> **For visitors from Japan / 日本語でご覧の皆さまへ**
>
> Thank you for taking an interest in the project and for sharing it with
> others. The current build is based on the USA release, which differs from the
> Japanese version in many ways beyond the language itself. I am working to
> bring the Japanese version's regional features and Japanese-language support
> to a future release.
>
> 本プロジェクトに関心を寄せていただき、ありがとうございます。また、本作を広めてくださった皆さまにも心より御礼申し上げます。現在のビルドは北米版をベースとしており、日本版とは言語以外にも多くの違いがあります。将来のリリースに向けて、日本版独自の要素への対応と日本語化を進めています。

---

## Progress at a glance

Development is active, and bugs remain.

| | |
|---|---|
| ✅ | **Action stages:** All USA action routes across the six regions and Death Heim have been completed end to end. |
| 🟡 | **Simulation mode:** Event coverage is confirmed in Fillmore, Bloodpool, Kasandora, Aitos, and Marahna; Northwall remains to be validated. |
| 🟡 | **Diorama mode:** Every action route except Northwall has been play-tested, with further room-by-room refinement planned. |
| 🟡 | **Platforms:** macOS arm64 and Steam Deck are confirmed. macOS x86_64, generic Linux, and Windows still need representative launch testing. |
| 🟡 | **Localization and regional support:** Multilingual text and optional Japanese-version mechanics are under investigation and early development. |

See [`docs/progress.md`](docs/progress.md) for stage, town, subsystem, and
platform details.

---

## AI disclosure

Multiple AI coding assistants have contributed to the tooling, runtime,
debugging infrastructure, and documentation under my direction and review.

Game logic is mechanically translated from the original binary and is never
committed to this repository. The widescreen renderer, 3D presentation,
settings system, and other host-side features are original work.

### Reverse-engineering notes

The MIT-licensed [research documents](docs/README.md) describe the original
game rather than this implementation. They contain no ROM-derived code and may
also be useful to an independent decompilation effort.

---

## Quick start

The downloadable builder needs only your ROM. It includes the compiler and
other build dependencies, so there is no need to install CMake, SDL, Go, or
clone the repository.

### What you need

Provide your own legally obtained USA cartridge dump. The project does not
include a ROM, and the builder verifies the file against these values before
continuing:

| | |
|---|---|
| Internal title | `ACTRAISER-USA` |
| Size | 1,048,576 bytes (1MB, no copier header) |
| Internal checksum | `0x83DB` |
| SHA-256 | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| SHA-1 | `e8365852cc20178d42c93cd188a7ae9af45369d7` |
| CRC32 | `0xEAC3358D` |

### 1. Download the builder for your platform

Download the archive for your machine from
**[Releases](https://github.com/DerrickGold/ar-recomp/releases)** and unpack it
anywhere.

| Platform | Bundle |
|---|---|
| macOS (Apple Silicon / Intel) | `actraiser-recomp-macos-arm64.tar.xz` / `-macos-x86_64.tar.xz` |
| Windows (x64 / ARM64) | `actraiser-recomp-windows-x86_64.zip` / `-windows-arm64.zip` |
| Linux (x64 / ARM64) | `actraiser-recomp-linux-x86_64.tar.xz` / `-linux-arm64.tar.xz` |
| Steam Deck | `actraiser-recomp-steam-deck-x86_64.tar.xz` |

Each bundle contains the buildable project, a pinned Zig C/C++ toolchain, and
SDL3 where a redistributable is available. A SHA-256 sidecar is included for
each archive.

The macOS arm64 and Steam Deck bundles have been tested end to end. The macOS
x86_64, Windows, and generic Linux archives are cross-built from the same
project but still need representative launch testing. Reports from those
platforms are welcome whether the build succeeds or fails.

![An unpacked bundle folder: a run-build script, a README, and a utils folder](/assets/builder-run-script.png)

### 2. Run `run-build` and pick your ROM

Double-click `run-build.command` on macOS or `run-build.bat` on Windows. On
Linux, run `./run-build.sh`. The script opens a private builder in your browser,
served only on the loopback interface and protected by a per-process token.
Choose your ROM and press **Build game**.

The builder generates and compiles the game's C code locally. Its page
communicates only with `127.0.0.1`, so the ROM never leaves your machine. The
initial build usually takes a few minutes.

![The local builder in three stages: the ROM picker with a Build game button; the build running with a step list and a progress dock at 38%; and the finished build showing the original instruction manual with a Launch game button](/assets/builder-stages.webp)

The **Manual** tab includes the original 40-page instruction booklet, which can
be read while the build runs.

### 3. Play

When the build finishes, press **Play**. The builder also creates a `run-game`
launcher in the bundle folder; use that for later sessions without rebuilding.
Running `run-build` again detects the existing game and opens directly as a
launcher.

### 4. Upgrading later

To upgrade, extract the newer bundle over the existing folder. Settings, saves,
authored diorama rooms, and asset entries remain in place because shipped
defaults are stored separately under `utils/defaults/`. On the next launch,
new settings are added without replacing values you have changed.

### 5. Optionally, reclaim the space

Most of the bundle's size comes from the toolchain. After a successful build,
the builder can remove it and reports how much space will be recovered. The
game, settings, saves, music, and graphics remain in place, and `run-build`
continues to work as a launcher. Download the bundle again if you later need
to rebuild.

<details>
<summary>Building and testing from a source checkout instead</summary>

For a source build, place the ROM at `ar.sfc` and run `make dev`, or pass a
different path with `ROM=/path/to/dump.sfc`. The build requires Go 1.24+,
CMake 3.16+, C and C++ compilers, and SDL3 3.4+. On a fresh checkout, this
command generates the ROM-derived C, creates a user-owned `config.ini` from the
stock template if one is not already present, and builds the optimized `play`
preset in `build-release/`. Generated game code stays local and is not
committed.

For the usual edit-and-build loop, run `cmake --build --preset play`. Configure
and build the Debug test tier with `cmake --preset dev` followed by
`cmake --build --preset dev`, then run it with `ctest --preset dev`. The Go
tooling tests use `go -C snesrecomp-go test ./...`. Additional checks are
available through `make check-constants` for high-risk authored constants and
`make check-cross` for a Windows x86_64 compile-and-link test using the pinned
Zig and SDL toolchain.

</details>

---

## Features

On a fresh install, the game uses 4:3 geometry and the original music and
artwork, with the 3D and CRT presentation modes disabled. You can configure the
enhancements from the in-game settings overlay (`Esc`, `F1`, or L3 on a pad)
and compare them with the authentic rendering and audio at any time.

### Widescreen

Widescreen supports true 16:9 and 16:10 presentation by streaming background
layers into the wider viewport and activating sprites beyond the original
screen bounds. This preserves the game's HDMA and parallax effects instead of
stretching or cropping the original view. Press `F9` to cycle among authentic
4:3, widescreen raw, and widescreen full, which places the HUD in an
independently scaled overlay.

Every standard action stage across regions 1–6, along with all of Death Heim,
has been played through and validated in widescreen.

![Bloodpool Act 2 in authentic 4:3 above the same scene in 16:9, with the background extending symmetrically into the extra width](/assets/widescreen-comparison.png)

### Diorama 3D for action stages

The renderer captures the PPU's background layers, sprite plane, and HUD
separately each frame, then places them on distinct planes in 3D space. BG2
sits behind BG1, sprites stand between them, and depth shading falls off with
distance to give the stage the appearance of a physical diorama.

Rooms can be tuned independently:

- **Layer layout:** Each room defines its own depths and can use a stable
  backdrop from the ROM when the live PPU view is too narrow.
- **Camera modes:** Free Cam supports manual orbit and zoom, while Dynamic Cam
  leans and reacts around an authored pose.
- **Framing:** Vertical extension reveals more of the stage above and below the
  original 224 lines. Skybox and Shoebox walls enclose finite backdrops.

![The diorama camera orbiting a Fillmore stage, layers moving against each other in depth](/assets/diorama.gif)

![Fillmore Act 1 side by side: flat 2D on the left, the same frame tilted into separated 3D planes on the right](/assets/diorama-comparison.png)

### 3D simulation towns

Simulation mode rebuilds each town as an oblique 3D scene:

- **Terrain:** A height field raises hills, plateaus, and cliff faces across
  all six towns. A landscape slider can scale the relief back to a flat plane.
- **Scenery:** Region-aware voxel models replace structures and foliage at
  selectable quality levels. Stone bridges span the banks, while mountains
  and volcanoes use camera-aligned relief.
- **Actors and depth:** People and effects remain billboards grounded against
  the terrain, and flying actors maintain a stable altitude above it. Ridges
  and buildings can occlude objects behind them.
- **Lighting and interaction:** Objects cast terrain-following shadows, with
  optional soft blur and rim lighting. The surrounding world map continues
  beyond the town, and building placement and miracle targeting remain in the
  tilted view.

![Fillmore projected onto a 3D ground plane, trees and structures standing as billboards while the camera moves](/assets/3dtown.gif)

![Aitos rebuilt with elevated terrain, voxel buildings, trees and mountain relief, with the authentic town view inset for comparison](/assets/sim3d-detail.png)

![Zooming out from Fillmore until the whole landmass and its surrounding clouds are visible](/assets/3dtown-zoom.gif)

### 3D world navigation

While the Sky Palace travels between towns, the game renders the full world map
on a single ground plane. Directional shadows move across the terrain, and the
Palace descends through world-anchored cloud banks as it approaches a town.

![Approaching Bloodpool across the full world map, terrain on one ground plane with cloud banks at the edges](/assets/worldnav-3d.png)

### GPU and CRT effects

The SDL GPU renderer adds rim lighting to diorama sprite silhouettes, depth of
field between layers, and edge anti-aliasing to tilted planes. Particles and
local illumination extend the original spell and environmental effects in both
flat and 3D action-stage views. Soft shadow blur is disabled by default because
it has a known transparency-bleed issue.

An optional final CRT pass applies to every presentation mode and provides live
controls for glass curvature, scanline depth, phosphor mask, colour fringing,
signal bandwidth, corner falloff, and brightness. Optional frame interpolation
uses consecutive action-layer captures for smoother output on high-refresh
displays without changing the game's 60 Hz logic.

![A diorama scene with GPU effects off on the left and rim lighting plus depth of field on the right](/assets/shader-comparison.png)

### High-resolution Mode 7

Mode 7 rendering runs at the internal render scale rather than 256×224, so its
rotation, zoom, and per-scanline warps stay sharp instead of magnifying the
original low-resolution sampling.

![Mode 7 rendering at increased internal resolution](/assets/mode7.png)

### Asset and music replacements

Art replacement uses explicit renderer hooks rather than a generic
texture-pack system. The stock manifest currently supports the title logo in
two contexts: the static title screen scales the replacement to the viewport,
while the animated intro carries it through the original Mode 7 matrix so its
rotation, zoom, and HDMA warps still apply.

![The title screen twice: the ROM's original logo on the left, a high-resolution replacement in its place on the right](/assets/hd-title-comparison.png)

Art and music replacements share the user-owned `game-assets/manifest.ini`,
which is preserved across upgrades. The builder's **Assets** tab provides:

- previews of the included HD title art and all 17 entries in the ROM's song
  table, including unnamed tracks identified by slot;
- extracted previews of the original audio; and
- controls for installing or restoring replacements without removing
  hand-authored gain, loop, or gate settings from the manifest.

![The local builder's Assets tab with the HD title toggle, original-audio extraction, side-by-side audio previews, and pending Save and Discard controls](/assets/assets-1.png)

![The Assets tab's Split by level editor, with per-region variants and replacement previews for a shared action track](/assets/assets-2.png)

Music replacement supports:

- OGG Vorbis files in place of the SPC driver's songs;
- sample-accurate looping through `LOOPSTART`/`LOOPLENGTH` tags or manifest
  keys; and
- variants selected by game state. When the ROM shares a song across several
  levels, **Split by level** gives each selected region and act its own OGG
  file. The unsplit entry remains the fallback everywhere else.

The SPC driver continues to handle sound effects, while only the instrument
voices for a replaced song are muted. You can switch between the replacement
and the original sequencer while a song is playing.

### Live authentic comparison

Bind **Compare rendering** in **Settings → Controls** to switch between the
current enhanced presentation and the ROM's native 256×224 graphics and SPC
audio without changing any saved settings:

- **Short press:** Toggle the enhanced and authentic base views.
- **Long press:** Open a persistent picture-in-picture view with the enhanced
  presentation as the main image and the authentic game inset. Hold the control
  again to close PiP without switching the base view, or press it briefly to
  close PiP and switch views.

The comparison state is shared by action and simulation modes and resets to
the enhanced view each time the game launches. Gameplay pauses during the
brief transition, but quality-of-life options and cheats remain active in both
views. The CRT pass follows the current Video settings and can be used with
either view.

![An enhanced widescreen action stage with CRT styling, with the authentic 4:3 renderer running in a framed picture-in-picture inset](/assets/picture-in-picture.png)

### Settings overlay

The in-game menu uses ActRaiser's own 2bpp dialog font and the Sky Palace dialog
frame decoded from the ROM at startup. Its nine sections cover Video, Action
3D, Town 3D, Audio, Controls, Cheats, Save, Manual, and System, and remain
accessible from any game state by keyboard or gamepad.

Each setting includes an explanation, and the manual is also available in-game.
Changes take effect immediately and are written atomically to `settings.ini`.
Enabling debug settings also reveals the unvalidated seeded randomizer and the
action-layer and background authoring tools.

![Navigating the settings overlay: moving between sections and tabs, drawn in the game's own dialog font and frame graphics](/assets/overlay.gif)

### Quality of life

| | |
|---|---|
| **Rebindable controls** | Bind every keyboard and gamepad control independently in Settings → Controls. Keyboard bindings use physical key positions, so they remain in place when the keyboard layout changes. |
| **Full gamepad support** | The default mapping follows a SNES-on-Xbox layout, with support for multiple hotpluggable pads and `gamecontrollerdb.txt`. Seven host actions are also available on the pad: menu, pause, turbo, save state, load state, reset camera, and rendering comparison. |
| **Steam Deck** | The dedicated bundle includes Valve's Steam Runtime SDL3. It works with the default Steam Input mapping, or with SDL's HIDAPI Steam driver in desktop mode. L3 opens the menu. |
| **Camera controls** | The right stick orbits, the triggers zoom, and R3 recentres the view. Sensitivity, deadzone, and invert-Y are configurable, and orbit speed remains consistent across frame rates. |
| **Save states** | Use `F5` and `F7`, or bind both actions to the gamepad. |
| **Turbo** | Press `T` to fast-forward at eight game frames per rendered frame, configurable from 2 to 64. |
| **Render scale and refresh** | Choose an internal render scale from 1× to 8×, downsampled to the window. Presentation modes include renderer-paced VSync, display-relative Uncapped, a selected FPS limit, and unthrottled Unlimited, with windowed, borderless, and exclusive fullscreen options. |
| **Independent HUD and menu scaling** | The promoted widescreen HUD and settings menu can be scaled independently of the game framebuffer, from 25–400% and 100–800% respectively. |
| **Save editor** | After enabling an explicit safety switch, inspect and stage changes to town states, unlocks, levels, magic, items, and scores. The editor creates backups, maintains checksums, and supports lossless INI import and export. |
| **Bridge-free structure limit** | This optional fix stops completed bridges from consuming a town's 128-structure population cap. It applies retroactively while preserving bridge tiles, crossings, and 32-person support. |
| **Cheats** | Infinite HP, MP, and SP; moonjump; invincibility; all magic; ranged sword; angel health; and a generic Pro Action Replay code pinner can all be toggled from the menu. |
| **Audio** | Music, sound effects, and master volume have independent controls. An optional 40-voice mode preserves all eight song voices while queued native effects use 32 additional voices. Output is available at 32.04, 44.1, or 48 kHz, and dialogue blips can be muted separately. |

![Independently scaled simulation-mode HUD in widescreen](/assets/hud-scaling.png)

See [the manual](docs/manual.md) for the complete settings and controls
reference.

---

## Development

The source tree keeps the mechanically generated game banks in `src/gen/`
separate from the authored runtime in `src/` and the reusable recompiler and
portable runner in `snesrecomp-go/`. Both local CMake builds and the hermetic
release builder read their source list from `snesbuild.ini`, which keeps the two
build paths in sync when an authored file is added.

| Preset | Purpose |
|---|---|
| `play` | Produces an optimized local build in `build-release/` without the trace recorder, deep CPU probes, tests, or default per-run diagnostic bundles. |
| `dev` | Produces a Debug build with the unit-test suite, on-demand trace recorder, and timestamped diagnostics under `runs/`. |
| `trace` | Extends the Debug build with generated CPU instrumentation. |
| `asan` | Enables AddressSanitizer and UndefinedBehaviorSanitizer for corruption testing. |
| `control` | Produces an optimized A/B control build with flat town terrain. |

Regression testing combines CTest, Go tests, deterministic recorded input,
semantic end-state digests, and manifest-defined replay benchmarks. Before
reporting performance, the benchmark runner verifies artifact equivalence and
isolates the saves and settings for each run. It can also compare candidate and
reference binaries in adjacent A/B pairs. For more information, see the
source-build instructions under [Quick start](#quick-start), the benchmark
manifest at [`tools/runner-bench.json`](tools/runner-bench.json), and the
[`snesrecomp-go` documentation](snesrecomp-go/README.md).

## Documentation

| Document | What it is |
|---|---|
| [`docs/README.md`](docs/README.md) | Curated game-documentation index |
| [`docs/manual.md`](docs/manual.md) | Player and power-user reference |
| [`docs/SEAMS.md`](docs/SEAMS.md) | Logic↔hardware boundary and architecture map |
| [`docs/progress.md`](docs/progress.md) | Stage, town, and subsystem status |
| [`docs/rendering-engine.md`](docs/rendering-engine.md) | Rendering, streaming, and OAM architecture |
| [`docs/sim-town-terrain.md`](docs/sim-town-terrain.md) | Audited town elevation, grounding, depth, and performance contracts |
| [`docs/snes-native-audio-channels.md`](docs/snes-native-audio-channels.md) | Original SPC channel ownership and effect sequencing |
| [`docs/regional-variants.md`](docs/regional-variants.md) | Hash-provenanced Japanese/USA ROM differences and localization seams |

## License

The repository's original source, including the runtime, tooling,
`recomp/*.cfg`, and documentation, is [MIT-licensed](LICENSE). This license does
not cover the ActRaiser ROM or material derived from it; the Scope section of
the license defines the boundary.

The Go implementation, tooling, tests, documentation, and project-authored
portable C runner under `snesrecomp-go/` use a separate
[MIT license](snesrecomp-go/LICENSE). Its slot-accurate S-DSP contains
adaptations from the MIT-licensed Snaggletooth project. The exact scope,
historical lineage, and retained third-party notices are documented in
[`snesrecomp-go/LICENSE_SCOPE.md`](snesrecomp-go/LICENSE_SCOPE.md),
[`snesrecomp-go/ATTRIBUTION.md`](snesrecomp-go/ATTRIBUTION.md), and
[`snesrecomp-go/THIRD_PARTY_NOTICES.md`](snesrecomp-go/THIRD_PARTY_NOTICES.md).
