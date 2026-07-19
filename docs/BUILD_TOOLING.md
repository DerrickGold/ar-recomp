# Cross-platform build tooling

ActRaiserRecomp uses `snesbuild`, a native Go project driver, for regeneration
and CMake orchestration. The command is deliberately separate from `v2regen`:
`v2regen` exposes individual recompiler operations, while `snesbuild` owns the
ordered per-project workflow and host-tool invocation.

## Current commands

When working from source, run the driver from the repository root through Go:

```sh
go -C snesrecomp-go run ./cmd/snesbuild doctor --root .. --rom ar.sfc
go -C snesrecomp-go run ./cmd/snesbuild regen --root .. --rom ar.sfc
go -C snesrecomp-go run ./cmd/snesbuild build --root ..
```

The current inherited stub backlog makes strict `regen` exit nonzero after all
outputs and sidecars are complete. `--allow-stubs` is available for local work
on the existing backlog; new release automation should remain strict.

Build a reusable host binary with:

```sh
go -C snesrecomp-go build -o build/snesbuild ./cmd/snesbuild
```

Because `-C snesrecomp-go` changes the output base, that command creates
`snesrecomp-go/build/snesbuild`. It can then be invoked without Go or Bash:

```sh
snesrecomp-go/build/snesbuild all \
  --root . --rom ar.sfc --allow-stubs
```

On Windows the equivalent downloaded or locally built executable is:

```powershell
snesbuild.exe all --root . --rom ar.sfc --allow-stubs
```

`tools/regen.sh` and `tools/build-macos.sh` are compatibility launchers for
existing developer muscle memory. They prefer `$SNESBUILD` or
`snesrecomp-go/build/snesbuild`, then fall back to `go run`. New automation
should call `snesbuild` directly.

## Hermetic builds (no CMake, no system compiler)

`snesbuild build --hermetic` compiles the entire game — engine runtime, game
sources, and generated banks — with a pinned [Zig](https://ziglang.org)
toolchain (`zig cc`, a self-contained clang+lld) and links the executable
directly. CMake, Xcode CLT/gcc, and SDL2 *development packages* are not
required; only the SDL2 runtime library remains an external input, and the
planned platform bundle will carry that beside the executable.

```sh
snesbuild toolchain fetch          # one-time: download + sha256-verify + extract
snesbuild build --hermetic --root . --rom ar.sfc
snesbuild all --hermetic --root . --rom ar.sfc --allow-stubs   # regen + build
```

The pinned Zig release (0.16.0) is resolved in order: `$SNESBUILD_ZIG`, the
project cache `build/toolchain/`, then `PATH`. `snesbuild toolchain status`
reports the pin, its checksum, and what is locally available. Fetch verifies
the archive against a checksum embedded in the snesbuild binary — the network
is trusted for bytes, never for content.

Inputs are split along the same boundary as the redistribution rules:

- `snesrecomp-go/runtime/runner.cmake` stays the single source of truth for
  the engine's source list (parsed directly, so the CMake and hermetic builds
  cannot drift);
- `snesbuild.ini` at the project root declares the game half: target name,
  game sources, includes, defines, and SDL2 usage. `snesbuild doctor`
  cross-checks it against the game target in `CMakeLists.txt` and warns on
  drift;
- generated banks are globbed from `src/gen` as always.

Output lands in `build/hermetic/` with flat per-source objects. Rebuilds are
incremental (source/header mtimes + a flags hash); a no-op rebuild takes well
under a second, a clean build roughly two minutes on an 8-core machine. The
CMake path (`snesbuild build`, presets, tests, sanitizers) remains the
developer workflow; hermetic is the distribution path.

## Dependency boundary

| Operation | Required on the user's machine |
|---|---|
| Run a downloaded `snesbuild doctor` or `regen` | `snesbuild`, this project, and the user's local ROM |
| Build `snesbuild` from source | Go 1.24+ |
| Compile the game today | CMake, a C11 compiler, SDL2 development files, and platform SDK/linker support |
| Run the compiled game | SDL2 runtime plus the user's local ROM |

The Go binary has no third-party Go or runtime dependencies. It uses Go's
standard library for path handling, worker selection, process execution, RTS
census deltas, and cross-platform exit status handling; it does not call
`grep`, `find`, `cp`, `readlink`, `sysctl`, or other Unix utilities.

## Self-contained distribution bundles

`snesrecomp-go/packaging/` is a standalone CMake project (it compiles no C
itself) that produces a **fully self-contained, one-click bundle per
platform**. A non-technical user downloads one archive, drops in their ROM,
and runs one script — no repository checkout, no compiler, no SDL, nothing
installed system-wide. Because the Go module is CGO-free, every platform
cross-builds from one machine.

**Build all six from the repo root, one command:**

```sh
make release
# or, pure CMake, from the packaging dir:
#   cd snesrecomp-go/packaging && cmake --workflow --preset release
# single platform:  make release-macos-arm64   (or the per-platform presets)
```

