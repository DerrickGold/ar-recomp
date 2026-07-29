# ActRaiser Recomp

**A native port of ActRaiser (SNES, USA release) — built on your machine, from
your own cartridge dump, into a real executable.**

![title screen](/assets/title.png)

*ActRaiser* is Quintet and Enix's 1990 SNES game that alternates between
side-scrolling action-platformer stages and a top-down "god game" town
simulation. This project targets the USA cartridge dump.

Not an emulator. The ROM's 65816 machine code is statically recompiled into C
and linked against a hand-written SDL3 runtime, which is what makes the rest
possible: true widescreen, a tilted 3D diorama presentation for the action
stages, a projected 3D simulation town, GPU shader effects, drop-in HD art and
music, re-bindable controls, save states, and a full in-game settings menu drawn
with the game's own dialog font.

**[Quick start](#quick-start)** · **[Features](#features)** ·
**[Manual](docs/manual.md)** · **[Contributing](docs/contributing.md)** ·
**[Status](#current-status)**

---

## Progress at a glance

| | |
|---|---|
| ✅ | **Action stages** — every act across all regions, plus Death Heim, played through end to end |
| 🟡 | **Simulation mode** — only Fillmore verified end to end; the other five towns await play-testing |
| 🟡 | **Diorama mode** — playable, but per-room layer depths still need tuning to look their best |
| 🟡 | **Platforms** — builds confirmed on macOS and Steam Deck. **Windows is untested and unverified.** |

Per-stage detail: [`docs/progress.md`](docs/progress.md).

---

## AI disclosure

**This project is built with substantial help from AI coding assistants** (Claude
/ Claude Code). Large portions of the recompiler tooling, runtime code,
debugging infrastructure, and documentation were written by AI under my
direction and review. Assume AI involvement throughout.

The two halves have different provenance: the recompiled game logic is a
mechanical translation of the original binary — never committed here, no
authorship claimed. The runtime around it (widescreen streaming, the diorama and
3D town renderers, the settings system) is original work.

It started as wanting to play the game sooner. It's now aimed at preservation —
a native build that outlives the hardware — and at improving the game without
changing how it feels, which is why every enhancement is a toggle and the
authentic 4:3 path stays intact.

### Notes for a non-AI decompilation

The reverse-engineering notes are maintained to be useful on their own: ~6,800
lines, none ROM-derived, all MIT-licensed. They describe the 1990 game rather
than this implementation, so they carry over to whatever you build.

| | |
|---|---|
| [`docs/SEAMS.md`](docs/SEAMS.md) | ~1,600 lines of logic↔hardware boundaries: object systems, dispatch tables, subsystem roles, and each seam's intent |
| [`docs/research-symbol-map.md`](docs/research-symbol-map.md) | address → candidate name, with confidence and evidence |
| [`docs/ram-map.md`](docs/ram-map.md) · [`docs/rom-map.md`](docs/rom-map.md) | WRAM and ROM data-region references |
| [`docs/rendering-engine.md`](docs/rendering-engine.md) | rendering, streaming, and OAM behaviour |
| [`docs/sim-object-catalog.md`](docs/sim-object-catalog.md) | simulation-mode object/structure records |
| [`recomp/*.cfg`](recomp/) | 29 bank files: function addresses, entry m/x pins, dispatch tables |

---

## Quick start

You do not need a compiler, CMake, SDL, Go, or a repository checkout. You need
your own ROM and about five minutes.

### What you need

Your own legally obtained USA cartridge dump. **The ROM is not included and
never will be.** The build verifies it against:

| | |
|---|---|
| Internal title | `ACTRAISER-USA` |
| Size | 1,048,576 bytes (1MB, no copier header) |
| Internal checksum | `0x83DB` |
| SHA-256 | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| SHA-1 | `e8365852cc20178d42c93cd188a7ae9af45369d7` |
| CRC32 | `0xEAC3358D` |

### 1. Download the builder for your platform

Grab the archive for your machine from
**[Releases](https://github.com/DerrickGold/ar-recomp/releases)** and unpack it
anywhere.

| Platform | Bundle |
|---|---|
| macOS (Apple Silicon / Intel) | `actraiser-recomp-macos-arm64.tar.xz` / `-macos-x86_64.tar.xz` |
| Windows (x64 / ARM64) | `actraiser-recomp-windows-x86_64.zip` / `-windows-arm64.zip` |
| Linux (x64 / ARM64) | `actraiser-recomp-linux-x86_64.tar.xz` / `-linux-arm64.tar.xz` |
| Steam Deck | `actraiser-recomp-steam-deck.tar.xz` |

Each bundle is fully self-contained: the whole buildable project plus a pinned C
toolchain (Zig) and, where a redistributable exists, SDL3. Every archive ships a
SHA-256 sidecar.

**macOS and Steam Deck are the tested paths.** Every bundle cross-builds from
one machine, but the Windows and generic Linux archives have not been run
end-to-end by this project yet — if you try one, an issue either way is useful.

![An unpacked bundle folder: a run-build script, a README, and a utils folder](/assets/builder-run-script.png)

### 2. Run `run-build` and pick your ROM

Double-click `run-build.command` (macOS) or `run-build.bat` (Windows), or run
`./run-build.sh` (Linux). Your browser opens a private local builder — served on
loopback only, behind a per-process token. Choose your ROM and press **Build
game**.

![The local builder: box art masthead, ROM file picker, and Build game button](/assets/builder-gui.png)

The game's C is generated on your computer from your ROM and compiled there.
**Your ROM never leaves the machine** — the page talks only to `127.0.0.1`, and
nothing is uploaded. This takes a few minutes and happens once.

![Build in progress: numbered step list and the progress dock at the bottom of the page](/assets/builder-progress.png)

### 3. Play

When the build finishes, press **Launch game** — and a `run-game` file appears
in the folder. Open that any time afterwards to play instantly, with no rebuild.

The bundle also carries the original 40-page instruction booklet, readable in
the builder's **Manual** tab.

<details>
<summary>Building from a source checkout instead</summary>

If you want to change the code, see
[docs/contributing.md](docs/contributing.md#building-from-source). The short
version is `make dev` with Go, CMake, a C11 compiler, and SDL3 installed.

</details>

---

## Features

Everything below is off-by-default-safe: the game boots as an authentic 4:3
SNES experience, and each enhancement is a toggle in the in-game settings
overlay (`Esc` or `F1`, or L3 on a pad).

### Widescreen

True 16:9 and 16:10 presentation — not a stretch or a crop. Background layers
are streamed into the wider viewport, sprites are activated beyond the original
screen bounds, and the HDMA/parallax effects the game relies on keep working.
Three modes cycle live with `F9`: authentic 4:3, widescreen raw, and widescreen
full (which promotes the HUD to an independently scaled host overlay).

Every ordinary action level across regions 1–6 plus all of Death Heim has been
played through and validated in widescreen.

![Bloodpool Act 2 in authentic 4:3 above the same scene in 16:9, with the background extending symmetrically into the extra width](/assets/widescreen-comparison.png)

### Diorama 3D — the action stages as a tilted layered box

The PPU's background layers, sprite plane, and HUD are captured separately each
frame and rendered as real planes in 3D space, tilted toward the camera. The
result reads like a physical diorama of the stage: BG2 sits behind BG1, sprites
stand between them, and depth shading falls off with distance.

A per-room layer file (`diorama-layers.ini`) tunes each area's depths, and the
camera is yours — orbit with the right stick or a right-drag, zoom with the
triggers or the wheel. Optional **Skybox** and **Shoebox walls** modes enclose
the box so tilting never reveals the void past a finite backdrop.

![The diorama camera orbiting a Fillmore stage, layers moving against each other in depth](/assets/diorama.gif)

The same paused frame, flat and tilted — nothing about the game changed, only how
its layers are composited:

![Fillmore Act 1 side by side: flat 2D on the left, the same frame tilted into separated 3D planes on the right](/assets/diorama-comparison.png)

### 3D simulation towns

Simulation mode's top-down map is projected onto an oblique 3D ground plane.
Structures, people, and effects become billboards standing on that ground;
flying actors are lifted to their real height; each object casts a ground
shadow, optionally soft-blurred, with a rim light on the side facing the sun.
The world map extends the terrain past the town's edge so the town sits in a
landscape instead of on a floating tile. Everything stays in the tilted space —
building placement and miracle targeting happen in 3D too, with no drop back to
a flat view.

![Fillmore projected onto a 3D ground plane, trees and structures standing as billboards while the camera moves](/assets/3dtown.gif)

![Close on Fillmore's temple: trees and buildings standing on the projected ground](/assets/sim3d-detail.png)

Pull the camera back and the world-map underlay carries the terrain out to the
coastline, with the cloud deck between you and the ground:

![Zooming out from Fillmore until the whole landmass and its surrounding clouds are visible](/assets/3dtown-zoom.gif)

### 3D world navigation

Flying the Sky Palace between towns renders as a full-world 3D scene: the whole
map on one ground plane, directional cloud shadows, and world-anchored cloud
banks that you descend through as the Palace drops toward a town.

![Approaching Bloodpool across the full world map, terrain on one ground plane with cloud banks at the edges](/assets/worldnav-3d.png)

### GPU shader effects

An SDL GPU renderer backend adds per-effect polish to the diorama: **rim
lighting** on sprite silhouettes, **depth of field** blurring layers by
distance, and **edge anti-aliasing** on the tilted layer edges. Soft shadow blur
and scroll interpolation are present but ship off by default with known issues.

![A diorama scene with GPU effects off on the left and rim lighting plus depth of field on the right](/assets/shader-comparison.png)

### Mode 7 and HD art replacement

Mode-7 rendering runs at the internal render scale rather than 256×224, so
rotation and zoom stay sharp. On top of that, a tracked manifest exposes every
known replacement hook: drop a PNG into `game-assets/hd/` and it appears next
launch — `screen`-plane art scales to the viewport, `mode7`-plane art is
rendered through the live Mode-7 matrix so warps and zooms apply to your
artwork. Nothing to configure; a missing file is silently inert.

![Mode 7 rendering at increased internal resolution](/assets/mode7.png)

![The title screen twice: the ROM's original logo on the left, a high-resolution replacement in its place on the right](/assets/hd-title-comparison.png)

### Custom music

The same manifest streams OGG Vorbis in place of the SPC driver's songs, with
sample-accurate looping (`LOOPSTART`/`LOOPLENGTH` tags or manifest keys) and
optional per-song variants gated on game state. Sound effects stay authentic —
the SPC driver keeps running and only its per-song instrument voices are muted.
Toggle between your track and the original sequencer live, mid-song.

Every song the game starts without a replacement prints exactly what to drop in,
so one playthrough identifies all 17 tracks.

### The settings overlay

A complete in-game menu, drawn with ActRaiser's own 2bpp dialog font and Sky
Palace dialog frame decoded from your ROM at startup. Eight sections — Video,
Diorama, Town 3D, Audio, Controls, Cheats, Save, System — reachable from any
game state, keyboard or gamepad, with every row explaining itself. Changes apply
live and are written back to `settings.ini` atomically.

![Navigating the settings overlay: moving between sections and tabs, drawn in the game's own dialog font and frame graphics](/assets/overlay.gif)

### Quality of life

| | |
|---|---|
| **Re-bindable controls** | Every button, keyboard and gamepad independently, from Settings → Controls. Keyboard binds are stored by physical key position, so layout changes follow the keys. |
| **Full gamepad support** | Standard SNES-on-Xbox mapping, multiple pads with hotplug, `gamecontrollerdb.txt` support, and six host actions (menu, pause, turbo, save/load state, reset camera) bound to the pad so no keyboard is needed. |
| **Steam Deck** | A dedicated bundle with Valve's Steam Runtime SDL3. Works with defaults through Steam Input, and via SDL's HIDAPI Steam driver from desktop mode. L3 opens the menu. |
| **Camera on the stick** | Right stick orbits, triggers zoom, R3 recentres — with sensitivity, deadzone, and invert-Y, integrated over real elapsed time so orbit speed is frame-rate independent. |
| **Save states** | `F5` / `F7`, or bind them to the pad. |
| **Turbo** | `T` fast-forwards at 8 game frames per rendered frame (2–64, configurable). |
| **Render scale & refresh** | Internal render scale 1×–8× downsampled to the window; vsync, uncapped, or a chosen FPS cap; windowed, borderless, or exclusive fullscreen. |
| **Independent HUD & menu scaling** | Scale the promoted widescreen HUD and the settings menu separately from the game framebuffer, 25–400% and 100–800%. |
| **Save editor** | Inspect and stage battery-save edits in-game behind an explicit safety switch: town states, unlocks, levels, magic, items, scores. Backs up, checksums, and supports lossless INI import/export. |
| **Bridge-free structure limit** | Optional fix: completed bridges stop consuming a town's 128-structure population cap, migrating to spare save space while keeping their tiles, crossing, and 32-person support. Retroactive on existing towns. |
| **Cheats** | Infinite HP/MP/SP, moonjump, invincibility, all magic, ranged sword, angel health, and a generic Pro Action Replay code pinner — all toggleable live from the menu. |
| **Audio** | Master volume, 32.04/44.1/48 kHz output through a continuous resampler that holds pitch and tempo at every rate, and a mute for the dialogue blip. |

![Independently scaled simulation-mode HUD in widescreen](/assets/hud-scaling.png)

Full reference for every one of these: **[docs/manual.md](docs/manual.md)**.

---

## Current status

**Actively in development — expect bugs.**
[`docs/progress.md`](docs/progress.md) is the authoritative, kept-current status
tracker: per-action-stage / per-sim-town playability tables plus automated
codebase metrics — read it rather than this summary if the two ever disagree.
Expanding on the [summary above](#progress-at-a-glance):

- **Action stages, regions 1–6** — every ordinary action level has been played
  through and is fully playable. Widescreen BG streaming, sprites, activation,
  enemies/platforms, bosses, fast vertical traversal, and the observed
  HDMA/parallax effects all render and behave correctly.
- **Death Heim (region 7)** — the complete boss rush, final boss, and post-boss
  sky transition are playable and widescreen-validated.
- **Simulation mode** — Fillmore has one confirmed clean end-to-end town round;
  Bloodpool has partial entry/lightning coverage. The remaining towns and full
  simulation-mode widescreen behavior are TBD.
- **Remaining widescreen polish** — presentation-aware camera/world-edge
  clamping so the ends of finite backgrounds cannot scroll into the wider
  viewport.
- **3D town rendering** — phases 0–4 plus the ground extension are done; other
  towns still need art and layer work.
- **Diorama polish** — the mode is playable everywhere, but per-room layer
  depths in `diorama-layers.ini` are still being tuned room by room; some areas
  read flatter than they should until their entry is authored.
- **Platforms** — macOS and Steam Deck are built and played on regularly. The
  Windows and generic Linux bundles cross-build but have not been run by this
  project; reports either way are welcome.

Open bugs and investigation state live in [`DEBUG.md`](DEBUG.md); design specs
and their real (code-derived) status live in [`specs/README.md`](specs/README.md).

## Documentation

| Document | What it is |
|---|---|
| [`docs/manual.md`](docs/manual.md) | Player/power-user reference: every config key, control, hotkey, cheat, overlay behavior, and the asset-replacement systems |
| [`docs/contributing.md`](docs/contributing.md) | Building from source, project layout, and what can/can't be committed here |
| [`DEBUG.md`](DEBUG.md) | The debugging guide — every tool, every known bug class, the full bug-hunt journal |
| [`docs/SEAMS.md`](docs/SEAMS.md) | Logic↔hardware boundary map and the reverse-engineered game architecture |
| [`docs/progress.md`](docs/progress.md) | Per-stage / per-town / per-subsystem status tracker |
| [`docs/rendering-engine.md`](docs/rendering-engine.md) | Rendering, streaming, and OAM architecture |
| [`docs/BUILD_TOOLING.md`](docs/BUILD_TOOLING.md) | The `snesbuild` driver, hermetic builds, and the distribution bundles |
| [`specs/README.md`](specs/README.md) | Index of every design spec with its real status |

## License

This repo's original source (runtime, tooling, `recomp/*.cfg`, docs) is
[MIT-licensed](LICENSE). That license explicitly does **not** cover the
ActRaiser ROM or anything derived from it — see the LICENSE file's Scope
section, and
[what can and can't be committed](docs/contributing.md#what-can-and-cant-be-committed-here).

The bundled `snesrecomp-go/` is a Go reimplementation of the historical Python
recompiler and includes its copied C runtime and adapted documentation. That
source repository had not declared an overall license at the snapshot used for
the port. The module is therefore explicitly excluded from this repository's
MIT grant; see `snesrecomp-go/ATTRIBUTION.md`, its runtime provenance README,
and the LICENSE Scope section.
