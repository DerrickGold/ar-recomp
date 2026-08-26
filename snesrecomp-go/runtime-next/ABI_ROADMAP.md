# Runner ABI roadmap

`runtime-next` currently preserves the generated-code ABI and the compatibility
global surface used during rollout. Those globals and concrete
`Ppu`, `Snes`, CPU, APU, and DSP layouts are compatibility details, not the
long-term extension API.

The next ABI phase is a versioned portable C API centered on a
`SnesRunnerApi` table. The table starts with an ABI version and structure
size, advertises capability bits, and operates on opaque runner, CPU, PPU, APU,
DSP, cartridge, and host handles. Public values use fixed-width integers and
descriptors; they never expose compiler layout, SDL objects, native file
handles, or platform graphics/audio types.

## Read access

- Stable queries cover CPU registers and execution location; WRAM, SRAM,
  VRAM, CGRAM, OAM, PPU registers; APU RAM, ports, timers, and DSP voices;
  cartridge mapping; controller state; frame/audio clocks; and runner status.
- Synchronous callers can borrow immutable spans valid until the next runner
  tick, reset, load, or mutation. Generation counters and dirty ranges let
  tools update only changed regions.
- A frame snapshot descriptor groups component generations, timing, render
  surfaces, audio spans, and optional trace cursors without copying whole
  component memories. Asynchronous consumers explicitly pin or clone only the
  resources they retain.
- Callers provide output buffers for serialization, audio, traces, and bulk
  queries. Scatter/gather descriptors allow discontiguous data without an
  intermediate contiguous frame copy.

## Observation and controlled mutation

- Capability-gated observers report frame boundaries, register and memory
  writes, DMA, audio production, interrupts, generated-function/block events,
  dispatch misses, and errors. Subscriptions select address ranges and event
  classes before execution so disabled observation is cheap.
- Mutations are separate from read views. They are validated, queued for a
  documented safe point, and return an explicit result. Debugger writes,
  save-state loads, input injection, and enhancement commands cannot race a
  running component.
- The runner owns its emulation thread. Borrowed data is thread-confined;
  pinned snapshots and host callbacks state their ownership and lifetime.

## Layering

The ABI describes SNES mechanisms only. ActRaiser addresses, scene meanings,
music identifiers, enhancement policy, and manifest rules remain in a game
adapter. This lets layered enhancements inspect the same generic component
state and events without teaching the runner about a particular title.

## ABI v1 foundation checkpoint (2026-08-26)

The first migration item is complete. `SnesRunnerApi` now begins with an ABI
version, byte size, and additive capability bits. Its operations use opaque
runner/component handles and fixed-width public values; the original descriptor
fields remain prefix-compatible and now also report size and capabilities.

ABI v1 currently provides:

- generic CPU, PPU, APU, DSP, SPC, DMA, cartridge, and runner component handles;
- thread-confined borrowed byte spans for WRAM, SRAM, and immutable ROM;
- lifetime, tick, reset, successful-load, and controlled-mutation generations;
- invalidation before each emulation tick and after reset/load safe points; and
- explicit `unsupported` results for APU RAM and DSP-register borrowing, because
  the audio thread can advance those components between main-thread ticks. They
  require a future lock-aware pinned snapshot rather than an unsafe pointer.

The ABI contract suite pins field order, v1 structure extents, capability
advertisement, unsupported-version behavior, output-size validation, component
resolution, borrow/reborrow behavior, and expiration through real reset and
load paths. The private `Snes` generation fields live before the legacy raw
savestate tail: its `hPos`-anchored byte count remains exactly 48 bytes.

The standalone portable suite passes 27/27. The full application suite passes
all 89 runnable tests; two optional GPU tests skip without a display, and the
shader GPU smoke passes when given host-display access. The public header also
passes C++17 syntax validation and the implementation passes an x86-64 macOS
cross-target syntax build from the ARM64 host.

Seven adjacent baseline/candidate pairs alternated first-run order and kept all
final replay artifacts identical. Portable median deltas were +0.87% for
Mode 7/world map, -0.05% for SIM, +0.09% for Aitos wide, and +0.18% for Death
Heim wide; suite regression was +0.27%. Native-SIMD deltas were -0.56%, -0.34%,
+0.66%, and effectively 0%, with the suite 0.06% faster. Both are inside the
documented acceptance gate.

## ABI v1 diagnostics checkpoint (2026-08-26)

The first read-only consumer slice is complete. ABI v1 now has an additive,
capability-gated CPU-state query with fixed-width registers, execution PC,
flags, frame counter, and lifetime generation. The portable runner does not
depend on generated game state: a lifecycle-bound game adapter supplies the
authoritative recompiled-CPU view. Registrations are scoped to one runner and
are revoked on replacement, failed initialization, and shutdown.

`runtime_diagnostics.c` now obtains its CPU state and WRAM/SRAM views through
ABI v1. ActRaiser addresses and meanings remain in the game-side diagnostic,
while the runner exposes only generic SNES state. Dispatch history and the
recompiled call stack still use compatibility observers and will move with the
filtered-observer phase.