Each platform's CMake build tree holds a freshly extracted ~180 MB Zig
toolchain, so it is removed automatically as soon as that bundle is staged
(pass `KEEP_BUILD=1` to retain them for debugging). The download cache
(`snesrecomp-go/packaging/cache/`, ~420 MB of archives) is kept, so re-runs
re-extract in seconds without re-downloading. To reclaim more afterwards:
`make clean` removes every regenerable artifact (build trees, generated C, tool
binaries, release bundles) while keeping the ROM, save files, source, and that
download cache; `make clean-all` also drops the cache.

Bundles (plus `.sha256` sidecars) land in `release/`, named
`actraiser-recomp-<os>-<arch>.{tar.xz,zip}` (~55–65 MB tar.xz, ~100 MB Windows
zip).

**The bundle is built for a non-technical user, so its root is deliberately
almost empty** — everything that would intimidate is hidden under `utils/`:

```
actraiser-recomp-<platform>/
├── README.txt              plain-text instructions
├── run-build.command/.bat/.sh   the one thing to run
└── utils/                  hidden: the whole build (ignore it)
    ├── snesbuild.ini, config.ini
    ├── recomp/ src/ third_party/stb/ snesrecomp-go/runtime/   authored source (no generated C)
    ├── game-assets/        manifest template + empty audio/+hd/ (no media ships)
    ├── tools/snesbuild     the driver (stripped, git-describe-stamped)
    ├── tools/toolchain/zig-*/   pinned C compiler (Zig 0.16.0)
    ├── tools/sdl2/         bundled SDL2 (macOS, Windows x86_64)
    └── LICENSE, ATTRIBUTION.md
```

The only things deliberately absent are the ROM (the user supplies it), the
ROM-derived generated C (regenerated locally), and the media assets — the
`game-assets/manifest.ini` ships as a template mapping every song/graphic slot
so users drop in their own files. The bundled SDL2 comes from SDL's official
redistributables (universal macOS `.dmg`; Windows mingw archive), both zlib
licensed; Linux ships no SDL (no single portable redistributable) and the run
script points the user at their package manager.

**How a user runs it:** unpack the archive, drop the ROM in the folder next to
`README.txt`, and run `run-build` **once**. It builds the game via
`snesbuild all --hermetic --allow-stubs` (regen + compile with the bundled
toolchain and SDL, `--root utils`), then does the two things that make play
trivial:

1. **Copies the finished game to the root** — the executable and, on
   macOS/Windows, its bundled SDL library — so the playable result sits in the
   top folder, not buried in a build directory.
2. **Generates a `run-game` script** (`.command`/`.bat`/`.sh`) next to it. The
   user opens `run-game` to play — every time, with no rebuild. (That generated
   script is created locally, so it is not Gatekeeper-quarantined — only the
   downloaded `run-build` triggers the one-time right-click-Open on macOS.)

`snesbuild` locates the bundled Zig and SDL beside its own executable
(`utils/tools/`), and the built game links SDL via an rpath
(`@executable_path` / `$ORIGIN`) with the bundled library copied next to it, so
it runs with no system SDL. The `run-game` script runs the root binary with its
working directory set to `utils/` so the game finds `config.ini` and
`game-assets/`. This whole flow is verified end-to-end: a bundle extracted to
an empty directory outside the repo, with the dev machine's Zig/SDL/homebrew
removed from the environment, builds from the ROM, deposits the game and
`run-game` in the root, and the generated `run-game` script launches the game.

A post-package **leak gate** re-extracts every archive and fails packaging if
an unreviewed top-level entry appears (only `README.txt`, the run script, and
`utils/` are allowed) or any first-party file contains a build-machine path;
third-party payloads under `utils/tools/toolchain/`, `utils/tools/sdl2/`, and
`utils/third_party/` are exempt from the string scan. `-trimpath` keeps such
paths out of the Go binary to begin with; the gate proves it stays that way
across the whole bundle (≈145 first-party files scanned per archive).

## Remaining distribution work

1. **Signing/notarization.** macOS Gatekeeper blocks the unsigned
   `run-build.command` (right-click → Open is the current workaround); the
   binaries want signing before public release.
2. **CI** to run `make release` on a clean tagged checkout (today's archives
   carry the `-dirty` stamp) and publish the artifacts + checksums.
3. **Windows/Linux runtime validation.** All six bundles cross-build and the
   full standalone flow is verified end-to-end on macOS; the built *game* has
   only been run on macOS, so the Windows SDL2main/WinMain path and the Linux
   system-SDL fallback still need a real run on those hosts.
4. **`--allow-stubs` decision.** The one-click flow currently passes it so
   regen always completes; closing the hard-stub backlog would let the shipped
   flow drop it.
5. Optionally, a small ROM-picker/progress GUI wrapping the same Go APIs — the
   CLI + scripts remain the automation surface.

Only generic tools and runtime sources should be distributed. The ROM,
generated C, generated manifests, and the resulting ROM-derived game binary
must continue to be produced locally from the user's legally obtained ROM.
