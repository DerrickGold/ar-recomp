# ActRaiser Recomp

**A native port of ActRaiser (SNES, USA release) that recompiles your own
cartridge dump into a standalone executable.**

![ActRaiser Recompiled title screen with the optional high-resolution logo](/assets/title-hd.png)

The 1990 SNES game from Quintet and Enix alternates between side-scrolling
action stages and a top-down town simulation. This project targets the USA
cartridge dump and converts its 65816 machine code into C, which is then linked
with a hand-written SDL3 runtime. Running the game as native code allows
widescreen rendering, layered 3D action stages, height-mapped towns and a
3D globe, GPU and CRT effects, replacement art and music, language packs,
and rebindable controls. An in-game settings menu and manual explain the options.

**[Quick start](#quick-start)** · **[Features](#features)** ·
**[Manual](docs/manual.md)** · **[Game documentation](docs/README.md)** ·
**[Status](#progress-at-a-glance)** · **[Development](#development)**

[![GitHub downloads (all assets, all releases)](https://img.shields.io/github/downloads/DerrickGold/ar-recomp/total)](https://github.com/DerrickGold/ar-recomp/releases)
![GitHub Downloads (all assets, latest release)](https://img.shields.io/github/downloads-pre/DerrickGold/ar-recomp/latest/total)
![GitHub commits since tagged version](https://img.shields.io/github/commits-since/DerrickGold/ar-recomp/v0494)


> **For visitors from Japan / 日本語でご覧の皆さまへ**
>
> Thank you for taking an interest in the project and for sharing it with
> others. The current build is based on the USA release, which differs from the
> Japanese version in many ways beyond the language itself. Language-pack
> support is now available, including tools for Japanese text. I am continuing
> work toward Japanese-language coverage and the Japanese version's regional
> features; language packs alone do not change the game mechanics.
>
> 本プロジェクトに関心を寄せていただき、ありがとうございます。また、本作を広めてくださった皆さまにも心より御礼申し上げます。現在のビルドは北米版をベースとしており、日本版とは言語以外にも多くの違いがあります。日本語のテキストにも対応した言語パック機能が利用できるようになりました。引き続き、日本語化と日本版独自の要素への対応を進めています。なお、言語パックだけではゲームの仕様は日本版に変わりません。

---

## Progress at a glance

Development is active, and bugs remain.

| | |
|---|---|
| ✅ | **Action stages:** All USA action routes across the six regions and Death Heim have been completed end to end. |
| 🟡 | **Simulation mode:** Event coverage is confirmed in Fillmore, Bloodpool, Kasandora, Aitos, and Marahna; Northwall remains to be validated. |
| 🟡 | **Diorama mode:** Every action route except Northwall has been play-tested, with further room-by-room refinement planned. |
| 🟡 | **Platforms:** macOS arm64 and Steam Deck are confirmed. macOS x86_64, generic Linux, and Windows still need representative launch testing. |
| 🟡 | **Localization and regional support:** Enhanced fonts, language packs, and Workshop authoring are available. [Regional settings](docs/regional-settings.md) provide separate gameplay and presentation presets, with grouped customization. [Regional artwork and music](docs/regional-media.md) can be extracted from supported donor ROMs. Combined play-testing is ongoing. |

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

**[Download the Builder from GitHub Releases](https://github.com/DerrickGold/ar-recomp/releases)**

The Builder includes the compiler and game dependencies. Building happens
locally and works offline once you have downloaded it.

### What you need

Provide your own legally obtained USA cartridge dump. The project does not
include a ROM, and the Builder verifies your file before continuing.

| | |
|---|---|
| Internal title | `ACTRAISER-USA` |
| Size | 1,048,576 bytes (1 MiB, no copier header) |
| Internal checksum | `0x83DB` |
| SHA-256 | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| SHA-1 | `e8365852cc20178d42c93cd188a7ae9af45369d7` |
| CRC32 | `0xEAC3358D` |

### 1. Download and open the Builder

Download the bundle for your machine from
[Releases](https://github.com/DerrickGold/ar-recomp/releases) and extract it
into a writable folder.

| Platform | Recommended download | Open |
|---|---|---|
| macOS (Apple Silicon / Intel) | `ActRaiserRecompBuilder-macos-arm64-portable.zip` / `-macos-x86_64-portable.zip` | `ActRaiserRecompBuilder.app` |
| Windows (x64 / ARM64) | `ActRaiserRecompBuilder-windows-x86_64-portable.zip` / `-windows-arm64-portable.zip` | The Builder `.exe` |
| Linux (x64 / ARM64) | `actraiser-recomp-linux-x86_64.tar.xz` / `-linux-arm64.tar.xz` | `./run-build.sh` |
| Steam Deck | `ActRaiserRecompBuilder-steam-deck-portable.tar.xz` | The Builder `.AppImage` in Desktop Mode |

Keep the portable desktop Builder and its `.portable` file together; its
build data goes into the adjacent `BuilderData` folder. App-only downloads
are also available for macOS, Windows, and Steam Deck and use per-user storage.
The generic Linux launcher opens the same Workshop in your browser.

On Windows, a startup window shows progress while the bundled tools and browser
are prepared or checked. The first launch can take a few minutes on slower
machines. You can cancel safely; opening the Builder again for the same workspace
brings the existing window forward instead of starting another copy.

On Linux, mark an AppImage executable if necessary. AppImages need FUSE;
`APPIMAGE_EXTRACT_AND_RUN=1 ./ActRaiserRecompBuilder-steam-deck.AppImage`
provides a fallback. Minimal Linux installations may also need the desktop,
audio, and font libraries listed in the download's README.

![The Workshop with an animated Fillmore background and shortcuts to play, languages, assets, and the manual](/assets/builder-workshop.gif)

### 2. Choose a game folder and build

The desktop Builder first asks where to put the game. Its default is an
`ActRaiserRecomp/` folder beside the Builder. Select **Update existing
installation** to rebuild a current game, or choose a new folder when migrating
an older `utils/` installation.

Open **Build game** in the sidebar, choose your USA ROM, and press **Build game**.
The Builder extracts the game's data, recompiles its code, and creates the playable
application. Progress stays visible while you browse the Workshop, including
its language tools and your own instruction manual.

![Three stages of a local build: ROM selected, compilation in progress, and the completed game ready to play](/assets/builder-build.gif)

### 3. Play

Choose **Run game** when the build finishes. Later, open the generated game
directly:

| Platform | Playable output |
|---|---|
| macOS | `ActRaiserRecomp.app` |
| Linux / Steam Deck | `ActRaiserRecomp.AppImage` |
| Windows | `ActRaiserRecomp.exe`; keep its supporting files beside it |

On Windows, double-click `ActRaiserRecomp.exe` directly—no `.bat` launcher is
required. Keep its DLLs, `user-rom.sfc`, `tools/`, assets, settings and saves in
the generated game folder; the `.exe` is not a single-file game package.

Keep the whole output folder for a portable installation, including its
`.portable` file on macOS/Linux, saves, settings, and assets. The game runs
independently of the Builder. These locally generated applications contain your
ROM and are not intended for redistribution.

<details>
<summary>Using per-user game storage on macOS or Linux</summary>

Copy just the generated `.app` or `.AppImage` to your preferred location,
leaving its `.portable` file behind. On launch, it initializes data under your
operating system's application-data directory. Subsequent app updates reuse
that data.

Moving between portable and per-user storage does not transfer saves
automatically. The Workshop edits the game folder selected in the Builder.
See [desktop packaging](docs/desktop-packaging.md) for data locations and
advanced options.

</details>

### 4. Update or import an existing installation

Select your existing game folder to use its settings, assets, ROM and saves
directly, or select an empty folder to start fresh.

Back up your saves, then rebuild into the same game folder with the newer
Builder. Saves, settings, language packs, authored diorama rooms, and custom
assets are retained. For a per-user macOS or Linux installation, replace only
the generated application.

To copy data from a different or legacy installation, use **Import previous installation…**,
review the detected data, and close the game before importing. The Builder
copies the files and preserves the originals; existing destination saves,
settings, and modified assets take priority.

**Change game folder…** selects the destination for the next Builder session.
Save your Workshop work, then close and reopen it to use that folder.

### 5. Keep the game after building

The desktop Builder's output folder is independent of its build tools, so you
can retain the game without keeping the Builder. Older archive installs offer
toolchain cleanup under **Storage & build tools**. Keep their `utils/`
runtime data and retained helper files; download the archive again to rebuild.

See the [Workshop guide](docs/builder-workshop.md) for the full installation,
asset, and language workflows.

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

![Bloodpool Act 2 in authentic 4:3 above the same scene in 16:9, with the background extending symmetrically into the extra width](/assets/widescreen-comparison.png)

### Diorama 3D for action stages

Diorama mode places action-stage backgrounds and sprites on separate planes
in 3D space. Depth shading gives the scene the appearance of a physical
diorama, while the HUD can be scaled independently.

Rooms can be tuned independently:

- **Layer layout:** Each room defines its own depths and can use a stable
  backdrop from the ROM when the live PPU view is too narrow.
- **Camera modes:** Free Cam supports manual orbit and zoom, while Dynamic Cam
  leans and reacts around an authored pose.
- **Framing:** Vertical extension reveals more of the stage above and below the
  original 224 lines. Skybox and Shoebox walls enclose finite backdrops.

![Aitos Act 2 gameplay with separated 3D layers and the dynamic diorama camera](/assets/gameplay-diorama.gif)

This Fillmore Act 1 comparison starts with the original 4:3 presentation,
then shows enhanced widescreen with the camera tilting and pulling back to
reveal the layers.

![Fillmore Act 1 in original 4:3, followed by enhanced widescreen with a tilted and zoomed-out diorama camera](/assets/fillmore-rendering-comparison.gif)

### 3D simulation towns

Simulation mode rebuilds each town as an oblique 3D scene:

- **Terrain:** A height field raises hills, plateaus, and cliff faces across
  all six towns. A landscape slider can scale the relief back to a flat plane.
- **Scenery:** Region-aware voxel models replace structures and foliage at
  selectable quality levels. Stone bridges span the banks, while mountains
  and volcanoes use camera-aligned relief.
- **Actors and depth:** People and effects are billboards grounded against
  the terrain, and flying actors maintain a stable altitude above it. Ridges
  and buildings can occlude objects behind them.
- **Lighting and interaction:** Objects cast terrain-following shadows, with
  optional soft blur and rim lighting. Miracles and enemy attacks add local
  lighting and particles. A connected globe continues beyond the town's borders,
  and building placement and miracle targeting work in the tilted view.

![Aitos during a volcanic eruption, with fireball trails and haze, ending with a low-angle view of the voxel buildings and raised terrain](/assets/sim3d-town.gif)

### 3D world navigation

The Sky Palace travels over a spherical world with raised mountain ranges,
developed towns, moving clouds, and an atmosphere against the starfield. The
globe uses the same terrain, buildings, and regional artwork as the town view.

- **Explore:** Right-drag or use the right stick to look around the globe.
  The wheel or triggers zoom; middle-click or R3 resets the view. Travel and
  town entry use the original menus.
- **Sky Palace:** The optional globe backdrop shows the currently selected
  town below the Palace, with moving clouds behind the original pillars,
  angel, and menus.
- **Adjust detail:** Lighting, clouds, atmosphere, town models, ground detail,
  and mountain relief have separate controls for balancing appearance and
  performance.

![Zoomed-out travel over the 3D globe, with moving clouds and the atmosphere visible against space](/assets/globe-travel.gif)

![Clouds drifting over developed Fillmore below the Sky Palace, with roads and buildings visible through the moving cloud layers](/assets/sky-palace-globe.gif)

### GPU and CRT effects

The GPU renderer provides individually adjustable effects:

- **Diorama lighting and focus:** Warm rim lighting outlines sprites, depth of
  field softens distant layers, and edge anti-aliasing smooths tilted planes.
- **Spell effects:** Particles and local illumination follow action-stage
  magic in both flat and 3D views.
- **CRT presentation:** Curved glass, scanlines, and a phosphor mask can be
  applied to any view, including authentic rendering. **Video → CRT** also
  controls colour fringing, signal softness, corner shading, and brightness.
- **Frame interpolation:** Optional intermediate diorama frames smooth motion
  on high-refresh displays while normal gameplay retains its 60 Hz logic.

![Aitos Act 2 with sprite rim lighting and depth-of-field blur on the distant diorama layers](/assets/gpu-effects.png)

![Fillmore Act 2 in widescreen with curved glass, scanlines, and the CRT phosphor mask enabled](/assets/crt-effects.png)

### High-resolution Mode 7

Mode 7 rendering runs at the internal render scale rather than 256×224, so its
rotation, zoom, and per-scanline warps stay sharp instead of magnifying the
original low-resolution sampling.

![Mode 7 rendering at increased internal resolution](/assets/mode7.png)

### Asset and music replacements

Art replacement is limited to the title logo on the title screen and in the
animated intro, where the replacement follows the original rotation, zoom,
and warp effects.

![The replacement title logo with its Recompiled subtitle](/assets/title-hd.png)

Art and music replacements share the user-owned `game-assets/manifest.ini`,
which is preserved across upgrades. The Workshop's **Assets** section provides:

- previews of the included HD title art and all 17 entries in the ROM's song
  table, including unnamed tracks identified by slot;
- extracted previews of the original audio; and
- controls for installing replacements or restoring the original art and music.

![The Workshop's music controls with original-ROM and replacement audio previews](/assets/assets-1.png)

![Split by level offering a separate Bloodpool replacement for music shared with Kasandora](/assets/assets-2.png)

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

### Languages and enhanced text

Language packs replace the game's text without patching the ROM. Enhanced
font rendering supports Unicode text, font fallback, and layouts that adapt
to translated dialogue.

- **Play:** Install an `.arlang` file in the Workshop's **Languages** section,
  restart the game, and choose it under **Settings → Localization**. The
  original US text remains available.
- **Read:** Adjust enhanced font size and choose Crisp or Smooth sampling,
  with full-resolution, low-resolution, or mosaic rendering independent of
  the game graphics. The original US script can also use its native font.
- **Create and share:** Start a translation from the US script, edit messages
  alongside a reference, check font coverage, and export a language pack from
  the Workshop. Saving a project and installing it into the game are separate
  steps.

The Workshop interface supports English, French, German, and Japanese;
its language is independent of the game's selected pack. Translation coverage
depends on the pack, and Japanese-version game mechanics are not yet included.
See the [language-pack guide](docs/language-packs.md) for installation and
authoring details.

This comparison shows the same dialogue with the native font and five enhanced
configurations. Enhanced samples use 140% font size; pixelation strengths are
labelled in output pixels. Open the image at full size to compare the edges.

![Native font compared with full-resolution Crisp and Smooth, low-resolution, and two mosaic settings](/assets/enhanced-font-comparison.png)

The Sky Palace capture below uses text extracted from the original Japanese
ROM, displayed with enhanced fonts in the US game. It demonstrates a partial
local language pack, not a complete or bundled Japanese translation.

![ROM-sourced Japanese dialogue and menu text in the Sky Palace above Fillmore](/assets/language-japanese.png)

![Editing an example English rewrite alongside the original US dialogue in the Workshop](/assets/language-workshop.png)

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

The in-game menu retains ActRaiser's dialog frames and offers both native and
enhanced text rendering. It groups display, 3D, audio, localization, controls,
save, and other settings into sections accessible by keyboard or gamepad.

Each setting includes an explanation. If you install an instruction manual in the
Builder, a Manual section appears in-game to read it; without one the section is
simply absent.
Settings are saved automatically to `settings.ini`.

![An illustrative tour of the settings overlay using the original dialog font; the current menu also includes localization controls](/assets/overlay.gif)

### Quality of life

| | |
|---|---|
| **Rebindable controls** | Bind every keyboard and gamepad control independently in Settings → Controls. Keyboard bindings use physical key positions, so they remain in place when the keyboard layout changes. |
| **Full gamepad support** | The default mapping follows a SNES-on-Xbox layout, with support for multiple hotpluggable pads and `gamecontrollerdb.txt`. Bind controls for the menu, pause, turbo, camera reset, and rendering comparison. |
| **Steam Deck** | The dedicated bundle includes Valve's Steam Runtime SDL3. It works with the default Steam Input mapping, or with SDL's HIDAPI Steam driver in desktop mode. L3 opens the menu. |
| **Camera controls** | The right stick orbits, the triggers zoom, and R3 recentres the view. Sensitivity, deadzone, and invert-Y are configurable, and orbit speed remains consistent across frame rates. |
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

### Build from source

For a local build without the packaged Builder, install Go 1.24 or newer,
CMake 3.25 or newer, GNU Make, a C/C++ toolchain, and the development libraries
for SDL3 3.4+ and SDL3_ttf 3.2+. On macOS, the compiler comes with Xcode Command
Line Tools; on Windows, use a compatible native compiler and a shell with Make.

From the repository root, run:

```sh
make dev ROM=/absolute/path/to/your-usa-rom.sfc
./build-release/ActRaiserRecomp /absolute/path/to/your-usa-rom.sfc --config config.ini
```

`make dev` prepares the native US language source, generates the game C files
if they are missing, and builds the optimized `play` preset. It also creates
`config.ini` from the stock template if you do not already have one. If your
ROM is named `ar.sfc` in the repository root, you can omit `ROM=…`.

For subsequent C/C++ changes, use `cmake --build --preset play`. These commands
produce a development executable; see [desktop packaging](docs/desktop-packaging.md)
to turn it into a self-contained `.app` or `.AppImage`.

### Layout and testing

`src/gen/` contains mechanically generated game code, while `src/` holds the
authored runtime. `installer/` contains the Builder, Workshop, content tools,
and release packaging. Shared recompilation and build tooling lives in
`snesrecomp-go/`; `snesbuild.ini` defines the runtime source list for both local
and packaged builds.

| Preset | Purpose |
|---|---|
| `play` | Produces an optimized local build in `build-release/` without tests or tracing. |
| `dev` | Produces a Debug build with the unit-test suite, on-demand trace recorder, and timestamped diagnostics under `runs/`. |
| `trace` | Extends the Debug build with generated CPU instrumentation. |
| `asan` | Enables AddressSanitizer and UndefinedBehaviorSanitizer for corruption testing. |
| `control` | Produces an optimized A/B control build with flat town terrain. |

After the initial source build, configure the test-enabled preset and run the
C and Go suites:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
go -C installer test ./...
go -C snesrecomp-go test ./...
```

Regression testing combines CTest, Go tests, and recorded gameplay replays.
Replay benchmarks verify equivalent game state before comparing performance.
For benchmark definitions and development details, see
[`tools/runner-bench.json`](tools/runner-bench.json), the
[`installer` documentation](installer/README.md) and
[`snesrecomp-go` documentation](snesrecomp-go/README.md).

## Documentation

| Document | What it is |
|---|---|
| [`docs/README.md`](docs/README.md) | Curated game-documentation index |
| [`docs/manual.md`](docs/manual.md) | Player and power-user reference |
| [`docs/builder-workshop.md`](docs/builder-workshop.md) | Builder and Workshop user guide |
| [`docs/language-pack-format.md`](docs/language-pack-format.md) | UTF-8 translation pack authoring and validation contract |
| [`docs/language-packs.md`](docs/language-packs.md) | Direct `.arlang` installation, sharing, and editor-free/AI authoring |
| [`installer/README.md`](installer/README.md) | ActRaiser Builder ownership and developer entry points |
| [`docs/diorama-depth-shapes.md`](docs/diorama-depth-shapes.md) | Depth effects for custom action-room layouts |
| [`docs/save-format.md`](docs/save-format.md) | SRAM fields, checksums, and save editing |
| [`docs/performance-overlay.md`](docs/performance-overlay.md) | Reading performance information and reporting slow scenes |
| [`docs/snes-native-audio-channels.md`](docs/snes-native-audio-channels.md) | Original SPC channel ownership and effect sequencing |
| [`docs/rom-map.md`](docs/rom-map.md) | ROM data regions and cross-release localization evidence |

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
ActRaiser Builder resources and game-distribution dependencies are documented
separately in
[`installer/THIRD_PARTY_NOTICES.md`](installer/THIRD_PARTY_NOTICES.md).
