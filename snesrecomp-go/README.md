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

The recompiler is a Go port of the Python `snesrecomp` project created by
Matthew Stanley and subsequently developed by its contributors. The bundled C
runner under `runtime/` is the project-owned replacement for the retired
comparison runner, which is not distributed. Its S-DSP accuracy core includes
an attributed MIT-licensed adaptation from Snaggletooth; the rest of that
boundary is explicit rather than implied. Exact repositories, the source
snapshot used for the Go port, contributor credit, prior-project
acknowledgements, and licensing boundaries are recorded in
[`ATTRIBUTION.md`](ATTRIBUTION.md) and
[`runtime/PROVENANCE.md`](runtime/PROVENANCE.md).

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
│   └── game_runtime.c       # RtlGameModule, HLE hooks, game policy
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

Before changing generated output, compare independently inferred dispatch
facts with authored cfg in a read-only shadow pass:

```sh
snesrecomp-go/build/snesbuild analyze --root . --rom game.sfc
# or: v2regen analyze --rom game.sfc --cfg-dir recomp --format json
```

See [`docs/ANALYSIS.md`](docs/ANALYSIS.md) for comparison semantics, the
entry/continuation model, and the cfg-removal validation gate.

For memory-producer investigation, use the same decoded instruction index
rather than scanning raw ROM bytes:

```sh
snesrecomp-go/build/snesbuild xref '$1C' --root . --rom game.sfc
snesrecomp-go/build/snesbuild xref '$00:C210' --root . --rom game.sfc
```

When a handler address comes from script or object data rather than a ROM code
reference, a trace build can capture a compact runtime census and report the
missing generated entries without game-specific logging:

```sh
SNESRECOMP_TRACE_FILE=saves/dispatch.jsonl \
SNESRECOMP_TRACE_CHANNELS=dispatch ./build/MyGame game.sfc --frames 2400

snesrecomp-go/build/snesbuild dispatch-census --root . \
  --trace saves/dispatch.jsonl --rom game.sfc \
  --out-analysis saves/dispatch-analysis.json

snesrecomp-go/build/snesbuild analyze --root . --rom game.sfc \
  --dispatch-analysis saves/dispatch-analysis.json
```

All three relative file paths in this command are resolved from `--root`.
Still-unresolved indirect jumps record their computed target immediately before
the existing hard trap, so the census can identify a bootstrap target without
requiring a pre-existing `hle_dispatch`; these records are labeled as trapped
and do not claim that the target executed.

The evidence file is deterministic and separate from authored cfg. Candidate
`func` lines in the text report are an escape hatch, not an automatic claim
that the target is a normal routine; entry/continuation semantics still need
classification. Build census runs without
`SNESRECOMP_SEMANTIC_DISPATCH_TRACE`, which is reserved for lowering-neutral
A/B edge hashes.

Audio bring-up has an equivalent no-Python capture/audit path. Set the prefix
before initializing the runner and shut the game down normally after reaching
a scene where sound is active:

```sh
SNESRECOMP_APU_AUDIT_PREFIX=saves/audio-stage1 \
  ./build/MyGame game.sfc --frames 2400

snesrecomp-go/build/snesbuild apu-audit --root . \
  --prefix saves/audio-stage1 --strict
```

The runtime writes `.aram`, `.dsp`, `.written`, and a canonical `.audio.jsonl`
sidecar that omits host PCM scheduling events.
The Go audit uses active DSP voices and retained key-on evidence, walks each
BRR stream with the DSP's wrapping 16-bit ARAM semantics, validates loop
addresses, and verifies every consumed byte against observed SPC/HLE writes.
Directory entries are starts, not sample bounds: shared BRR suffixes remain
valid. The audit reports a different CPU-to-APU port value replacing an unread
value, while counting same-value rewrites separately. Changed overwrites are
shown live on first/base-16 hits and in the final report. A capture with no
audible voice or retained key-on is explicitly inconclusive rather than a successful audit.
Capture close to the failure when a game replaces sample banks dynamically.

To build an isolated runtime candidate from only closed, statically proven
automatic facts, use the explicit experimental overlay. It refuses the normal
`src/gen` path and never edits cfg. The same validation mode propagates exact
live M/X state across direct calls, avoiding speculative callee variants. It
also validates small, metadata-free, single-owner continuation blocks as exact
resumable-region edges while retaining their standalone registry entries:

