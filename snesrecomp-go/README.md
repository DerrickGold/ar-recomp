# snesrecomp-go

`snesrecomp-go` is a Go static recompiler for SNES (Super Famicom) LoROM
games. It translates configured 65816 entry points into C ahead of time and
ships the C runtime and SNES hardware model that generated code links against.

This directory is a self-contained Go module inside ActRaiserRecomp. It is not
published as a separate repository yet, but it is deliberately structured so
it can be extracted later without bringing the historical Python checkout or
any ActRaiser files with it.

No ROM, generated game code, game assets, memory dumps, or captured gameplay
data belong in this module. Supply ROMs locally and keep generated C and
baseline snapshots ignored in each game project.

## Origin and status

This implementation is a Go port of the Python `snesrecomp` project created by
Matthew Stanley and subsequently developed by its contributors. The bundled C
runtime was carried forward from the same project. Exact repositories, the
source snapshot used for this port, contributor credit, prior-project
acknowledgements, and licensing caveats are recorded in
[`ATTRIBUTION.md`](ATTRIBUTION.md).

The normal recompiler path is Go-only. It covers ROM/config loading, 65816
decode, control-flow analysis, IR lowering, C emission, variant discovery and
pruning, deterministic concurrent generation, dispatch output, `funcs.h`
synchronization, metadata, census tools, link audit, and opcode differential
testing. The Python implementation is no longer required to generate or build
a project.

During the port, Go output was verified byte-for-byte against the Python
implementation for the ActRaiser project and both implementations reached the
same generated function and hard-stub counts. Those comparison archives were
intentionally removed because generated C is derived from the user-supplied
game ROM and should not be redistributed.

## Requirements

- A downloaded `v2regen` or `snesbuild` binary needs no Go installation.
- Go 1.24 or newer is required to build the tools or run their tests from source.
- A C11 compiler, CMake 3.16+, and SDL3 development files for the traditional
  developer build. The hermetic/GUI path uses the packaged Zig and SDL3 inputs.
- A legally obtained, local ROM for the game being recompiled.

The Go module has no third-party Go dependencies.

## Build the tool

From this directory:

```sh
go test ./...
mkdir -p build
go build -o build/v2regen ./cmd/v2regen
go build -o build/snesbuild ./cmd/snesbuild
```

`v2regen` exposes individual recompiler operations. `snesbuild` is the
cross-platform project driver: it runs the complete regeneration pipeline and
can configure/build a game's CMake project. Both binaries are relocatable as
long as project paths are passed explicitly.

## Use it from a game project

A project normally keeps `snesrecomp-go` as a subdirectory and owns all
game-specific material beside it:

```text
MyGameRecomp/
├── game.sfc                 # local and ignored
├── recomp/
│   ├── bank00.cfg           # authored project input
│   └── funcs.h              # generated and ignored
├── src/
│   ├── gen/                 # generated C, ignored
│   ├── main.c               # frontend, ROM loading, frame loop
│   ├── config.h             # optional project/frontend configuration
│   └── game_runtime.c       # RtlGameInfo, HLE hooks, game policy
└── snesrecomp-go/
```

Build the tools once, then run the strict generation pipeline from the game
project root in one command:

```sh
snesrecomp-go/build/snesbuild regen --root . --rom game.sfc
```

The equivalent low-level commands are:

```sh
snesrecomp-go/build/v2regen regen \
  --rom game.sfc --cfg-dir recomp --out-dir src/gen --jobs 8

snesrecomp-go/build/v2regen sync-funcs \
  --cfg-dir recomp --out recomp/funcs.h

snesrecomp-go/build/v2regen stub-census --gen-dir src/gen
```

`regen` fails when hard stubs remain. `--allow-stubs` is available during an
initial port so the complete output can be inspected, but release/CI pipelines
should omit it.

Configure and compile a CMake-based game project with:

```sh
snesrecomp-go/build/snesbuild build --root . --config RelWithDebInfo
```

`snesbuild all` performs both phases. A prebuilt `snesbuild` removes Go and
shell interpreters from the user path. With the default CMake path, CMake, a
native compiler, the platform SDK, and the frontend's native libraries remain
build-time dependencies. The hermetic path removes all but the last of those:

```sh
snesbuild toolchain fetch                # pinned Zig, checksum-verified
snesbuild build --hermetic --root .      # zig cc + link, no CMake
```

