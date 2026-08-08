# Contributing — building from source and working in this repo

This is the developer-facing companion to the [README](../README.md). It covers
what a recomp actually is, how to build from a source checkout, where everything
lives, and the one rule that matters most here: what can and cannot be committed.

**Contents**

- [Recomp, not decomp](#recomp-not-decomp)
- [Building from source](#building-from-source)
- [Producing the distribution bundles](#producing-the-distribution-bundles)
- [Project layout](#project-layout)
- [Where to read first](#where-to-read-first)
- [What can (and can't) be committed here](#what-can-and-cant-be-committed-here)

## Recomp, not decomp

This is a **static recompilation ("recomp")**, not a decompilation ("decomp") —
the distinction matters:

- A **decompilation** is a hand-written, from-scratch reimplementation: someone
  reads the original binary (or its disassembly), understands *what* it does, and
  writes new, original source code that reproduces that behavior. The result is
  new expression of the same functionality.
- A **static recompilation** mechanically translates the ROM's actual 65816
  machine code into equivalent C, one function at a time, via an automated tool
  ([`snesrecomp-go`](../snesrecomp-go/README.md), the bundled Go reimplementation
  of the historical SNES static recompiler). The output is a direct, literal
  translation of the original binary's logic — not new authorship. That generated
  code is copyrighted-ROM-derived and is **never committed to this repo** (see
  below); it's regenerated locally by everyone who builds this project, from
  their own ROM.

Layered on top of the recompiled game logic is a hand-written runtime — SDL3
windowing/input, a PPU/APU (video/audio) reimplementation, save-state handling,
and a growing set of "HLE" (high-level emulation) shims that replace timing- or
hardware-dependent ROM routines with equivalent native code. That runtime layer
*is* original engineering and *is* what's tracked in this repo.

### Why recomp instead of just running it in an emulator?

An emulator interprets or JIT-compiles the original ROM on the fly, forever
depending on an emulation core. A static recomp instead produces a real, native,
standalone executable — no core, no interpretation loop, no per-instruction
overhead — that can be profiled, debugged, and modernized (widescreen, higher
resolutions, better performance, native ports) like any other codebase, while
still requiring the end user to legally own the original game. That's the
long-term preservation case: as SNES hardware and even software emulators age
out, a native recompilation is far more portable and maintainable than either.

## Building from source

Most people should not do this — the [released builder
bundle](https://github.com/DerrickGold/ar-recomp/releases) does all of it with a
GUI and no toolchain install. Build from a checkout if you want to change the
code.

### Dependencies

- **Go 1.24+** — builds the recompiler/driver; required for both build paths
- **CMake** ≥ 3.16 — for the developer CMake presets (not needed for `--hermetic`)
- **A C11 compiler** (clang or gcc) — likewise (the hermetic path uses its own
  bundled Zig instead)
- **SDL3** (development package/headers) — the only external library this links
  against; auto-discovered by both build paths, and bundled outright in the
  distribution package (so end users need nothing)
- **git**

Python is optional for unrelated forensic/triage scripts; it is not a build,
regeneration, runtime, or opcode-validation dependency.

The `brew`/`apt` lines below install the CMake-preset build's dependencies. The
hermetic path drops the CMake and C-compiler requirements (it uses its own
bundled Zig), so it needs only Go and SDL3 development files — and the
distribution bundle removes even the SDL3 requirement by carrying it inside.

**macOS** (verified — the primary development platform):

```sh
brew install cmake sdl3 go
```

**Linux** (Debian/Ubuntu — the Steam Deck bundle is built and played on
regularly, so Linux x86_64 is a confirmed target; this from-source CMake path on
a general distro is untested but the build has no OS-specific code beyond
standard SDL3/POSIX):

```sh
sudo apt install build-essential cmake libsdl3-dev golang-go
```

**Windows**: the bundled runtime has MSVC-oriented support (see
`snesrecomp-go/runtime/runner.cmake`), but this specific project
hasn't been built on Windows yet — no `.vcxproj`/CI verifying it works here.
If you get it building, a PR documenting the steps would help.

### Steps

1. **Clone this repo.** The Go recompiler and C runtime are included; there is
   no secondary toolchain checkout or symlink to create.

2. **Supply your own ROM.** Place a verified `ar.sfc` (see the checksums in the
   [README](../README.md#what-you-need)) at the repo root.

3. **Regenerate the recompiled banks.** The cross-platform `snesbuild` driver
   runs the recompiler over your ROM, refreshes `src/gen/*.c`, `recomp/funcs.h`,
   metadata, RTS-web census, and the hard-stub report. From a source checkout:

   ```sh
   go -C snesrecomp-go run ./cmd/snesbuild regen \
     --root .. --rom ar.sfc --allow-stubs
   ```

   A downloaded `snesbuild`/`snesbuild.exe` can run the same operation directly
   without Go or Bash. `bash tools/regen.sh` remains a compatibility command.
   The inherited hard-stub backlog currently makes strict regeneration exit
   nonzero after writing complete output; see `DEBUG.md` §8.

4. **Compile the game.** Two options:

   - **Hermetic (no CMake/compiler/SDL install)** — compiles with a pinned Zig
     toolchain that `snesbuild` downloads on first use, discovering SDL3
     automatically:

     ```sh
     go -C snesrecomp-go run ./cmd/snesbuild toolchain fetch   # one time
     go -C snesrecomp-go run ./cmd/snesbuild build --hermetic --root ..
     ```

   - **CMake presets (the classic developer build)** — needs CMake, a C11
     compiler, and SDL3 development files:

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

Steps 2–4 are exactly what the player-facing `run-build` bundle script
automates through its local graphical builder.

To reuse the bundled toolchain from another game project, start with
[`snesrecomp-go/README.md`](../snesrecomp-go/README.md) and its
[project integration guide](../snesrecomp-go/docs/PROJECT_INTEGRATION.md).
The native project-driver design, the hermetic build, and the self-contained
distribution bundles are documented in
[`BUILD_TOOLING.md`](BUILD_TOOLING.md).

## Producing the distribution bundles

To produce all seven bundles from a source checkout (into `release/`, named
`actraiser-recomp-<platform>.{tar.xz,zip}` with SHA-256 sidecars):

```sh
make release
```

Go and CMake are the only host requirements — the C toolchain and SDL3 are
downloaded and bundled automatically. Because the Go module is CGO-free, every
platform cross-builds from one machine. Full details, layout, and the current
signing/CI gaps are in [`BUILD_TOOLING.md`](BUILD_TOOLING.md).

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

`DEBUG.md`, `docs/SEAMS.md`, `docs/research-symbol-map.md`, and
`docs/progress.md` are the documents worth reading before diving into the code:
`DEBUG.md` tells you *how to diagnose* a problem (which tool, which env var,
which known gotcha), `SEAMS.md` tells you *what the game's internal architecture
actually is* (object systems, dispatch tables, subsystem boundaries) as
reverse-engineered so far, and `progress.md` tells you *what actually works
today* (playability per stage/town + codebase metrics).
`docs/settings-system.md` records the architecture and implementation of the
live settings registry, persistent user settings, and host-side overlay UI.

`docs/code-style.md` is the one to read before *writing*: size budgets, how to
derive a file split and declare its seam, where data belongs, and the
verification tiers a render-affecting change has to clear. It also carries the
current debt register, so you can tell deliberate structure from accumulated
sprawl.

`specs/README.md` is the fourth: `docs/` is what is *continuously true*, while
`specs/` is work at a point in time — proposed, in progress, or done and kept
for the reasoning behind the code. Read its index before starting a change in an
area a spec already covers.

## What can (and can't) be committed here

This repo mixes original engineering (safe to commit) with content mechanically
derived from the copyrighted ActRaiser ROM (must never be committed). The
`.gitignore` enforces this, but the reasoning is worth understanding if you're
contributing:

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
