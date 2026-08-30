# Project integration

This guide describes the contract between the shared `snesrecomp-go` module
and a per-game project. The toolchain is game-agnostic; ROM knowledge, authored
configuration, frontend code, and game-specific runtime policy stay in the
game repository.

For the graphics-producer, widescreen, separated-layer, and enhanced-audio
workflow, continue with
[`GAME_ENHANCEMENT_INTEGRATION.md`](../runtime/docs/GAME_ENHANCEMENT_INTEGRATION.md).

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
│   ├── game_cpu_infra.c        # RtlGameModule registration
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
`stub-census` covers unresolved gotos, dispatch bounds, unresolved indirect
jumps, inline invalid-target traps, and target bodies in
`unresolved_stubs_v2.c`; it collapses M/X variants to logical sites/targets.

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

`runtime/runner.cmake` exports a source list, two include classifications, and
a target configuration helper:

- `SNESRECOMP_RUNNER_SOURCES`: shared C11 runtime sources plus the private
  C++20 S-DSP accuracy translation units.
- `SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS`: supported namespaced SDK headers.
- `SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS`: runner implementation headers.
- `snesrecomp_configure_runtime_target(target)`: applies the public/private
  boundary, public C11/private C++20 requirements, SIMD selection, and bitset
  configuration.

The manifest defines `SNESRECOMP_ENABLE_TRACE`. The trace option compiles local
trace rings and a link stub; the historical TCP debug server was intentionally
not carried into the independent runner.

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyGameRecomp LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)

