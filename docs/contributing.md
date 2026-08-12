# Contributing — building from source and working in this repo

Developer guide to the recompilation model, source builds, repository layout,
and contribution boundaries. For player setup, use the
[README](../README.md).

**Contents**

- [Recomp, not decomp](#recomp-not-decomp)
- [Building from source](#building-from-source)
- [Producing the distribution bundles](#producing-the-distribution-bundles)
- [Project layout](#project-layout)
- [Where to read first](#where-to-read-first)
- [What can (and can't) be committed here](#what-can-and-cant-be-committed-here)

## Recomp, not decomp

This is a **static recompilation ("recomp")**, not a decompilation ("decomp"):

- A **decompilation** is a new, hand-written implementation based on an
  understanding of the original binary.
- A **static recompilation** mechanically translates the ROM's 65816 machine
  code into equivalent C. This project uses
  [`snesrecomp-go`](../snesrecomp-go/README.md), the bundled Go reimplementation
  of the historical SNES static recompiler. Its generated output is ROM-derived,
  never committed, and rebuilt locally from each user's ROM.

The tracked, original code is the hand-written runtime: SDL3 windowing and
input, PPU/APU video and audio, save states, and HLE shims for timing- or
hardware-dependent routines.

### Why recomp instead of just running it in an emulator?

A static recomp produces a native executable instead of depending on an
emulation core. It can be profiled, debugged, ported, and extended like other
native code while still requiring the user to own the original game. That
portability is the project's preservation case.

## Building from source

Most people should not do this — the [released builder
bundle](https://github.com/DerrickGold/ar-recomp/releases) does all of it with a
GUI and no toolchain install. Build from a checkout if you want to change the
code.

### Dependencies

- **Go 1.24+** — builds the recompiler/driver; required for both build paths
- **CMake** ≥ 3.16 — developer preset builds only
- **A C11 compiler** (clang or gcc) — developer preset builds only
- **SDL3** development files — auto-discovered by both source-build paths
- **git**

Python is optional for unrelated forensic/triage scripts; it is not a build,
regeneration, runtime, or opcode-validation dependency.

The commands below install dependencies for the CMake-preset build. The
hermetic path needs only Go and SDL3 development files because it uses a pinned
Zig compiler. Released bundles include SDL3 where redistribution permits it.

**macOS** (verified — the primary development platform):

```sh
brew install cmake sdl3 go
```

**Linux** (Debian/Ubuntu; the general-distro source path is not yet verified):

```sh
sudo apt install build-essential cmake libsdl3-dev golang-go
```

**Windows**: the runtime has MSVC-oriented support in
`snesrecomp-go/runtime/runner.cmake`, but this project has not been run from a
Windows source build. Documented build steps are welcome.

### Steps

1. **Clone this repo.** The Go recompiler and C runtime are included; there is
   no secondary toolchain checkout or symlink to create.

2. **Supply your own ROM.** Place a verified `ar.sfc` (see the checksums in the
   [README](../README.md#what-you-need)) at the repo root.

3. **Regenerate the recompiled banks.** `snesbuild` refreshes `src/gen/*.c`,
   `recomp/funcs.h`, metadata, the RTS-web census, and the hard-stub report:

   ```sh
   go -C snesrecomp-go run ./cmd/snesbuild regen \
     --root .. --rom ar.sfc --allow-stubs
   ```

   A downloaded `snesbuild`/`snesbuild.exe` can run the same command without Go.
   `tools/regen.sh` remains a compatibility wrapper. Because of the inherited
   hard-stub backlog, strict regeneration exits nonzero after producing its
   output; local work may use `--allow-stubs`.

4. **Compile the game.** Two options:

   - **Hermetic** — uses a pinned Zig toolchain and discovers SDL3:

     ```sh
     go -C snesrecomp-go run ./cmd/snesbuild toolchain fetch   # one time
     go -C snesrecomp-go run ./cmd/snesbuild build --hermetic --root ..
     ```

   - **CMake presets** — uses the installed CMake, C11 compiler, and SDL3:

     ```sh
     cmake --preset play && cmake --build --preset play
     # or, through the driver: go -C snesrecomp-go run ./cmd/snesbuild build --root ..
     ```

     The `play` preset builds an optimized binary into `build-release/`; `dev`
     builds into `build/`; `asan` and `trace` exist for debugging.

From a clean or fresh tree, `make dev` does steps 3 and 4 in one command
(regenerating only if `src/gen` is empty). For the normal inner loop after
editing C, run `cmake --build --preset play` directly — no regen or reconfigure
needed.

The released `run-build` bundle automates steps 2–4. For toolchain integration,
see [`snesrecomp-go/README.md`](../snesrecomp-go/README.md), the
[project integration guide](../snesrecomp-go/docs/PROJECT_INTEGRATION.md), and
[`BUILD_TOOLING.md`](BUILD_TOOLING.md).

## Producing the distribution bundles

To produce all seven bundles from a source checkout (into `release/`, named
`actraiser-recomp-<platform>.{tar.xz,zip}` with SHA-256 sidecars):

```sh
make release
```

Go and CMake are the host requirements. The packaging process downloads the C
toolchain and SDL3, and the CGO-free Go module cross-builds every target from one
machine. See [`BUILD_TOOLING.md`](BUILD_TOOLING.md) for packaging details and
current signing, CI, and runtime-validation gaps.

The platforms are `macos-arm64`, `macos-x86_64`, `linux-x86_64`, `linux-arm64`,
`windows-x86_64`, `windows-arm64`, and `steam-deck`. Each can be built alone,
e.g.:

```sh
make release-steam-deck
```

The dedicated `steam-deck` bundle is always Linux x86_64 and carries the
pinned SDL3 runtime from Valve's Steam Runtime.

## Project layout

```
ActRaiserRecomp/
├── ar.sfc                  # (you supply this — gitignored)
├── config.ini               # default runtime config (player-facing)
├── dev-config.ini           # development config: debug flags + cheats enabled
├── nocheats-config.ini      # like dev-config.ini, cheats off
├── diorama-layers.ini       # per-room diorama layer depth/alpha/rake overrides
├── CMakeLists.txt            # developer build (CMake presets: dev/play/asan/trace)
├── snesbuild.ini             # game build manifest for the hermetic/bundled path
├── Makefile                  # `make release` (all platform bundles), `make clean`
├── DEBUG.md                  # ★ the debugging guide — every tool, every known
│                                bug class, and the full bug-hunt journal.
│                                Start here if something's broken.
├── docs/
│   ├── manual.md              # ★ player/power-user reference: every config key,
│   │                             control, cheat, and overlay behavior
│   ├── contributing.md        # this file
│   ├── SEAMS.md               # ★ logic↔hardware boundary map + reverse-
│   │                             engineered game architecture (object systems,
│   │                             dispatch tables, subsystem roles) — the
│   │                             groundwork for a future full decompilation.
│   ├── research-symbol-map.md  # manually curated address → candidate semantic
│   │                             name index, with confidence/promotion status
│   ├── ram-map.md             # WRAM address reference
│   ├── rom-map.md             # ROM data-region reference
│   ├── rendering-engine.md    # rendering/streaming/OAM architecture
│   ├── widescreen-survey.md   # widescreen evidence + implementation record
│   ├── settings-system.md     # live settings + overlay architecture/record
│   ├── BUILD_TOOLING.md       # cross-platform driver + binary-bundle roadmap
│   └── progress.md            # ★ per-action-level / per-sim-mode-town /
│                                 major-functionality status tracker
├── specs/
│   ├── README.md              # ★ index: every spec with its REAL status,
│   │                             derived from the code (several documents'
│   │                             own headers are stale)
│   ├── SPEC-*.md              # specs for discrete changes; source comments
│   │                             cite these by bare filename, so the folder
│   │                             is flat and names never move
│   └── ar-recomp-*.md         # broader multi-milestone implementation plans
├── recomp/
│   ├── bank*.cfg              # hand-authored per-bank recompiler directives:
│   │                             function addresses, entry m/x width pins,
│   │                             indirect-dispatch tables, HLE hooks. This is
│   │                             the actual authored "source" that drives
│   │                             regeneration — safe to commit (see below).
│   └── funcs.h                # generated from the *.cfg files — NOT committed
├── game-assets/
│   ├── manifest.ini           # ★ tracked asset-replacement manifest: every
│   │                             known HD-art + music hook, engaged by
│   │                             dropping the asset file it names
│   ├── hd/                    # your HD art (*.png) — gitignored
│   └── audio/                 # your music (*.ogg) — gitignored
├── src/
│   ├── main.c                 # SDL3 entry point, input, frame loop, config
│   ├── actraiser_rtl.c        # game-specific HLE/runtime glue + cheats
│   ├── actraiser_spc_player.c # SPC/audio upload handling
│   ├── diorama*.c             # tilted-3D action-stage presentation
│   ├── sim3d*.c, sim_world_*.c # 3D simulation town + world navigation
│   ├── hd_replacements.c      # HD-art manifest parsing + per-frame gates
│   ├── music_replacements.c   # music manifest + OGG loop streamer + triggers
│   ├── settings.c / settings_overlay.c  # live settings registry + host menu
│   ├── input_map.c            # keyboard/gamepad binding registry
│   ├── save_system.c          # SRAM backends, save editor, import/export
│   ├── config.c               # .ini parsing
│   └── gen/                   # ★ regenerated C output — NOT committed (you
│                                 produce this locally via snesbuild regen)
├── third_party/
│   └── stb/                   # vendored single-file libs (stb_image,
│                                 stb_vorbis) — tracked, no install step
├── snesrecomp-go/             # standalone concurrent Go recomp toolchain
│   ├── docs/                  # per-project integration/config/runtime guides
│   ├── internal/buildgui/     # the local browser builder served by `snesbuild gui`
│   ├── packaging/             # builds the self-contained per-platform bundles
│   └── runtime/               # bundled C runtime + SNES hardware model
├── tools/
│   ├── regen.sh                # compatibility launcher for Go snesbuild
│   ├── rom_info.py, quintet_lzss.py, ... — game/trace analysis tools
│   └── oracle/                 # differential-testing harness vs. real snes9x
├── tests/                      # golden-image + replay regression tests
├── release/                    # produced distribution bundles — NOT committed
└── saves/                      # runtime output only (dumps, snapshots,
                                   replays) — NOT committed, purely local
```

## Where to read first

| If you need to… | Read |
|---|---|
| Diagnose a problem | [`DEBUG.md`](../DEBUG.md) |
| Understand game architecture and hardware seams | [`SEAMS.md`](SEAMS.md) |
| Look up candidate symbols | [`research-symbol-map.md`](research-symbol-map.md) |
| Check current playability and subsystem status | [`progress.md`](progress.md) |
| Change the settings system or overlay | [`settings-system.md`](settings-system.md) |
| Write or restructure code | [`code-style.md`](code-style.md) |
| Find an existing design or implementation plan | [`specs/README.md`](../specs/README.md) |

Documents under `docs/` describe the living system. Files under `specs/` record
work at a point in time and may be historical; use their index for current
status.

## What can (and can't) be committed here

This repository mixes original engineering with material that can be derived
from the copyrighted ROM. `.gitignore` enforces much of the boundary, but
contributors must still understand it.

**Never commit:**

- The ROM itself (`*.sfc`/`*.smc`), any save file derived from it (`*.srm`),
  or any raw memory/WRAM/SRAM dump captured while running it (`saves/`,
  `*.bin`) — these can contain decompressed copyrighted assets (graphics,
  text, audio) that were resident in memory at capture time.
- **`src/gen/*.c` and `recomp/funcs.h`** — the actual recompiled C output.
  This is a direct, literal translation of the copyrighted ROM's machine code.
  It's regenerated locally by `snesbuild regen` from your own ROM; there is
  nothing to commit here, ever.
- Audio/video recordings captured from a running instance (`*.wav`, replay
  recordings) — same reasoning as memory dumps, but for audio/video instead.
- Asset-pack files under `game-assets/` (`hd/*.png`, `audio/*.ogg`) — a rip of
  the original soundtrack or art is copyrighted content even after
  re-encoding, and even a fully original fan arrangement is yours, not this
  repo's, to distribute. The tracked `manifest.ini` (our own hook metadata) is
  the only file that belongs in git there.
- Trace/log files from debugging sessions (`*.jsonl`, `*.trace`, `*.log`,
  `*.cdl`) — these encode per-frame memory-state traces from actual gameplay.
- Prebuilt third-party binaries (e.g. the `snes9x_libretro.dylib` reference
  core used by the oracle test harness) — vendoring compiled binaries of
  someone else's project is both bad practice and, for GPL-licensed code like
  snes9x, a license-compliance problem on top of it. Fetch your own copy per
  `tools/oracle/README.md`.

**Safe to commit:**

- `recomp/*.cfg` — these are our own hand-written addresses, directives, and
  commentary that *tell* the recompiler what to do. They don't contain any
  translated ROM logic themselves, similar to how a symbol map or linker
  script isn't itself a copy of a binary.
- All hand-written project runtime/tooling source (`src/*.c` outside
  `src/gen/`, everything under `tools/`) — original engineering, not
  ROM-derived. `snesrecomp-go/` also contains no ROM-derived game data, but it
  has separate upstream provenance and licensing status documented in
  `snesrecomp-go/ATTRIBUTION.md`.
- `docs/`, `DEBUG.md` — our own analysis and documentation. Short illustrative
  disassembly snippets in service of explaining architecture are fine and
  normal for this kind of documentation; wholesale reproduction of ROM data
  tables is not, and none currently exists in these docs.

If you're ever unsure whether something is safe to commit, default to **not**
committing it and ask first — a `.gitignore` fix is trivial; scrubbing
something out of published git history is not.