The standalone suite remains 27/27, including provider availability, output
extent, register/flag contents, and lifecycle bind/revoke coverage. The full
application passed all 89 headless/CPU tests and both host-display GPU smoke
tests. Seven final adjacent baseline/candidate replay pairs kept every WRAM,
SRAM, state-report, and dispatch-log artifact identical. Portable paired
median deltas were -0.16% for Mode 7/world map, effectively 0% for SIM, -0.59%
for Aitos wide, and -0.58% for Death Heim wide; the suite was 0.33% faster.
Native-SIMD deltas were -0.24%, +0.03%, +0.31%, and +0.11%; suite regression
was +0.05%. Both configurations remain inside the performance gate.

## ABI v1 PPU inspection checkpoint (2026-08-26)

ABI v1 now exposes a fixed-width PPU register/background snapshot and
zero-copy, host-native `uint16_t` views for VRAM, CGRAM, and OAM. High OAM is
available through the existing byte-span contract. The PPU descriptor includes
screen masks, display/object/mode controls, derived OBJ and BG tile addresses,
scroll values, tilemap dimensions, bit depth, tile size, and current horizontal
and vertical margins. This is sufficient for generic resident-asset tools
without exposing `Ppu` layout or requiring redundant full-memory copies.

The resident scene-asset exporter now consumes only these ABI descriptions and
borrowed WRAM. It rejects wrong-region, undersized, or mixed-generation inputs
before writing a package, so an asynchronous or stale combination cannot be
mistaken for a coherent snapshot. Its PNG/raw/metadata regression fixture and
all 27 runner contract tests pass; the full 89-test application tier and both
host-display GPU smoke tests remain green.

Seven final adjacent baseline/candidate pairs kept every replay artifact
identical. Portable paired median deltas were +0.83% for Mode 7/world map,
+0.47% for SIM, -0.18% for Aitos wide, and -0.57% for Death Heim wide; suite
regression was +0.14%. Native-SIMD deltas were +0.06%, -0.12%, +0.61%, and
+0.70%; suite regression was +0.31%. An additional 11-pair native Death Heim
probe measured +0.74%, consistent with a limited sub-1% code-layout effect from
the larger cold ABI surface rather than per-frame ABI work. Both configurations
remain well inside the acceptance gate.

## ABI v1 PPU frame-state checkpoint (2026-08-26)

ABI v1 now provides one coherent, fixed-width frame-state query for the five
generic SNES render sources (BG1-BG4 and OBJ). It describes overlay bounds,
content bands, composition flags, transparent fill, OBJ ranges, display/mode
state, HUD split geometry, widescreen margin budget, and Mode 7 override state.
The query returns only compact metadata; pixel surfaces are not copied, and OAM
continues to use the generation-checked borrowed-memory contract.

The presentation-time frame-slot capture now consumes this view instead of
reading the concrete `Ppu` overlay and HUD fields. A five-pair hidden-renderer
probe exercised the actual per-frame path over 600 wide-action frames, with
alternating baseline/candidate order. Native-SIMD median elapsed time changed
from 5.1945 seconds to 5.2000 seconds (+0.11%). Its 2160x1344 framebuffer and
final WRAM, SRAM, CPU-state, and dispatch-log artifacts were byte-identical.

The four-workload pure-headless gate also remains exact. Portable paired median
deltas were +0.84% for Mode 7/world map, +0.18% for SIM, -0.34% for Aitos wide,
and -0.45% for Death Heim wide; suite regression was +0.06%. Native-SIMD deltas
were -0.22%, +0.04%, +0.04%, and +0.02%; the suite was 0.03% faster. The
headless gate primarily covers state parity and code-layout effects because it
does not submit presentation frames; the hidden-renderer probe is the explicit
hot-path performance and rendered-output check for this migration.

## ABI v1 derived OBJ raster checkpoint (2026-08-26)

ABI v1 now exposes a synchronous, generation-checked OBJ-range raster service.
The caller supplies the output buffer, byte capacity, and pitch; the runner
returns fixed-width bounds and transparent `0xAARRGGBB` pixels. It resolves the
selected OAM range once and rasterizes directly into caller-owned storage, so
consumers neither receive a concrete `Ppu` nor pay for an intermediate surface.
Unsupported formats, stale generations, invalid sprite ranges, misaligned
pitch, and insufficient capacity fail explicitly.

The generic service is bound separately from the dependency-free core ABI
table. This preserves low-level SNES tests that intentionally substitute fake
components while allowing the complete runner to provide the real PPU service.
The world-navigation adapter now classifies a typed borrowed OAM view, reads
master brightness through the PPU snapshot, reads backdrop color through the
typed CGRAM view, and renders its Palace/UI ranges through this service. Its
public capture boundary no longer accepts or includes a concrete `Ppu`.

The focused PPU regression exercises both non-empty world-navigation layers,
pixel colors, bounds, partial brightness, empty animation, and forced-blank
fallback. The standard replay gate intentionally disables enhanced SIM3D, so
it measures cumulative runner overhead and state parity rather than active OBJ
raster cost. Seven adjacent portable pairs changed by +0.81% for Mode 7/world
map, +0.09% for SIM, +0.07% for Aitos wide, and +0.43% for Death Heim wide;
suite regression was +0.35%. Native-SIMD changed by -0.93%, -0.43%, +0.65%,
and -0.14%; the suite was 0.21% faster. All final replay artifacts remained
identical and both configurations remain inside the acceptance gate.