set(SNESRECOMP_GO_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp-go")
include("${SNESRECOMP_GO_ROOT}/runtime/runner.cmake")

find_package(SDL2 REQUIRED)
file(GLOB GAME_GEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/gen/*.c")
if(NOT GAME_GEN_SOURCES)
  message(FATAL_ERROR "Run the project regeneration script before building")
endif()

add_library(snesrecomp_runtime STATIC
  ${SNESRECOMP_RUNNER_SOURCES})
snesrecomp_configure_runtime_target(snesrecomp_runtime)
add_library(snesrecomp::runtime ALIAS snesrecomp_runtime)

add_executable(MyGame
  ${GAME_GEN_SOURCES}
  src/main.c
  src/config.c
  src/game_cpu_infra.c
  src/game_rtl.c)

target_include_directories(MyGame PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/recomp"
  ${SDL2_INCLUDE_DIRS})

target_compile_definitions(MyGame PRIVATE
  SNESRECOMP_TRACE=$<BOOL:${SNESRECOMP_ENABLE_TRACE}>
  SNESRECOMP_REVERSE_DEBUG=0)
target_link_libraries(MyGame PRIVATE
  snesrecomp::runtime ${SDL2_LIBRARIES} m)
```

The runner must be its own static-library target. Game and generated
translation units compile against public headers only; they do not compile or
reference runner implementation objects. With an installed or vended SDK, use
the equivalent package target:

```cmake
find_package(snesrecomp-runtime CONFIG REQUIRED)
target_link_libraries(MyGame PRIVATE snesrecomp::runtime)
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
4. **Game registration.** Define one immutable, versioned `RtlGameModule`, call
   `RtlRegisterGame(&game_module)` before `SnesInit`, require
   `SR_RESULT_OK`, and keep the module and every referenced table alive for the
   process lifetime. Registration is rejected while a runner exists.
5. **Frame/interrupt policy.** Supply `run_frame`, optional `draw_ppu_frame`,
   reset entry, NMI/IRQ invocation, any coroutine/yield policy, and the
   recovered ordering of body slices, interrupt handlers, and scanout. A host
   tick is not a universal hardware phase; record the evidence for the chosen
   cyclic boundary in the project.
6. **HLE hooks and required symbols.** Every C symbol named by `hle_func`,
   `hle_func_if`, or `hle_dispatch` in a cfg must be implemented by the game
   project with the generated `CpuState *` ABI. `snesrecomp/game/required_symbols.h`
   collects the non-cfg symbols the runner references but does not define
   (currently `RtlApuLock`/`RtlApuUnlock`); `snesbuild doctor` checks both. An
   `hle_func_if` predicate returns `bool` and must not mutate CPU or emulated
   state. Doctor uses `snesbuild.ini`'s actual source list. Ordinary definitions
   are verified; unusual macro-authored definitions are reported as unverified
   and left to the native linker instead of being rejected heuristically.
7. **Game-specific hardware workarounds.** Put ROM-address policy in the game
   layer. `RtlGameExecutionApi.read_rdnmi` may override a `$4210` read (return
   `-1` for shared behavior), and `recover_dispatch_miss` may opt verified
   dispatch sites into the shared recovery mechanism. Leave both NULL unless
   needed.

A minimal registration unit looks like:

```c
#include "snesrecomp/game/bootstrap.h"
#include "game_rtl.h"

static const RtlGameIdentity kGameIdentity = {
  .struct_size = RTL_GAME_IDENTITY_V1_SIZE,
  .game_id = "my_game",
  .display_name = "My Game",
  .save_name_prefix = "save",
};

static const RtlGameExecutionApi kGameExecution = {
  .struct_size = RTL_GAME_EXECUTION_API_V2_SIZE,
  .run_frame = &RunOneFrameOfGame,
  .draw_ppu_frame = NULL,
  .read_rdnmi = NULL,
  .recover_dispatch_miss = NULL,
};

const RtlGameModule kGameModule = {
  .abi_version = RTL_GAME_MODULE_ABI_VERSION,
  .struct_size = RTL_GAME_MODULE_V2_SIZE,
  .capabilities = RTL_GAME_MODULE_CAP_IDENTITY |
                  RTL_GAME_MODULE_CAP_EXECUTION,
  .identity = &kGameIdentity,
  .execution = &kGameExecution,
};

/* Before SnesInit: */
if (RtlRegisterGame(&kGameModule) != SR_RESULT_OK)
  return false;
```

Add `RtlGameLifecycleApi`, `RtlGameStateProviderApi`, or `RtlGameAudioApi`
only when the project needs those contracts, and set the matching module
capability bit. The lifecycle initialization callback gets callback-lifetime
mutable ROM bytes for verified game patches. The runner installs state
providers and revokes them at shutdown; game code does not call private runner
binding functions. Audio sub-capabilities independently opt into SPC upload,
voice classification, and extended-audio safe points.

## Ownership matrix

The runner implements SNES hardware. It does not implement an application, and
several of the gaps are silent rather than loud. This table is the contract;
the "symptom if missing" column is the one worth reading first.

| Area | Runner provides | Game must provide | Symptom if missing |
| --- | --- | --- | --- |
| Frame loop | `RtlRunFrame`, watchdog, host-tick counter | `RtlGameExecutionApi.run_frame` and a recovered body/NMI/scanout schedule | game can boot and render but advance at the wrong stable rate or present stale HDMA/PPU state |
| Reset / main loop | generated `ResetHandler` | call it, and **service tail calls** (below) | reset unwinds silently; NMI keeps firing against a game that never started |
| NMI / IRQ | `RtlGameFrameComplete` reports `NMI_ENTERED` | push an interrupt frame, call `NmiHandler`, restore | `RTI` over-pops and corrupts `P`, so M/X widths of interrupted code go wrong |
| Rendering (when video is requested) | per-line render, `$420C`-owned HDMA, margins via `run_ppu_scanout` | `draw_ppu_frame` that drives it once per frame | **nothing is ever rasterized**; frames advance over a black canvas |
| Canvas (when video is requested) | writes into a host buffer | `bind_ppu_output_surface`, `SR_PPU_OUTPUT_MAIN`, `scale` **must be 0** | PPU has nowhere to draw; black canvas |
| Canvas format | colour in the low 24 bits, **top byte left zero** | present as `XRGB8888`, not `ARGB8888` | every pixel is alpha 0 and blends away to black |
| Input | `SwapInputBits` on register reads | 12 bits per controller in `RtlRunFrame` (player 2 at bits 12-23) | every button dead or mapped to the wrong function |
| APU lock | calls `RtlApuLock`/`RtlApuUnlock` | define both (see `game/required_symbols.h`) | link error naming an unfamiliar symbol |
| Audio | SPC, DSP, mixing | `RtlGameAudioApi` only for upload/routing/extension | silence, with no diagnostic |
| Hardware polling | deterministic progress for modeled status/counter registers and APU catch-up | HLE only a documented, title-specific timing dependency the shared model cannot express | a genuinely unmodeled spin can still consume no emulated time and hang |

Hardware register state has exactly one owner. A `$420C` write arms the runner's
DMA channels and `run_ppu_scanout` consumes that state directly. The
zero-initialized `SrPpuScanoutRequest` preserves normal hardware behavior;
`hdma_suppress_mask` may disable selected channels for explicit enhancement
policy, but can never enable one or mutate the saved `$420C` state. Do not keep
a game-side HDMAEN shadow or echo a register value into the scanout request.
Use `query_dma_state` when diagnostics need to observe the current mask.

The recompiler does not provide a cycle-exact CPU clock, but generic polling is
still runner responsibility. `$4212` and SLHV/OPHCT/OPVCT share one saveloaded,
deterministic beam model. Status reads advance it in bounded master-cycle steps;
an SLHV latch advances one synthetic scanline so a loop waiting for the beam to
leave a range makes progress. This is a liveness and ordering contract, not a
promise of dot-accurate timing. If a title depends on tighter timing, put that
ROM-address policy in a narrowly scoped HLE and document the unsupported timing
assumption rather than replacing the generic register globally.

## Recovering the game frame schedule

The console timeline is cyclic, so adjacent phases can be grouped into a host
tick in more than one valid way. The runner therefore defines the effects of
its timing and scanout primitives but does not impose a body/NMI/scanout order.
`RtlGameFrameBegin` positions the beam at VBlank, publishes the RDNMI token, and
enables forced pacing. `RtlGameFrameComplete` disables forced pacing and reports
whether the live NMI gate transitioned. Neither function executes the game
body or NMI handler. `run_ppu_scanout` synchronously consumes the live PPU,
DMA, HDMA, and IRQ state present at its call site.

Recover the adapter schedule instead of selecting one by convention:

1. Identify the ROM's VBlank wait, `WAI`/`$4210` polling, or WRAM frame gate.
   Use decoded `snesbuild xref` queries on the candidate gate to find every
   producer and consumer; do not rely on a raw byte search.
2. Trace an explicit steady-state frame range. Record HOST_TICK, GAME_SLICE,
   SCANOUT, NMI transition, actual NMI/IRQ handler entry and exit, and the
   raster line of each IRQ callback.
3. Determine which NMI produces the token or state consumed by each body
   slice, which body generation prepares HDMA/PPU state for scanout, and which
   body-written gates raster IRQs observe.
4. Implement that schedule in the game adapter. Emit unqualified interrupt
   events around actual handler calls; runner-qualified TRANSITION and CALLBACK
   events deliberately distinguish latch/callback observation from execution.
5. Validate cumulative body-gate releases versus NMIs, chained raster IRQs,
   deterministic CPU/WRAM/SRAM state, and presentation hashes over the same
   steady-state range. Include loading and forced-blank paths, which may use a
   different number of body resumptions without changing the adapter contract.

A wait-token coroutine may resume a body slice and then service the NMI
reported by COMPLETE. Another recovered loop may service that NMI before
running the body that prepares scanout. These examples demonstrate why the
choice is game-owned; they are not a complete enumeration and should not become
a runner schedule enum. Do not add shared warnings based on zero blocks between
NMI and scanout or on execution while `forceNmi` is set: both confuse one
adapter pattern with a hardware invariant.

Intentional headless runs may omit `draw_ppu_frame` and never call
`RtlGameDrawPpuFrame`. Once a host requests video, however, a missing callback
is reported immediately. If the callback exists but no scanout or main surface
has been observed after 120 emulated frames, those omissions are reported once.
The diagnostic state belongs to the runner instance; it is neither public ABI
nor savestate data.

Input packing deserves spelling out, because the obvious guess is wrong. The
`kJoypadL_*` / `kJoypadH_*` constants in `game/runtime.h` describe the
**hardware** `$4218`/`$4219` bytes you read back. They are not the format
`RtlRunFrame` accepts. The runner stores its argument verbatim and reverses all
16 bits on each register read, so argument bit *N* becomes joypad bit *15-N*:

```text
bit  0  B      bit  4  Up      bit  8  A
bit  1  Y      bit  5  Down    bit  9  X
bit  2  Select bit  6  Left    bit 10  L
bit  3  Start  bit  7  Right   bit 11  R
```

`RtlRunFrame`'s `bool` return is vestigial: it is unconditionally `false` and
carries no success information. Ignore it.

## Servicing tail calls

A recompiled function that ends in a jump to another entry (`JMP` to a declared
`func`, or a decode that runs past an `end:` boundary) has two emitted forms.
When the caller supplied a host return context it dispatches inline. When it did
not -- which is exactly the top-level call a frontend makes -- it records a
pending tail call and returns `RECOMP_RETURN_TAILCALL` for the host to service.

A frontend that calls `ResetHandler` and treats any return as "the game
finished" will therefore appear to boot, run frames, and dispatch NMI forever
against a game that never entered its main loop. The screen stays on whatever
the boot left behind. Service the request instead:

```c
RecompReturn result = ResetHandler_M1X1(&g_cpu);
while (result == RECOMP_RETURN_TAILCALL) {
    const uint32 target = g_tailcall_pc24;
    result = cpu_dispatch_pc_from(&g_cpu, target, g_tailcall_miss_s,
                                  g_tailcall_src24);
}
/* The main loop never returns. Reaching here is an invariant violation --
 * report it rather than parking silently. */
```

This only applies to entries the host calls directly. Generated call sites carry
a return context and resolve tail calls themselves.

## M/X variants and `force_variant_at`

Every function is emitted in up to four variants for the accumulator/index
widths at entry, and call sites dispatch on the live `m`/`x` flags. When width
propagation is wrong, the failure is not a clean crash: the same bytes decode
into a *different but coherent-looking* instruction stream, which can run real
instructions with the wrong operand sizes and fall through into an adjacent
routine entirely.

The signature to watch for is a routine that appears to hang or corrupt memory
inside code it should never have reached. Disassemble the entry at both widths:
usually exactly one decodes as sane code, and the other contains a read-modify-
write to an implausible address. Pin the verified call site with
`force_variant_at BBPPPP M X`.

Pinning fixes dispatch, not semantics. A routine that depends on hardware timing
beyond the runner's documented deterministic model may still need an
`hle_func`; a modeled generic register should not be replaced merely because an
older constant implementation made its polling loop hang.

## Frame presentation policy

Use `SrPpuFramePolicy` once per rendered frame instead of calling private PPU
setters. A normal application is a BEGIN transaction: it validates and replaces
margin geometry, layer fill/motion, row bands, vertical clipping, capture
padding, and HUD split state while clearing the previous frame's retained
virtual providers and layer extents. Publish any provider/extent resources only
after BEGIN.

`CENTERED` reserves the horizontal allocation while retaining a centred native
256-pixel raster; `AVAILABLE` makes requested side pixels rasterizable. Vertical
top/bottom fields are already exact live rows and have no reservation mode.
Policy application fails when bound main/authentic surface capacity cannot hold
the full reserved width or `224 + top + bottom` rendered rows.

If exact margins or fallback masks depend on whether those resources were
accepted, apply the same policy again with `SR_PPU_FRAME_POLICY_FINALIZE`.
FINALIZE requires the active margin budget to match and preserves providers and
extents. Both phases are synchronous, allocation-free, and retain no caller
pointer. The linked game uses `RtlGameApplyPpuFramePolicy`; external enhancement
layers use the capability-gated `SnesRunnerApi.apply_ppu_frame_policy` request
with the current lifetime generation.

The generated ABI is `RecompReturn Function_MxXx(CpuState *cpu)`. Do not invent
per-function return structs or pass CPU registers as C parameters; mutate the
shared `CpuState` exactly as generated code does.

Current generated output includes `snesrecomp/game/cpu.h`,
`snesrecomp/game/trace.h`, and `snesrecomp/game/generated_support.h`.
`snesrecomp/game/bootstrap.h` is the frontend lifecycle and game-registration
surface. Legacy short-header forwarders are not shipped: generated and
authored project code must use the namespaced public headers and must not
include runner implementation or `snes/*` headers. The full public operation
matrix is documented in
[`API_REFERENCE.md`](../runtime/docs/API_REFERENCE.md).

## Hermetic builds (`snesbuild build --hermetic`)

The hermetic path compiles game translation units with a pinned Zig toolchain
and links the executable itself — no CMake and no system compiler. It first
looks for `runtime/lib/<zig-target>/libsnesrecomp_runtime.a` (or the Windows
`.lib`) and uses the matching public headers under `runtime/include`. When no
archive is present in a source checkout, it builds the same library from
`runner.cmake`. It then reads these project inputs:

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
then `PATH`. Objects and the executable land in `build/hermetic/`. Rebuilds are
incremental by source/header mtime plus a compile-flags hash.

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
