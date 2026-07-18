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

- Go 1.24 or newer for the recompiler and its tests.
- A C11 compiler and CMake 3.16 or newer for a game executable.
- SDL2 development headers/libraries for the bundled runtime's input layer.
- A legally obtained, local ROM for the game being recompiled.

The Go module has no third-party Go dependencies.

## Build the tool

From this directory:

```sh
go test ./...
mkdir -p build
go build -o build/v2regen ./cmd/v2regen
```

The resulting `build/v2regen` binary can be invoked from any game project as
long as all project paths are passed explicitly.

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

Build the tool once, then run the strict generation pipeline from the game
project root:

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

Generated C includes `cpu_state.h`, `cpu_trace.h`, `common_cpu_infra.h`, and
`funcs.h`. The game target must therefore include `runtime/src`,
`runtime/src/snes`, and its own `recomp` directory. A minimal CMake pattern is:

```cmake
set(SNESRECOMP_GO_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp-go")
include("${SNESRECOMP_GO_ROOT}/runtime/runner.cmake")

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
policy, and any C functions named by `hle_func`/`hle_dispatch`.
See [`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md) for the full
contract and [`docs/CFG_FORMAT.md`](docs/CFG_FORMAT.md) for bank directives.

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

Run `v2regen help` or `v2regen <command> -h` for every option.

## Documentation

- [`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md): project layout,
  generation pipeline, CMake, runtime hooks, and redistribution rules.
- [`docs/CFG_FORMAT.md`](docs/CFG_FORMAT.md): supported `bankNN.cfg` syntax.
- [`docs/RUNTIME.md`](docs/RUNTIME.md): shared runtime boundary, optional
  features, and current limitations.
- [`ATTRIBUTION.md`](ATTRIBUTION.md): Python-source provenance, prior work,
  contributor credit, and licensing status.

## Licensing

The historical source repository had not declared an overall license at the
snapshot used for this port. Attribution is not a substitute for a license.
Until that status is clarified, do not assume this directory is covered by the
ActRaiserRecomp repository's MIT grant. See [`LICENSE`](LICENSE),
[`ATTRIBUTION.md`](ATTRIBUTION.md), and
[`runtime/README.md`](runtime/README.md).
