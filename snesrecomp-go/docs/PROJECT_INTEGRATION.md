# Project integration

This guide describes the contract between the shared `snesrecomp-go` module
and a per-game project. The toolchain is game-agnostic; ROM knowledge, authored
configuration, frontend code, and game-specific runtime policy stay in the
game repository.

## Recommended layout

```text
MyGameRecomp/
├── CMakeLists.txt
├── game.sfc                    # user supplied; never committed
├── recomp/
│   ├── bank00.cfg              # one file per configured LoROM bank
│   ├── bank01.cfg
│   └── funcs.h                 # generated; never committed
├── src/
│   ├── gen/                    # generated C; never committed
│   ├── main.c                  # host frontend and ROM verification
│   ├── config.h                # optional project/frontend configuration
│   ├── config.c
│   ├── game_cpu_infra.c        # RtlGameInfo registration
│   └── game_rtl.c              # frame loop, interrupts, HLE hooks
└── snesrecomp-go/              # this module
```

The module may instead be a sibling checkout or submodule. Do not rely on a
particular relative path: set `SNESRECOMP_GO_ROOT` in CMake and pass explicit
CLI paths so either arrangement works.

## Generation pipeline

For end users and cross-platform automation, `snesbuild` owns the ordered
pipeline and all sidecars:

```sh
snesbuild regen --root . --rom game.sfc
snesbuild build --root .
# or: snesbuild all --root . --rom game.sfc
```

This path uses no shell utilities. A downloaded `snesbuild` binary also needs
no Go installation. The individual `v2regen` commands below remain useful for
CI composition and recompiler development.

Build one binary and reuse it for every stage:

```sh
mkdir -p build
(cd snesrecomp-go && go build -o ../build/v2regen ./cmd/v2regen)

build/v2regen regen \
  --rom game.sfc \
  --cfg-dir recomp \
  --out-dir src/gen \
  --jobs 8

build/v2regen sync-funcs \
  --cfg-dir recomp \
  --out recomp/funcs.h

build/v2regen stub-census --gen-dir src/gen
```

`regen` writes bank translation units, `dispatch_v2.c`, and
`unresolved_stubs_v2.c`. It converges cross-bank discovery and variant routing
before replacing output, preserves deterministic source order across workers,
and removes stale bank parts when the translation-unit split changes.

Use strict `regen` and `stub-census` in CI. During initial bring-up,
`regen --allow-stubs` writes all output while reporting unresolved control flow;
that flag is a diagnostic escape hatch, not a release mode.

Recommended additional gates are:

```sh
go -C snesrecomp-go test ./...
build/v2regen link-audit --gen-dir src/gen --src-dir src \
  --runtime-dir snesrecomp-go/runtime/src
build/v2regen rts-webs --rom game.sfc --cfg-dir recomp --suggest
```

## CMake integration

`runtime/runner.cmake` exports two variables:

- `SNESRECOMP_RUNNER_SOURCES`: shared C runtime and SNES hardware sources.
- `SNESRECOMP_RUNNER_INCLUDE_DIRS`: `runtime/src` and `runtime/src/snes`.

It also defines `SNESRECOMP_USE_CCACHE`, `SNESRECOMP_DEV_FAST_BUILD`, and
`SNESRECOMP_ENABLE_TRACE` options. The trace option compiles local trace rings
and a link stub; the historical TCP debug server is not currently enabled.

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyGameRecomp C)
set(CMAKE_C_STANDARD 11)