```sh
snesrecomp-go/build/v2regen regen \
  --rom game.sfc --cfg-dir recomp \
  --out-dir build/proven-analysis-candidate \
  --experimental-proven-analysis
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

Windows ARM64 uses a native AArch64 Zig development snapshot rather than the
stable 0.16.0 binary. The stable binary was itself miscompiled by an upstream
LLVM ARM64 COFF TLS bug and crashes on every compile; the replacement is the
exact fixed snapshot verified in
[`kaappi#1613`](https://github.com/kaappi/kaappi/issues/1613), authenticated
with the Zig Software Foundation minisign key, and pinned by SHA-256. Other
platforms remain on stable Zig 0.16.0. Replace this exception when a stable Zig
release containing the LLVM fix becomes available.

It is driven by a `snesbuild.ini` manifest at the project root; see
[`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md).

Generated C includes the public `snesrecomp/game/cpu.h`,
`snesrecomp/game/trace.h`, `snesrecomp/game/generated_support.h`, and project-owned
`funcs.h`. The game target must therefore use the include directories exported
by `runtime/runner.cmake` plus its own `recomp` directory. A minimal CMake
pattern is:

```cmake
set(SNESRECOMP_GO_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp-go")
include("${SNESRECOMP_GO_ROOT}/runtime/runner.cmake")

file(GLOB GAME_GEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/gen/*.c")

add_library(snesrecomp_runtime STATIC
  ${SNESRECOMP_RUNNER_SOURCES})
snesrecomp_configure_runtime_target(snesrecomp_runtime)
add_library(snesrecomp::runtime ALIAS snesrecomp_runtime)

add_executable(MyGame
  ${GAME_GEN_SOURCES}
  src/main.c
  src/game_runtime.c)

target_include_directories(MyGame PRIVATE
  "${CMAKE_SOURCE_DIR}/recomp"
  "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(MyGame PRIVATE snesrecomp::runtime)
```

For a vended SDK, replace the source-library block with
`find_package(snesrecomp-runtime CONFIG REQUIRED)`. The game still links the
same `snesrecomp::runtime` target and never receives runner-private headers.

The runtime is not a complete frontend. Each project supplies ROM validation
and loading, video/audio presentation, its versioned `RtlGameModule`, frame
and interrupt policy, and any C functions named by
`hle_func`/`hle_func_if`/`hle_dispatch`.
See [`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md) for the full
toolchain contract,
[`runtime/docs/GAME_ENHANCEMENT_INTEGRATION.md`](runtime/docs/GAME_ENHANCEMENT_INTEGRATION.md)
for widescreen and enhanced-audio producer integration, and
[`runtime/docs/API_REFERENCE.md`](runtime/docs/API_REFERENCE.md) for the capability, lifetime,
and function-level SDK reference. See
[`docs/CFG_FORMAT.md`](docs/CFG_FORMAT.md) for bank directives.

### Portable runner

The independently authored MIT runner under `runtime/` is the sole runtime:

```sh
snesbuild build --root .
snesbuild build --hermetic --root .
```

Its standalone module tests every core subsystem without a ROM, generated game
code, or SDL:

```sh
cmake -S snesrecomp-go/runtime -B build/runtime
cmake --build build/runtime
ctest --test-dir build/runtime --output-on-failure
```

The generated-code and public runner ABI is portable C11. The private S-DSP
accuracy unit is C++20 behind a C bridge; OS, SDL, graphics API, and audio API
types remain behind host adapters. Hermetic objects live under
`build/hermetic/`.

## Commands

All commands accept explicit paths and can be run from the game project root:

```sh
v2regen regen --rom game.sfc --cfg-dir recomp --out-dir src/gen --jobs 8
v2regen regen --rom game.sfc --cfg-dir recomp \
  --out-dir build/proven-analysis-candidate --experimental-proven-analysis
v2regen analyze --rom game.sfc --cfg-dir recomp --jobs 8
v2regen poll-census --rom game.sfc --cfg-dir recomp --registers 4210,4212
v2regen disasm 01:9C6F --rom game.sfc --mx 0,0 --until-flow --raw
v2regen rom-info --rom game.sfc
v2regen spc-disasm 0800 08F0 --input game.sfc --upload-block 0x011ACD
v2regen apu-audit --prefix saves/audio-stage1 --strict
v2regen quintet-lzss 0x0CD695 --input game.sfc --out build/blob.bin
v2regen xref 00:9DE1 --rom game.sfc --cfg-dir recomp --kind branch
v2regen xref 0295 --rom game.sfc --cfg-dir recomp --kind write --wram-mirrors
v2regen xref 01:9CD6 --rom game.sfc --cfg-dir recomp \
  --data-words --target-minus-one
v2regen sync-funcs --cfg-dir recomp --out recomp/funcs.h
v2regen metadata --gen-dir src/gen --cfg-dir recomp --out build/gen_meta.json
v2regen rts-webs --rom game.sfc --cfg-dir recomp --suggest --yield-helpers
v2regen trace-inspect runs/example.jsonl --summary
v2regen trace-inspect runs/example.jsonl --diagnose \
  --metadata saves/gen_meta.json --rom game.sfc
v2regen trace-diff final oracle.jsonl recomp.jsonl
v2regen trace-diff sequence oracle.jsonl recomp.jsonl --skip-zp
v2regen trace-diff aligned oracle.jsonl recomp.jsonl \
  --clock-low 0x88 --clock-high 0x89
v2regen mx-diff recomp_mx.txt oracle_mx.txt --offset 0
v2regen wram get --symbols docs/ram-map.md \
  --file saves/dump_wram.bin 21 0295
v2regen chr-render rom game.sfc 0x68000 0x8000 build/chr.png --cols 16
v2regen stub-census --gen-dir src/gen
v2regen link-audit --gen-dir src/gen --src-dir src --tailcalls \
  --runtime-dir snesrecomp-go/runtime/src
v2regen inspect --rom game.sfc --cfg-dir recomp --jobs 8
v2regen emit-function --rom game.sfc --cfg-dir recomp \
  --bank 00 --start 8000 --m 1 --x 1
```

`poll-census --registers` accepts any comma-separated hexadecimal 16-bit
addresses, including hardware status, APU ports, and WRAM synchronization
flags; `$4210` and `$4212` are defaults, not a fixed allow-list. By default,
`--interrupt-sync` also finds low-WRAM addresses written by decoded NMI/IRQ
ownership and read by decoded code, then includes those addresses in the
census with writer and interrupt-root provenance. Use
`--interrupt-sync=false` for the explicit list only. Coverage reports
distinguish conditional HLE fallbacks from whole-body replacement. Only
whole-body coverage removes a poll from the potentially live count.

The packaged `snesbuild` binary exposes the same `disasm` and `xref` commands
with paths relative to `--root`. Decoded xrefs come only from rooted instruction
boundaries. `--data-words` is a separate raw-byte scan whose results are marked
with unknown ownership and reachability; add `--target-minus-one` for pushed
continuation or handler-minus-one tables.

The packaged project driver also provides manifest-defined replay gates without
a Python runtime:

```sh
snesbuild replay-bench --suite tools/runner-bench.json \
  --binary build/game --rom game.sfc --config config.ini \
  --runs 7 --warmups 1 --output build/runner-baseline.json
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

Each target keeps a separate object tree under `build/hermetic/<target>/`, so
cross checks and the native build stay independently incremental. A cross build
never falls back to the host's SDL3; pass
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
the per-user cache. It has no dependency on the playable C runner, a browser
SPC player, FFmpeg, or a system audio converter. The generated WAVs are ROM-derived game
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
or media assets (the asset manifest ships as an empty template). The packaging
CMake install manifest is the authoritative bundle contract.

## Documentation

- [`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md): project layout,
  generation pipeline, CMake, runtime hooks, and redistribution rules.
- [`docs/CFG_FORMAT.md`](docs/CFG_FORMAT.md): supported `bankNN.cfg` syntax.
- [`docs/TOOLING_MIGRATION.md`](docs/TOOLING_MIGRATION.md): Go tooling replacements
  for retired project Python scripts and the boundary for intentionally
  game-specific helpers.
- [`runtime/docs/RUNTIME.md`](runtime/docs/RUNTIME.md): shared runtime boundary, optional
  features, and current limitations.
- [`ATTRIBUTION.md`](ATTRIBUTION.md): Python-source provenance, prior work,
  contributor credit, and licensing status.
- [`LICENSE_SCOPE.md`](LICENSE_SCOPE.md): the original-code grant, explicit
  inclusion of the replacement runner, and game-content exclusions.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md): compatible third-party
  components, including the Snaggletooth S-DSP attribution.

## Licensing

The original Go implementation, tooling, tests, documentation, and
project-authored portable runner sources are [MIT-licensed](LICENSE). The
complete runner is redistributable under MIT-compatible terms: its attributed
Snaggletooth S-DSP portions retain Eric Tomasso's upstream MIT notice. See
[`LICENSE_SCOPE.md`](LICENSE_SCOPE.md) for the exact grant and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for retained third-party
credits.

Game ROMs, generated/recompiled ROM code, extracted previews, and embedded
retail media are not relicensed under MIT.
