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

## ABI v1 execution-observer checkpoint (2026-08-26)

ABI v1 now exposes one coherent generated-execution snapshot containing the
current function and block, the bounded recompiled call stack, and the last 256
basic blocks with M/X flags, X, and S. A lifecycle-bound game adapter supplies
that generated-code view; the portable runner still contains no ActRaiser
addresses, symbols, or meanings.

The first synchronous observer classes cover basic-block execution and dynamic
dispatch. Subscriptions are fixed-capacity, allocation-free, capability-gated,
and optionally filtered by a 24-bit PC range before callback dispatch. Hot
instrumentation checks a union class mask before constructing an event. The
application crash recorder uses the snapshot for bounded block history and
subscribes only to the much rarer dispatch stream; per-block callbacks remain
available to opt-in tools without imposing their callback cost on normal play.
The retired private dispatch ring and JSON writer were removed after their sole
production consumer moved behind the ABI.

The contract suite covers structure extents, provider availability, stack and
history contents, unsupported classes and filters, invalid ranges, multiple
subscriptions, PC filtering, serial order, unsubscribe behavior, and lifecycle
cleanup. The standalone runner remains 27/27, the complete application suite
passes 91/91 including all GPU tests, the public header passes C++17 syntax
validation, and the implementation passes an x86-64 macOS cross-target syntax
build.

Seven adjacent baseline/candidate pairs kept every WRAM, SRAM, CPU-state, and
dispatch artifact identical. Portable paired median deltas were +0.13% for
Mode 7/world map, -0.26% for SIM, -1.08% for Aitos wide, and -0.70% for Death
Heim wide; the suite was 0.48% faster. Native-SIMD deltas were -0.24%, +0.16%,
+0.06%, and +0.16%; suite regression was +0.04%. Both configurations remain
well inside the performance gate.

## ABI v1 memory/register-observer checkpoint (2026-08-26)

ABI v1 now reports canonical memory writes for WRAM, SRAM, VRAM, CGRAM, OAM,
and high OAM, plus CPU-visible register reads and writes across the recompiled
fast path and the portable SNES bus/device path. Memory addresses are byte
offsets within an explicit region; register addresses retain their full CPU
bus value. Address and memory-region filters are validated when a subscription
is installed, before any hot-path payload is built.

The unified JSONL trace now consumes these observer classes for WRAM/stack,
PPU-memory, and register channels. Its old per-WRAM-write environment poll was
removed. Disabled observation uses an unlikely class-mask branch, and previous
values are loaded only when a memory observer or compatibility hook actually
needs them. The PPU-to-runner association remains in the cold ABI adapter so
the large VRAM/CGRAM/OAM arrays retain their prior layout and alignment.

The contract tests exercise filtered real WRAM, VRAM, and register accesses,
invalid address/region filter combinations, subscription union masks, and
trace-consumer bind/unbind cleanup. The standalone runner passes 27/27 and the
full application passes 91/91, including all GPU tests. The public header also
passes C++17 syntax validation and the touched implementation passes an
x86-64 macOS cross-target syntax build from the ARM64 host.

Seven alternating adjacent pairs kept every final WRAM, SRAM, CPU-state, and
dispatch artifact identical. Portable median deltas were +1.19% for Mode
7/world map, +0.23% for SIM, +0.04% for Aitos wide, and -0.12% for Death Heim
wide; suite regression was +0.33%. Native-SIMD deltas were -0.71%, +0.16%,
-0.36%, and -0.20%; the suite was 0.28% faster. Both configurations remain
well inside the performance gate.

## ABI v1 DMA-observer checkpoint (2026-08-26)

ABI v1 now reports one `DMA_BEGIN` event for every selected general-DMA or
HDMA channel at activation. The payload exposes the channel, transfer mode,
initial 24-bit A-bus address, B-bus register, indirect bank, direction and
address-control flags, and normalized general-DMA byte count. HDMA reports its
table start and a zero byte count because the table determines the eventual
transfer length. Address filtering uses the initial A-bus/table address.

The event is deliberately not emitted per transferred byte or HDMA scanline.
Normal play pays only the same unlikely union-mask branch used by the other
observer classes. The unified JSONL trace now subscribes to the public DMA
class and distinguishes DMA from HDMA; the narrow historical start hook remains
available as an internal compatibility seam.