set(SNESRECOMP_GO_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp-go")
include("${SNESRECOMP_GO_ROOT}/runtime/runner.cmake")

find_package(SDL2 REQUIRED)
file(GLOB GAME_GEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/gen/*.c")
if(NOT GAME_GEN_SOURCES)
  message(FATAL_ERROR "Run the project regeneration script before building")
endif()

add_executable(MyGame
  ${SNESRECOMP_RUNNER_SOURCES}
  ${GAME_GEN_SOURCES}
  src/main.c
  src/config.c
  src/game_cpu_infra.c
  src/game_rtl.c)

target_include_directories(MyGame PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/recomp"
  ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
  ${SDL2_INCLUDE_DIRS})

target_compile_definitions(MyGame PRIVATE
  SNESRECOMP_TRACE=$<BOOL:${SNESRECOMP_ENABLE_TRACE}>
  SNESRECOMP_REVERSE_DEBUG=0)
target_link_libraries(MyGame PRIVATE ${SDL2_LIBRARIES} m)
```

Projects with a non-SDL frontend can omit `keybinds.c` from their local source
list and provide equivalent host integration, but `runner.cmake` intentionally
represents the currently tested shared runtime configuration.

## Required project layer

The shared runtime supplies CPU state, LoROM memory routing, PPU/APU/DSP/SPC,
DMA, save-state primitives, tracing hooks, and generated dispatch support. It
does not supply a complete application. A game project owns:

1. **ROM handling.** Resolve a user-provided path, verify the expected
   checksum/region/revision, then pass the bytes to `SnesInit`. Never compile or
   package the ROM into the executable.
2. **Frontend.** Create the window/audio device, map input, present the PPU
   framebuffer, and drive `RtlRunFrame` or the project's equivalent loop.
3. **Project configuration.** Define any config types, files, and settings the
   chosen frontend needs. The shared runtime does not impose a config schema.
4. **Game registration.** Define a stable `RtlGameInfo`, call
   `RtlRegisterGame(&game_info)` before `SnesInit`, and keep the structure alive
   for the process lifetime.
5. **Frame/interrupt policy.** Supply `run_frame`, optional `draw_ppu_frame`,
   reset entry, NMI/IRQ invocation, and any coroutine/yield policy.
6. **HLE hooks.** Every C symbol named by `hle_func` or `hle_dispatch` in a cfg
   must be implemented by the game project with the generated `CpuState *`
   ABI.
7. **Game-specific hardware workarounds.** Put ROM-address policy in the game
   layer. `RtlGameInfo.read_rdnmi` may override a `$4210` read (return `-1` for
   shared behavior), and `recover_dispatch_miss` may opt verified dispatch
   sites into the shared recovery mechanism. Leave both NULL unless needed.

A minimal registration unit looks like:

```c
#include "common_cpu_infra.h"
#include "game_rtl.h"

const RtlGameInfo kGameInfo = {
  .title = "my_game",
  .initialize = NULL,
  .run_frame = &RunOneFrameOfGame,
  .draw_ppu_frame = NULL,
  .read_rdnmi = NULL,
  .recover_dispatch_miss = NULL,
  .save_name_prefix = "save",
};
```

The generated ABI is `RecompReturn Function_MxXx(CpuState *cpu)`. Do not invent
per-function return structs or pass CPU registers as C parameters; mutate the
shared `CpuState` exactly as generated code does.

## Hermetic builds (`snesbuild build --hermetic`)

The hermetic path compiles every translation unit with a pinned Zig toolchain
and links the executable itself — no CMake and no system compiler. It reads
two inputs the CMake path also uses, plus one manifest it owns:

- `runtime/runner.cmake` — the engine's source/include lists (parsed
  directly; the first unconditional `set(...)` block of each variable). The
  `SNESRECOMP_ENABLE_TRACE` developer sources are never part of hermetic
  builds.
- `src/gen/*.c` — globbed as always.
- `snesbuild.ini` at the project root — the game half:

```ini
[project]
name = MyGame          # executable name (must match the CMake target
                       # for doctor's drift cross-check)
std = c11
sdl2 = true            # discover SDL2 headers/libs and link -lSDL2
link = -lm             # extra linker args, repeatable

define = MY_FLAG=0     # repeatable
include = src          # repeatable, relative to the project root
source = src/main.c    # repeatable, game translation units only
```

Toolchain resolution order: `$SNESBUILD_ZIG`, the project's
`build/toolchain/` cache (populated by `snesbuild toolchain fetch`, which
verifies the release archive against a checksum embedded in the binary),
then `PATH`. Objects and the executable land in `build/hermetic/`; rebuilds
are incremental by source/header mtime plus a compile-flags hash.

## Per-game conventions

Carry forward the source project's neutral conventions:

- executable/window title: `Full Game Name (Recompiled)`;
- default config file: `config.ini`, with optional user/local overlays;
- shared hooks use neutral names such as `RunOneFrameOfGame`;
- game-prefixed names stay in the game project, never in shared runtime APIs;
- cfg files and comments are authored source; generated C is disposable.

## Redistribution boundary

Commit authored cfg, game glue, build scripts, and documentation. Do not commit
or publish:

- ROMs (`*.sfc`, `*.smc`) or cartridge save data;
- generated `src/gen/*.c` or generated `recomp/funcs.h`;
- generated-output baseline archives or hash manifests tied to a game ROM;
- WRAM/VRAM/SRAM dumps, traces, screenshots, audio, or extracted assets; or
- prebuilt third-party emulator cores used as a differential oracle.

The tool's generic unit tests synthesize their inputs at runtime and do not
need game data. Projects can keep private generated-output snapshots under
`build/` for local parity checks without redistributing them.