It is driven by a `snesbuild.ini` manifest at the project root; see
[`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md).

Generated C includes `cpu_state.h`, `cpu_trace.h`, `common_cpu_infra.h`, and
`funcs.h`. The game target must therefore include `runtime/src`,
`runtime/src/snes`, and its own `recomp` directory. A minimal CMake pattern is:

```cmake
set(SNESRECOMP_GO_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp-go")
include("${SNESRECOMP_GO_ROOT}/runtime-next/runner.cmake")

file(GLOB GAME_GEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/gen/*.c")

add_executable(MyGame
  ${SNESRECOMP_RUNNER_SOURCES}
  ${GAME_GEN_SOURCES}
  src/main.c
  src/game_runtime.c)

target_include_directories(MyGame PRIVATE
  ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
  "${CMAKE_SOURCE_DIR}/recomp"
  "${CMAKE_SOURCE_DIR}/src")
```

The runtime is not a complete frontend. Each project supplies ROM validation
and loading, video/audio presentation, its `RtlGameInfo`, frame and interrupt
policy, and any C functions named by `hle_func`/`hle_func_if`/`hle_dispatch`.
See [`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md) for the full
contract and [`docs/CFG_FORMAT.md`](docs/CFG_FORMAT.md) for bank directives.

### Selectable runner

The independently authored MIT runner under `runtime-next/` is the default.
The inherited runner remains available as an explicit comparison target:

```sh
snesbuild build --root .                       # next (default)
snesbuild build --hermetic --root .            # next, isolated Zig build
snesbuild build --root . --runner legacy       # historical A/B target
```

The CMake equivalent is `-DSNESRECOMP_RUNNER=legacy|next`. The `next` manifest
contains no legacy source or include fallbacks and reports that boundary in its
runner descriptor. Its standalone module tests every core subsystem without a
ROM, generated game code, SDL, or the historical runner:

```sh
cmake -S snesrecomp-go/runtime-next -B build/runtime-next
cmake --build build/runtime-next
ctest --test-dir build/runtime-next --output-on-failure
```

Core replacement code is portable C11 and must keep OS, SDL, graphics API, and
audio API types behind host adapters. The isolated object caches remain
`build/hermetic/` for `legacy` and `build/hermetic-next/` for `next`, so the
default transition cannot reuse incompatible historical objects.

## Commands

All commands accept explicit paths and can be run from the game project root:

```sh
v2regen regen --rom game.sfc --cfg-dir recomp --out-dir src/gen --jobs 8
v2regen sync-funcs --cfg-dir recomp --out recomp/funcs.h
v2regen metadata --gen-dir src/gen --cfg-dir recomp --out build/gen_meta.json
v2regen rts-webs --rom game.sfc --cfg-dir recomp --suggest
v2regen stub-census --gen-dir src/gen
v2regen link-audit --gen-dir src/gen --src-dir src \
  --runtime-dir snesrecomp-go/runtime/src
v2regen inspect --rom game.sfc --cfg-dir recomp --jobs 8
v2regen emit-function --rom game.sfc --cfg-dir recomp \
  --bank 00 --start 8000 --m 1 --x 1
```

The opcode differential harness consumes locally cached
[Tom Harte processor tests](https://github.com/SingleStepTests/ProcessorTests):

```sh
v2regen opcode-diff --cache-dir tools/oracle/harte_cache \
  --runtime-dir snesrecomp-go/runtime/src --all
```

Generated-output snapshots are useful locally but should not be distributed
when they contain ROM-derived C:

```sh
v2regen baseline capture --source src/gen \
  --archive build/baseline/generated-src.tar.gz \
  --metadata build/baseline/generated-src.json

v2regen baseline verify --actual src/gen \
  --archive build/baseline/generated-src.tar.gz
```

A hermetic build can also target a platform this machine is not, which turns
"does it still build on Windows?" into something answerable without a Windows
box. `sdl stage` fetches and sha256-verifies the same pinned SDL3
redistributable the platform bundle ships, so the link is the real one:

```sh
snesbuild sdl stage  --root . --target x86_64-windows-gnu
snesbuild build --hermetic --root . --target x86_64-windows-gnu
```

Each target keeps a separate object tree under `build/hermetic/<target>/` for
legacy or `build/hermetic-next/<target>/` for next, so cross checks, runner
comparisons, and the native build stay independently incremental. A cross
build never falls back to the host's SDL3; pass
`--sdl-include`/`--sdl-lib` for targets with no official redistributable to
stage.

Run `v2regen help` or `v2regen <command> -h` for every option.
Run `snesbuild help` or `snesbuild <command> -h` for project-driver options.

### Local audio previews

The `snesbuild` binary also contains an independently authored, pure-Go
SPC700/S-DSP audio-only emulator for ROM-owner preview generation:

```sh
snesbuild audio-preview --rom ../game.sfc --out ../build/audio-previews \
  --seconds 30 --tracks title-theme,song-00
```

The GUI invokes the same package from its Assets tab and writes stereo WAVs to
the per-user cache. It has no dependency on `runtime/`, a browser SPC player,
FFmpeg, or a system audio converter. The generated WAVs are ROM-derived game
content and are intentionally outside the module's MIT grant.

## Distribution packaging

`packaging/` is a standalone CMake project that builds a **fully
self-contained, one-click bundle per platform**: the whole buildable game
project plus the build machinery (`tools/snesbuild`, the pinned Zig toolchain,
and the supported SDL3 redistributables) and a `run-build` script. A user
unpacks it and runs the script, which opens the local graphical ROM picker and
build log — no repository checkout or developer tools required.

```sh
make release                                    # from the game repo root, all platforms
# cd packaging && cmake --workflow --preset release   # pure-CMake equivalent
```

Go is CGO-free, so every platform cross-builds from one machine. Bundles are
named `actraiser-recomp-<os>-<arch>.{tar.xz,zip}` and written to the repo's
`release/`. They contain only generic tools, the project's own authored
source, and redistributable third-party components — never a ROM, generated C,
or media assets (the asset manifest ships as an empty template). See the game
repository's `docs/BUILD_TOOLING.md` for the full bundle contract.

## Documentation

- [`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md): project layout,
  generation pipeline, CMake, runtime hooks, and redistribution rules.
- [`docs/CFG_FORMAT.md`](docs/CFG_FORMAT.md): supported `bankNN.cfg` syntax.
- [`docs/RUNTIME.md`](docs/RUNTIME.md): shared runtime boundary, optional
  features, and current limitations.
- [`ATTRIBUTION.md`](ATTRIBUTION.md): Python-source provenance, prior work,
  contributor credit, and licensing status.

## Licensing

The original Go implementation, tooling, tests, and documentation in this
module are [MIT-licensed](LICENSE). The inherited C runner under `runtime/` is
explicitly outside that grant and retains its current unresolved written-
license status; see [`runtime/LICENSE`](runtime/LICENSE),
[`runtime/README.md`](runtime/README.md), and [`ATTRIBUTION.md`](ATTRIBUTION.md).

Game ROMs, generated/recompiled ROM code, extracted previews, and embedded
retail media are not relicensed under MIT.