The contract suite covers filtered general DMA, the SNES zero-size-to-65536
rule, HDMA metadata, subscription masks, and trace bind/unbind behavior. The
standalone runner passes 27/27 and both portable and native-SIMD application
configurations pass 91/91, including all GPU tests. The public header passes
C++17 syntax validation, and the touched C sources pass an x86-64 macOS
cross-target syntax build from the ARM64 host.

Seven alternating adjacent pairs kept every final WRAM, SRAM, CPU-state, and
dispatch artifact identical. Portable median deltas were +0.77% for Mode
7/world map, +0.20% for SIM, -0.03% for Aitos wide, and -0.28% for Death Heim
wide; suite regression was +0.17%. Native-SIMD deltas were +0.81%, +0.28%,
-0.29%, and -0.01%; suite regression was +0.20%. Both configurations remain
well inside the performance gate.

## ABI v1 frame/interrupt/error-observer checkpoint (2026-08-26)

ABI v1 now reports host-frame begin/end boundaries, typed NMI/IRQ/BRK/COP
entry and exit, and stable runner error codes. Interrupt payloads include the
interrupted PC, hardware vector, and IRQ scanline when one applies. Error
payloads distinguish unreachable execution, unmapped ROM access, final dynamic
dispatch misses, and dispatch recursion limits; PC filters apply to interrupt
and error streams.

Frame boundaries originate in the common runtime. A game adapter reports its
hardware and software interrupt calls through one generic runner seam, without
placing title addresses or meanings in the ABI. Dispatch, mapping, and
unreachable errors originate at their portable runtime owners. The unified
JSONL trace consumes these public events for its historical vblank, NMI, and
dispatch-miss records; direct game/dispatcher calls into the trace were
removed. A 120-frame application smoke trace observed all 120 vblank boundaries
and 11 actual NMI entries through the new route.

The contract suite covers structured frame, interrupt, and error payloads;
supported and unsupported PC-filter combinations; filtered errors; subscription
union masks; CPU-dispatch error emission; and trace cleanup. The standalone
runner passes 27/27 and both portable and native-SIMD application configurations
pass 91/91, including all GPU tests. The public header passes C++17 syntax
validation, and all touched C sources pass an x86-64 macOS cross-target syntax
build from the ARM64 host.

Seven alternating adjacent pairs kept every final WRAM, SRAM, CPU-state, and
dispatch artifact identical. Portable median deltas were -0.14% for Mode
7/world map, -0.27% for SIM, +0.37% for Aitos wide, and +0.78% for Death Heim
wide; suite regression was +0.18%. Native-SIMD deltas were -0.91%, -0.38%,
+0.19%, and -0.13%; the suite was 0.31% faster. Both configurations remain
well inside the performance gate.

## ABI v1 audio-observer checkpoint (2026-08-26)

ABI v1 now reports one `AUDIO_PRODUCED` event for each completed host PCM
block, after DSP resampling, MSU-1 mixing, and replacement-music mixing. The
payload describes interleaved native-endian signed 16-bit samples, output rate,
channel count, frame count, and a monotonic output-frame offset since reset.
The sample pointer is zero-copy and valid only for the synchronous callback.

Audio events originate on the host audio thread. Observer dispatch is now
serialized across runner and audio producers so a subscription never receives
concurrent callbacks. Subscription installation/removal remains a lifecycle
operation that must not race active producers. The event is emitted after the
APU lock is released, keeping external work out of the emulation/audio critical
section. Normal play adds only one 64-bit audio-clock increment and the existing
unlikely observer-mask branch per host block; the per-DSP-sample loop is
unchanged.

The raw DSP-sample trace remains an internal diagnostic because routing every
native sample through the general observer registry would add unacceptable hot
loop overhead and would not describe the final host mix. Likewise, the
replacement-music mixer remains a mutation hook and belongs to the later
controlled-mutation API rather than a read-only observer.

The contract suite drives the real `RtlRenderAudio` path and verifies final-mix
ordering, transient buffer contents, format metadata, and consecutive audio
clock offsets. The standalone runner passes 27/27 and both portable and
native-SIMD application configurations pass 91/91, including all GPU tests.
The public header passes C++17 syntax validation, and all touched C sources
pass an x86-64 macOS cross-target syntax build from the ARM64 host.

