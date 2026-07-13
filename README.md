# ActRaiser Recomp

A native, static-recompilation port of **ActRaiser (SNES, USA release)** to modern
desktop platforms.

![title screen](/assets/title.png)

## What is ActRaiser?

*ActRaiser* is a 1990 Quintet-developed, Enix-published SNES game that alternates
between side-scrolling action-platformer stages and a top-down "god game" town
simulation. This project targets the verified USA cartridge dump:

| | |
|---|---|
| Internal title | `ACTRAISER-USA` |
| Size | 1,048,576 bytes (1MB, no copier header) |
| Internal checksum | `0x83DB` |
| SHA-256 | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| SHA-1 | `e8365852cc20178d42c93cd188a7ae9af45369d7` |
| CRC32 | `0xEAC3358D` |

**The ROM is not included and never will be.** You must supply your own legally
obtained `ar.sfc` dump matching the checksums above, placed at the repo root. See
[What can (and can't) be committed here](#what-can-and-cant-be-committed-here).

## What is this project?

This is a **static recompilation ("recomp")**, not a decompilation ("decomp") —
the distinction matters:

- A **decompilation** is a hand-written, from-scratch reimplementation: someone
  reads the original binary (or its disassembly), understands *what* it does, and
  writes new, original source code that reproduces that behavior. The result is
  new expression of the same functionality.
- A **static recompilation** mechanically translates the ROM's actual 65816
  machine code into equivalent C, one function at a time, via an automated tool
  ([snesrecomp](https://github.com/DerrickGold/snesrecomp), our fork of an
  existing SNES static recompiler). The output is a direct, literal translation
  of the original binary's logic — not new authorship. That generated code is
  copyrighted-ROM-derived and is **never committed to this repo** (see below);
  it's regenerated locally by everyone who builds this project, from their own
  ROM.

Layered on top of the recompiled game logic is a hand-written runtime — SDL2
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

### Current status

**Actively in development — expect bugs.**
**[`docs/progress.md`](docs/progress.md) is the authoritative, kept-current
status tracker** — per-action-stage / per-sim-town playability tables plus
automated codebase metrics; update it whenever a stage/town is actually played.
Snapshot as of 2026-07-12:

- **Fillmore (kingdom 1)**: full clean round — act 1 → sim mode → act 2 —
  including development cycles, story events, lair-seal cutscenes, rewards,
  magic, and widescreen action-stage rendering.
- **Bloodpool (kingdom 2)**: both action stages and the act-2 boss have been
  completed in widescreen; its sim entry/lightning event is confirmed, while
  the full town round remains.
- **Kasandora (kingdom 3)**: an instrumented act-1 widescreen pass is captured;
  act 2 and its simulation town remain unverified.
- **Aitos, Marahna, Northwall, and Death Heim**: static action-handler preflight
  is complete, but direct playthroughs remain. The current local generated build
  includes the 43-entry handler batch needed for that matrix.
- Open bugs and investigation state live in [`DEBUG.md`](DEBUG.md).


Extra features WIP:
* Proper-ish Widescreen support

#### Screenshots

![menu](/assets/menu.png)
![mode7](/assets/mode7.png)
![fillmore boss](/assets/fillmore-boss.png)
![fillmore sim](/assets/sim.png)

## AI disclosure

**This project is being built with substantial help from AI coding assistants**
(Claude / Claude Code). Large portions of the recompiler tooling, the runtime
code, the debugging infrastructure, and this documentation were written by AI
under my direction and review, not typed by me line-by-line.

This project is not intended to be the following:

* A showcase of technical skill or understanding
* Any form of creative expression (it's a literal translation of already written code from one platform to another)
* A pathway to fame or fortune

The entire goals of this project are:

* Another form of game preservation
* I can play the game sooner


If you're evaluating this codebase's provenance or reviewing a contribution, assume AI
involvement throughout unless told otherwise.

## Project layout

```
ActRaiserRecomp/
├── ar.sfc                  # (you supply this — gitignored)
├── config.ini               # default runtime config (player-facing)
├── dev-config.ini           # development config: debug flags + cheats enabled
├── nocheats-config.ini      # like dev-config.ini, cheats off
├── CMakeLists.txt
├── DEBUG.md                  # ★ the debugging guide — every tool, every known
│                                bug class, and the full bug-hunt journal.
│                                Start here if something's broken.
├── docs/
│   ├── SEAMS.md               # ★ logic↔hardware boundary map + reverse-
│   │                             engineered game architecture (object systems,
│   │                             dispatch tables, subsystem roles) — the
│   │                             groundwork for a future full decompilation.
│   ├── ram-map.md             # WRAM address reference
│   ├── rom-map.md             # ROM data-region reference
│   └── progress.md            # ★ per-action-level / per-sim-mode-town /
│                                 major-functionality status tracker
├── recomp/
│   ├── bank*.cfg              # hand-authored per-bank recompiler directives:
│   │                             function addresses, entry m/x width pins,
│   │                             indirect-dispatch tables, HLE hooks. This is
│   │                             the actual authored "source" that drives
│   │                             regeneration — safe to commit (see below).
│   └── funcs.h                # generated from the *.cfg files — NOT committed
├── src/
│   ├── main.c                 # SDL2 entry point, input, frame loop, config
│   ├── actraiser_rtl.c        # game-specific HLE/runtime glue + cheats
│   ├── actraiser_spc_player.c # SPC/audio upload handling
│   ├── config.c               # .ini parsing
│   └── gen/                   # ★ regenerated C output — NOT committed (you
│                                 produce this locally via tools/regen.sh)
├── third_party/
│   └── snesrecomp/            # our fork of the recompiler engine + runtime
│                                 (separate git repo, gitignored here — clone
│                                 it yourself, see Build below)
├── tools/
│   ├── regen.sh                # the regen pipeline — run this after cloning
│   ├── rom_info.py, lzss_decompress.py, opcode_diff.py, ... — analysis tools
│   └── oracle/                 # differential-testing harness vs. real snes9x
├── tests/                      # golden-image + replay regression tests
└── saves/                      # runtime output only (dumps, snapshots,
                                   replays) — NOT committed, purely local
```

`DEBUG.md`, `docs/SEAMS.md`, and `docs/progress.md` are the three documents worth
reading before diving into the code: `DEBUG.md` tells you *how to diagnose* a
problem (which tool, which env var, which known gotcha), `SEAMS.md` tells you
*what the game's internal architecture actually is* (object systems, dispatch
tables, subsystem boundaries) as reverse-engineered so far, and `progress.md`
tells you *what actually works today* (playability per stage/town + codebase
metrics).

## Build instructions

### Dependencies

- **CMake** ≥ 3.16
- **A C11 compiler** (clang or gcc)
- **SDL2** (development package/headers, not just the runtime library) — the
  only external library this links against
- **Python 3**, standard library only — the regen pipeline (`tools/regen.sh`
  and everything it calls in `third_party/snesrecomp/`) imports nothing
  outside the stdlib, so there's no `pip install` step
- **git**

**macOS** (verified — this is the only platform actually built/tested so far):

```sh
brew install cmake sdl2 python3
```

**Linux** (Debian/Ubuntu — same CMake setup, untested by this project but
should work; the build has no OS-specific code beyond standard SDL2/POSIX):

```sh
sudo apt install build-essential cmake libsdl2-dev python3
```

**Windows**: the underlying `snesrecomp` engine supports MSVC (see
`third_party/snesrecomp/runner/runner.cmake`), but this specific project
hasn't been built on Windows yet — no `.vcxproj`/CI verifying it works here.
If you get it building, a PR documenting the steps would help.

### Steps

1. **Clone this repo**, then clone the recompiler engine into `third_party/`
   (it's a separate repo, not a submodule — a `snesrecomp -> third_party/snesrecomp`
   symlink at the repo root already points here):

   ```sh
   git clone https://github.com/DerrickGold/snesrecomp third_party/snesrecomp
   cd third_party/snesrecomp && git checkout actraiser-main && cd ../..
   ```

2. **Supply your own ROM.** Place a verified `ar.sfc` (see the checksums above)
   at the repo root.

3. **Regenerate the recompiled banks.** This runs the recompiler over your ROM
   using the directives in `recomp/*.cfg` and writes `src/gen/*.c` +
   `recomp/funcs.h` — required before building, and not something you can skip
   (those files are intentionally not committed):

   ```sh
   bash tools/regen.sh
   ```

4. **Build:**

   ```sh
   mkdir build && cd build
   cmake ..
   cmake --build . -j
   ```

   `CMakeLists.txt` will fail loudly with setup instructions if `src/gen/` is
   still empty at configure time.

## Running the game

```sh
./build/ActRaiserRecomp ar.sfc --config config.ini        # normal play
./build/ActRaiserRecomp ar.sfc --config dev-config.ini    # cheats + debug flags on
```

`--config <file>` **replaces** the default `config.ini` load entirely (the file
you pass carries its own `[Graphics]`/`[Sound]` sections too), so
`dev-config.ini` and `nocheats-config.ini` are complete, self-contained configs,
not overlays.

### What's actually wired up in the config file

Only what's listed below is read by `config.c`. **`config.ini`'s `[KeyMap]` and
`[GamepadMap]` sections, and the `Autosave`/`DisableFrameDelay`/`SkipLauncher`/
`EnableSnes9xOracle`/`WindowSize`/`IgnoreAspectRatio` keys, are currently
placeholders — none of them are parsed or have any effect.** Gamepad input isn't
implemented at all yet.

**Real config keys** (`[Graphics]`/`[Sound]`):

| Key | Effect |
|---|---|
| `WindowScale` | integer window scale factor |
| `Fullscreen` | fullscreen toggle |
| `NewRenderer` | use the newer rendering path |
| `NoSpriteLimits` | disable the SNES's per-scanline sprite limit |
| `LinearFiltering` | linear texture filtering |
| `EnableAudio`, `AudioFreq`, `AudioChannels`, `AudioSamples` | audio output settings |

**Keyboard controls are hardcoded** (not configurable via `.ini` yet) —
see `HandleInput()` in `src/main.c`:

| Key(s) | Function |
|---|---|
| Arrow keys | D-pad |
| `Z` | primary action button (SNES B) |
| `X`, `A`, `S`, `Q`, `W`, Return, Right Shift | remaining face/shoulder buttons and Start/Select — exact SNES button-to-key mapping isn't documented here yet; see `HandleInput()` for the precise bit values |
| `Esc` | quit |
| `P` | pause |
| `T` | turbo — fast-forward at 8 game frames per rendered frame (`AR_TURBO_MULT` to change) |
| `F5` / `F7` | save / load state |
| `F6` | level warp (see `AR_WARP` below) |
| `F2` | full diagnostic snapshot (WRAM/VRAM/CGRAM/OAM + screenshot) |
| `F9` | dump diagnostic state |

**Cheats** (`[Cheats]` section — any `AR_*`/`SNESREF_*` key in the `.ini` is
exported as an environment variable, which is how these are read):

| Key | Effect |
|---|---|
| `AR_INF_HP=1` | pins player HP — infinite health |
| `AR_FREEZE_TIMER=1` | freezes the action-stage countdown timer. **Currently still buggy** — has an auto-release heuristic for the boss-defeat drain sequence that isn't fully reliable yet |
| `AR_MOONJUMP=1` (or `=<n>` for px/frame) | hold the jump button to fly upward |
| `AR_NO_KNOCKBACK=1` | permanent invincibility — no damage, no hitstun. Magic-aware: invulnerability drops only for the 1-2 frames where a spell cast actually fires |
| `AR_ALL_MAGIC=1` | unlocks all four spells in the equip menu |
| `AR_RANGED_SWORD=1` | sword fires a projectile |
| `AR_INF_MP=1` (or `=<n>`) | infinite magic scrolls (pins the working count; never written to the save file) |
| `AR_INF_SP=1` | sim mode: infinite SP (miracle points), self-calibrating to your max |
| `AR_ANGEL_HP=1` | sim mode: infinite angel health, self-calibrating to your max |
| `AR_PIN=<8-hex-PAR>[,...]` | generic Pro Action Replay code pinner (e.g. `7E00210A`); catalogue in `codes.txt` / `docs/ram-map.md` |
| `AR_WARP=<region_hex><act_hex>` | sets the `F6` warp target (default `0101` = Fillmore act 1); only takes effect from a transition-capable state |
| `AR_TURBO_MULT=<n>` | game frames per rendered frame while `T` turbo is on (default 8) |

Everything else in `dev-config.ini`'s `[Debug]` section is diagnostic
instrumentation for active bug-hunting, documented in `DEBUG.md` — not
gameplay-relevant, off by default, and safe to ignore unless you're debugging.

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
  It's regenerated locally by `tools/regen.sh` from your own ROM; there is
  nothing to commit here, ever.
- Audio/video recordings captured from a running instance (`*.wav`, replay
  recordings) — same reasoning as memory dumps, but for audio/video instead.
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
- All hand-written runtime/tooling source (`src/*.c` outside `src/gen/`,
  everything under `tools/`, the `third_party/snesrecomp` engine fork) —
  original engineering, not ROM-derived.
- `docs/`, `DEBUG.md` — our own analysis and documentation. Short illustrative
  disassembly snippets in service of explaining architecture are fine and
  normal for this kind of documentation; wholesale reproduction of ROM data
  tables is not, and none currently exists in these docs.

If you're ever unsure whether something is safe to commit, default to **not**
committing it and ask first — a `.gitignore` fix is trivial; scrubbing
something out of published git history is not.

## License

This repo's original source (runtime, tooling, `recomp/*.cfg`, docs) is
[MIT-licensed](LICENSE). That license explicitly does **not** cover the
ActRaiser ROM or anything derived from it — see the LICENSE file's Scope
section, and "What can (and can't) be committed here" above.

The recompiler engine, [`snesrecomp`](https://github.com/DerrickGold/snesrecomp)
(cloned separately into `third_party/`, not distributed with this repo), is
currently unlicensed — no license has been declared there yet.