## ABI v1 resident tile-census checkpoint (2026-08-26)

The live HD tile census and Mode-7 canvas dumper now consume only the generic
PPU snapshot plus generation-matched borrowed VRAM, CGRAM, OAM, and high-OAM
views. Public constants describe the native scan width, OBJ coordinate wraps,
negative-Y band, and tile-id extent, so the external tool no longer includes
or dereferences the concrete `Ppu` layout. When both developer features are
disabled, no ABI query or memory borrow occurs.

The migration also removed an invalid retained PPU pointer from the process-exit
contact-sheet writer. A 1,200-frame enabled A/B kept the census JSONL, BG/OBJ
contact sheets, WRAM, SRAM, CPU-state report, and dispatch log byte-identical.
The old path then dereferenced the already-destroyed PPU while starting its
Mode-7 sheet, wrote a zero-byte file, and exited with SIGSEGV. The ABI path uses
the last coherent CGRAM snapshot, writes the complete Mode-7 sheet and summary,
and exits normally. A separate 400-frame `AR_M7_DUMP` comparison produced eight
canvases with identical filenames and SHA-256 hashes.

Seven adjacent portable pairs changed by +0.40% for Mode 7/world map, +0.04%
for SIM, +0.01% for Aitos wide, and +0.04% for Death Heim wide; suite regression
was +0.12%. Native-SIMD changed by -0.33%, +0.16%, +0.16%, and +0.10%; suite
regression was +0.03%. All final replay artifacts remained identical and both
configurations remain inside the acceptance gate. The standalone runner suite
passes 27/27 and the application passes all 91 CPU and GPU tests.

## ABI v1 retained PPU-surface checkpoint (2026-08-26)

ABI v1 now exposes one coherent snapshot of the host output surfaces bound to
the PPU: the main and authentic framebuffers, five generic BG/OBJ overlay
families with their priority bands, and the scaled Mode-7 overlay. Each
read-only descriptor carries its pixel format, byte extent, pitch, dimensions,
screen origin, and scale. The pixels remain host-owned and are never copied.
A runner-lifetime generation covers ticks, reset, and load; a separate PPU
binding generation invalidates a snapshot immediately after any successful
surface rebind without making ordinary pixel writes increment an epoch.

`FrameSlot` captures the surface snapshot directly and presentation no longer
reads the PPU-bound `g_pixels`, authentic, HUD, action-mask, Diorama, SIM-plane,
or Mode-7 globals. Host-derived products such as the SIM object atlas and flat
composite remain host APIs because they are not PPU surfaces. Diorama upload
and frame generation now consume each descriptor's actual pitch rather than
reconstructing a game-specific stride. Retained presents reuse only the
borrowed descriptors between ticks; no framebuffer copy was added.

Four host-GPU baseline/candidate replays exercised the settled HD Mode-7 title
at game frame 200, the D3a projected-SIM checkpoint, widescreen Aitos action at
game frame 1779, and its 32-line vertical-Diorama variant at the same frame.
Both projected and authentic SIM captures and all three final composited PPM
captures were byte-identical. The standalone suite passes 27/27, the 88-test
headless/CPU tier passes, and all three display/GPU tests pass.

Seven adjacent portable replay pairs changed by -0.82% for Mode 7/world map,
+0.32% for SIM, +1.09% for Aitos wide, and +0.34% for Death Heim wide; suite
regression was +0.23%. Native-SIMD changed by +0.36%, +0.03%, -0.31%, and
+0.50%; suite regression was +0.14%. All final WRAM, SRAM, CPU-state, and
dispatch artifacts remained identical, and both configurations remain inside
the performance gate.

## Migration order

1. [x] Add ABI layout, capability, lifetime, and generation-counter tests while
   retaining the existing globals as an internal compatibility adapter.
2. [ ] Move read-only inspection, diagnostics, and snapshot consumers to ABI v1.
   - [x] Move diagnostic CPU registers and WRAM/SRAM dumps.
   - [x] Add typed PPU register/memory views and migrate the resident-asset
     snapshot exporter.
   - [x] Add compact overlay/frame-state views and migrate frame-slot capture.
   - [x] Add a caller-owned derived OBJ raster service and migrate
     world-navigation capture.
   - [x] Migrate the live HD tile census and Mode-7 canvas dump to coherent
     typed PPU views.
   - [x] Add retained render-surface views and migrate remaining per-frame
     consumers.
3. [ ] Move trace/event consumers to filtered observers and measure disabled-hook
   overhead.
4. [ ] Add safe-point mutation commands and versioned save-state serialization.
5. [ ] Remove external concrete-structure access only after all game adapters use
   the versioned boundary.

Cross-platform CI should compile the ABI from C, C++, Go/cgo, and Rust bindgen
fixtures; poison expired borrowed views in tests; verify serialized versions;
and count bytes cloned per frame so a low-copy regression fails visibly.