Seven alternating adjacent pairs kept every final WRAM, SRAM, CPU-state, and
dispatch artifact identical, including across portable and native builds.
Portable median deltas were -0.29% for Mode 7/world map, +0.05% for SIM,
-0.47% for Aitos wide, and -0.67% for Death Heim wide; the suite was 0.35%
faster. Native-SIMD deltas were +0.07%, +0.12%, +0.14%, and +0.20%; suite
regression was +0.13%. Both configurations remain well inside the performance
gate.

## ABI v1 safe-point-mutation checkpoint (2026-08-26)

ABI v1 now accepts fixed-size, caller-independent mutation commands from host
threads and applies them in command-ID order at the beginning of the next host
frame. The initial command set covers up to 16 copied bytes in WRAM, SRAM,
VRAM, CGRAM, OAM, or high OAM, plus masked one-frame input overrides for both
controllers. ROM, APU RAM, and DSP registers fail closed because they require
different ownership or mutation contracts.

The 32-entry queue retains no caller pointers. Each accepted command has a
queryable queued/applying/applied/failed state, explicit result, safe-point
frame number, and caller-consumed terminal record. A short C11 atomic lock
protects cross-thread queue/status access. The emulation thread performs only
one acquire load when the queue is empty; it takes no lock in normal frames.
Commands accepted after a safe point begins are deferred to the following
frame by an ID cutoff.

Borrowed views expire before the first write becomes observable. Memory
mutations reuse the generic memory-event stream, including canonical
little-endian byte addresses for host-native PPU word arrays. Input overrides
are applied before the existing opposing-direction normalization. Pending and
terminal records are removed when a runner is destroyed.

The contract suite validates rejected structure extents, command types,
regions and ranges; queued/terminal status and consumption; deferred WRAM and
cross-word VRAM writes; memory-event routing; input application through the
real `RtlRunFrame` safe point; command ordering; frame metadata; and generation
invalidation. The standalone runner passes 27/27 and both portable and
native-SIMD application configurations pass 91/91, including all GPU tests.
The public header passes C++17 syntax validation, and all touched C sources
pass an x86-64 macOS cross-target syntax build from the ARM64 host.

Seven alternating adjacent pairs kept every final WRAM, SRAM, CPU-state, and
dispatch artifact identical, including across portable and native builds.
Portable median deltas were -0.37% for Mode 7/world map, -0.17% for SIM,
-0.08% for Aitos wide, and -0.04% for Death Heim wide; the suite was 0.16%
faster. Native-SIMD deltas were +0.28%, +0.07%, -0.17%, and +0.17%; suite
regression was +0.09%. Both configurations remain well inside the performance
gate.

## Migration order

1. [x] Add ABI layout, capability, lifetime, and generation-counter tests while
   retaining the existing globals as an internal compatibility adapter.
2. [x] Move read-only inspection, diagnostics, and snapshot consumers to ABI v1.
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
3. [x] Move trace/event consumers to filtered observers and measure disabled-hook
   overhead.
   - [x] Add coherent execution/flight-recorder state plus filtered block and
     dispatch observers; migrate runtime crash diagnostics.
   - [x] Add filtered memory/register observers and migrate unified trace
     memory/register channels.
   - [x] Add filtered DMA activation observers and migrate the unified trace
     DMA channel.
   - [x] Add frame and interrupt/error observers and migrate unified trace
     lifecycle/error channels.
   - [x] Add final-mix audio observers; retain the raw sample diagnostic and
     mutating replacement mixer under their distinct contracts.
4. [ ] Add safe-point mutation commands and versioned save-state serialization.
   - [x] Add a copied, ordered, queryable safe-point queue for bounded memory
     writes and one-frame input overrides.
   - [ ] Add caller-buffer versioned serialization and queue state loads through
     the same safe-point boundary.
5. [ ] Remove external concrete-structure access only after all game adapters use
   the versioned boundary.

The cutover scope for item 5 is tracked in
`docs/runner-concrete-access-audit.md`.  The application begins this phase with
27 exact concrete-header exceptions guarded by a build-time test.  Serialization,
asynchronous pinning, language bindings, and broad SDK fixtures are follow-up
work; the only optimization included in this pass is measured dirty-range or
scatter/gather support that removes a real bulk copy from a migrated consumer.

Cross-platform CI should compile the ABI from C, C++, Go/cgo, and Rust bindgen
fixtures; poison expired borrowed views in tests; verify serialized versions;
and count bytes cloned per frame so a low-copy regression fails visibly.
