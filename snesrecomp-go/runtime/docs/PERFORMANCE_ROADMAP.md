# Runner performance implementation record

This document records the measured optimization sequence that brought the
current runtime to parity and beyond. It is not a current source-porting plan.
References to the historical comparison runner and the former `next` name are
dated benchmark evidence, not active build choices or maintained fallbacks.

## Baseline

The pinned baseline is the release build on Apple ARM64 using
`saves/sim-actions.rec`, the SHA-256-pinned SRAM fixture, and the matching
settings fixture.  Both runners stop at game frame 11,222 after 11,223 rendered
frames.

| Runner | 6,000-frame headless run | Complete replay with four snapshots |
| --- | ---: | ---: |
| `legacy` | 1.78 s | comparison oracle |
| `next` | 15.73 s | 29.12 s |

The complete replay has exact legacy/next matches for four screenshots and
the corresponding WRAM, VRAM, CGRAM, OAM, high-OAM, and PPU snapshots.  Final
WRAM, SRAM, and dispatch history also match.  Keep those artifacts as the
acceptance oracle for every optimization.

Sampling the same workload shows:

- about 96% of `next` samples under `ActRaiserDrawPpuFrame`, almost entirely
  `render_line`/`render_line_to`;
- 2,476 of 5,512 `next` samples directly in `sample_bg`, before counting its
  caller-side source selection and per-pixel composition;
- the historical runner spends most of its render samples in whole-tile
  background rasterization, a compact resolved-line composite, and one sprite
  evaluation per scanline; and
- row clears and memory copies are below the first-order hotspots in both
  profiles.

This means the remaining gap is a PPU algorithm problem, not a general CPU,
DMA, audio, allocator, or frame-copy problem.

## Tiled-renderer checkpoint (2026-08-25)

The first PPU optimization pass reduced the matched 6,000-frame median from
15.73 seconds to 4.10 seconds.  A fresh interleaved five-run comparison put the
legacy runner at 1.90 seconds, so `runtime` is now about 3.8 times faster
than its starting point and 2.2 times slower than legacy.  Both runners used
the same ROM, replay, SRAM seed, settings, dummy audio/video drivers, release
configuration, and frame limit.

The optimized path now includes:

- a native tile-span renderer for SNES tiled BG Modes 0 through 6, with the
  pixel renderer retained as the diagnostic and uncommon-policy fallback;
- self-validating direct-index 2-bpp and 4-bpp decoded-row caches;
- uniform hardware-window detection that hoists invariant window tests out of
  the pixel loops;
- tile-span OBJ scanline resolution and clipped tile-span rasterization for the
  public `PpuRasterizeParts` component API; and
- direct/reference parity coverage for Modes 0 through 6 across bit depths,
  priorities, tile sizes, flips, scrolling, windows, color math, and OBJ.

The complete 12,000-frame acceptance replay finishes in approximately 7.4
seconds with artifact capture enabled.  Its four screenshots, all 24 WRAM,
VRAM, CGRAM, OAM, high-OAM, and PPU snapshots, final WRAM/SRAM, and dispatch
history are byte-identical to the pinned legacy oracle.

## Scanline/compositor checkpoint (2026-08-25)

The second PPU pass reduced the matched 6,000-frame median from 3.49 seconds
to 2.62 seconds.  A fresh five-run interleaved comparison measured legacy at
1.91 seconds, leaving `runtime` 1.37 times slower instead of 1.80 times
slower at the start of the pass.  Relative to the original 15.73-second
baseline, the runner is now 6.0 times faster.

The portable improvements in this checkpoint are:

- two 64-bit OAM eligibility masks per scanline, rebuilt lazily when OBJ
  geometry changes;
- small sorted hardware-window runs for BG, OBJ, and color-window scanout;
- a priority-first packed resolved-pixel key, making winner selection a direct
  integer comparison;
- fused color math and brightness expansion, plus a separate simple-color loop
  when color math is disabled, prevented, or has no visible effect;
- fixed-shift full-tile scanout for 2-bpp, 4-bpp, and 8-bpp BG modes, retaining
  the variable-span path for clipped and windowed tile fragments; and
- one bulk authentic-row copy after composition instead of a second store in
  every live pixel iteration.

The simple-color loop and fixed 8-pixel tile path were the two large omissions:
they improved the matched replay by about 12% and 13% respectively.  Forced
64-byte stack alignment was neutral and was removed.  A 4-KiB color-math LUT
was also neutral/slower and was removed.  Existing fill loops already compile
to optimized platform memory intrinsics, so replacing them with hand-written
`memcpy`/`memset` calls is not a remaining first-order opportunity.

The complete acceptance replay remains byte-identical across all 31 compared
artifacts: four screenshots, 24 intermediate hardware snapshots, and final
WRAM, SRAM, and dispatch history.

## Mode-7 checkpoint (2026-08-25)

The native resolved-line renderer now covers Mode 7 as well as tiled Modes 0
through 6.  With diagnostic tracing explicitly disabled, five interleaved
6,000-frame comparisons measured a 2.62-second median for the preceding
checkpoint and 2.33 seconds for the Mode-7 build, an 11.1% overall
improvement.  The same comparison put legacy at 1.93 seconds, so the remaining
measured gap is about 1.21x rather than 1.37x.

The fast path computes the two affine numerators once at the left edge and
advances them with fixed integer increments across the scanline.  Each source
pixel is fetched once and offered to both main/subscreen and EXTBG winners;
hardware-window plans and the existing compact compositor remain shared with
the tiled modes.  No platform SIMD, assembly, or compiler-specific CPU feature
is required.

Reference/fast tests cover Mode-7 wrapping, large-field transparency,
character fill, X/Y flips, EXTBG priorities, main/subscreen windows, OBJ,
color math, and authentic output.  Mode-7 non-unit mosaic and the explicitly
requested reference renderer remain on the general pixel path.  The complete
replay is still byte-identical across all 31 acceptance artifacts.

## Main-screen tile checkpoint (2026-08-25)

The full-tile BG loop now selects main-only versus general main/subscreen
output once per eight-pixel group.  Previously the common main-only scanline
retested both constant screen-visibility flags for every decoded pixel; the
generated code contained as many as 16 redundant conditional branches per
tile.  This is a mode- and game-independent optimization for tiled Modes 0
through 6.

Five interleaved runs measured `runtime` at 2.26 seconds while legacy
remained at 1.93 seconds.  Relative to the immediately preceding 2.33-second
Mode-7 checkpoint, this is another 3.0% improvement and reduces the measured
gap to approximately 1.17x.  The reference/fast matrix now explicitly tests
both main-only and main-plus-subscreen scanout across every BG mode.

## Legacy-parity checkpoint (2026-08-25)

The final branch/locality pass reached the migration performance gate.  Both
binaries were warmed before timing, then measured as 12 adjacent pairs with
the first runner alternating in every pair.  Diagnostics were disabled and
the ROM, replay, SRAM, settings, release configuration, dummy drivers, and
6,000-frame limit were identical.  Despite shared system thermal drift, the
new runner won 10 pairs, tied one, and lost one.  Its aggregate median was
2.145 seconds versus 2.205 seconds for legacy, about 2.7% faster; the median
within-pair advantage was 0.06 seconds.

The accepted portable changes are:

- use conditional selection in the common main-only full-tile write instead
  of two data-dependent transparency/priority branches;
- iterate the existing two-word OBJ eligibility masks in cyclic hardware OAM
  order instead of probing all 128 slots on every scanline;
- carry four visible-pixel bitmask words out of OBJ rasterization so
  composition visits only opaque sprite winners rather than branching across
  256 pixels;
- leave the subscreen priority line uninitialized when no SNES mode or color
  operation can consume it; and
- cache the 256 brightness-expanded CGRAM colors once per output binding or
  brightness generation, maintaining changed entries on register writes.  The
  cache is explicitly invalidated at output binding and savestate load so
  host-side direct state inspection/editing remains coherent.

Individually confirmed replay medians showed approximately 4% for the
main-only conditional selection, 6% for OAM set-bit iteration, 5.4% for sparse
OBJ composition, 1.7% for skipping the unused subscreen clear, and 3--7% for
the derived RGB palette depending on system load.  These percentages are
incremental measurements and should not be added together.

Several plausible branch rewrites were measured and removed: unconditional
dual-screen stores, per-layer rank hoisting, a mode-indexed active-layer loop,
and forced source-level 2/4/8-bpp specialization were neutral or slower.  The
compiler already hoisted or unswitched most of those invariants, while the
extra stores/code size outweighed removed branches.

The sign-off replay remains byte-identical across all 31 oracle artifacts.
The standalone runner suite passes 25/25 tests, the full application suite
passes 90/90 (including GPU paths), and all Go packages pass.

## Compile-time tile SIMD checkpoint (2026-08-25)

A fresh current-tree baseline first confirmed that the scalar new runner still
cleared the legacy gate: 12 warmed, adjacent, alternating-order pairs measured
1.6867 seconds for `runtime` and 1.7392 seconds for legacy, making the new
runner 3.02% faster on the pinned 6,000-frame workload.

The remaining profile was still dominated by complete, unwindowed tiled-BG
winner selection.  That loop now spreads the compact cached 2-bpp or 4-bpp row
into eight byte lanes and performs its transparency and unsigned-priority
selection eight pixels at a time.  ARM NEON and x86 SSE2 implementations are
chosen at compile time; all other targets retain the proven scalar loop.  No
runtime feature probe or dispatch occurs, and the decoded-row caches remain
compact rather than doubling their footprint to store expanded pixels.

Against a preserved pre-change `runtime` binary, 12 warmed alternating
pairs measured 1.6114 seconds for NEON and 1.6871 seconds for scalar, a 4.49%
median improvement.  A separate final 12-pair comparison measured the NEON
runner at 1.6095 seconds versus legacy at 1.7387 seconds, 7.43% faster.  Only
the Apple ARM64 path has a performance claim; the x86-64 SSE2 and forced
generic paths were cross-compiled to validate portability and await native
host benchmarking.

The complete 12,000-frame replay remained byte-identical to the preserved
scalar runner at four representative snapshots: all 29 snapshot, final-SRAM,
and auxiliary artifacts matched by SHA-256.  The root PPU integration tests
and all 25 standalone runner tests also pass with the NEON path enabled.

## Tiled-BG register-invariant checkpoint (2026-08-25)

The post-SIMD profile exposed one more portable compiler boundary.  Decoded
row cache helpers receive the complete mutable `Ppu`, so the optimizer could
not prove that those calls leave BG scroll and tilemap/character-base registers
unchanged.  The tile loop consequently reloaded and decoded invariant register
fields for every eight pixels.  The renderer now snapshots the horizontal
scroll, tilemap row base, character base, and tile stride once per layer and
scanline.  This is valid for every tiled BG mode because PPU register writes
occur between scanlines, never during one layer's synchronous resolve.

Twelve warmed alternating pairs measured 1.5456 seconds with the hoist versus
1.6079 seconds for the preserved SIMD baseline, a further 3.88% improvement.
A final same-cohort 12-pair comparison measured the new runner at 1.5444
seconds and legacy at 1.7352 seconds, making `runtime` 11.00% faster on
the pinned workload.  All 29 artifacts from the complete replay and four
representative snapshots remained byte-identical to the pre-hoist runner.

## Portable SIMD control and audio checkpoint (2026-08-25)

Architecture-specific implementations now share one explicit CMake gate,
`SNESRECOMP_ENABLE_SIMD`.  It defaults to `ON`, but the compiler's target
macros—not the machine running CMake—select NEON or SSE2.  Unsupported targets
therefore compile the portable C11 paths, cross-builds cannot accidentally
inherit the build host's instruction set, and
`-DSNESRECOMP_ENABLE_SIMD=OFF` forces the portable implementation for A/B and
portability checks.  A plain non-CMake C11 build also defaults to portable.

The DSP output resampler now interpolates a stereo pair with ARM64 NEON or x86
SSE2 double-precision operations.  Ring-wrap frames and all unsupported builds
continue through the original scalar helper.  A randomized reference test
covers five source steps, five ring positions (including 32-bit counter wrap),
and exact PCM, phase, and consumption equality.  Twelve alternating-order
focused trials on Apple ARM64 measured 0.2737 seconds for NEON and 0.3069
seconds for scalar, a 10.8% median improvement with identical output
checksums.

The existing complete-tile SIMD winner operation is also used for eligible
main-plus-subscreen and subscreen-only tiles in Modes 0 through 6.  The
main-only hot case remains separate, and the unchanged scalar fixed-shift
implementation remains the fallback.  The PPU parity matrix now explicitly
compares unwindowed main-plus-subscreen output in every tiled mode as well as
its existing windowed, main-only, flip, priority, and bit-depth coverage.

As an end-to-end validation of the master switch, 12 warmed alternating
6,000-frame pairs measured approximately 1.51 seconds with SIMD enabled and
1.60 seconds with portable code forced.  This comparison includes both the
existing tile SIMD and the new paths, so it is a build-switch result rather
than an incremental audio claim.  The complete 11,223-rendered-frame replay
was then run once through each configuration.  Its screenshot, four sets of
WRAM/VRAM/CGRAM/OAM/high-OAM/PPU snapshots, final WRAM/SRAM, state report, and
dispatch history were byte-identical across all 29 artifacts.  The 25-test
standalone suite passes on native ARM64 with SIMD enabled and disabled, and
the x86-64 SSE2 build passes the same 25 tests under Rosetta.

## Per-layer tiled-mosaic checkpoint (2026-08-26)

Fallback counters on the action-heavy Aitos replay found one remaining
algorithmic cliff: a non-unit mosaic bit on any visible BG rejected the whole
scanline from the native renderer.  Only about 1.8% of eligible scanlines had
that state, but those lines reran every BG, OBJ, window, main/subscreen, color
operation, and capture decision through the per-pixel reference renderer.
Death Heim did not exercise the rejection and therefore showed a much smaller
gap, confirming that this was a workload-shaped fallback rather than a general
tile-loop regression.

Tiled Modes 0 through 6 now resolve mosaic as a per-layer raster kernel.  The
kernel samples each display-anchored mosaic group once, repeats that packed
source winner across the group, and evaluates hardware windows at each
destination coordinate.  Other BGs, OBJ, composition, authentic output, and
capture stay on their existing native paths.  Widescreen margins retain their
per-source reference sampler until the arbitrary-span resolver can preserve
display-space mosaic phase; this no longer downgrades the authentic scanline.
Mode 7 mosaic remains on its reference affine path pending a dedicated kernel.

The reference/fast matrix covers Modes 0 through 6, 2/4/8-bpp sources,
8x8/16x16 tiles, group sizes 2, 5, and 16, independent layer masks, windows,
main/subscreen output, overlay capture, virtual tilemaps, and horizontal and
vertical presentation margins.  Native SIMD, forced-portable scalar, and
forced-32-bit configurations each pass all 25 standalone tests.

Five warmed Aitos runs measured 1.72, 1.72, 1.75, 1.74, and 1.73 seconds: a
1.73-second median versus approximately 1.96 seconds before the change and
the earlier 1.74-second legacy median.  Five-run medians for Death Heim and
SIM were 1.32 and 1.35 seconds respectively, within noise of their preceding
1.33 and 1.34-second measurements.  The optimization therefore closes the
action-replay structural gap without trading performance between modes.

## Virtual-tile span checkpoint (2026-08-26)

The first profile after removing whole-line mosaic fallback made the virtual
Mode-1 tilemap span the largest PPU leaf.  Its ordinary scanout still called a
provider's optional semantic-band lookup even though band metadata is consumed
only by overlay capture, then sent each complete unwindowed tile through the
clipped/windowed per-pixel loop.

Ordinary virtual scanout now skips unused band callbacks and applies complete
4-bpp tile winners through the same fixed eight-pixel operation used by VRAM
scanout.  NEON/SSE2 builds use their existing compile-time winner primitive;
all other targets retain a fully unrolled scalar implementation.  Clipped
tiles, hardware windows, capture bands, mirror/repeat margins, and unavailable
provider tiles continue through the prior general span loop.

Five native-SIMD runs measured medians of 1.31 seconds for Aitos, 1.17 seconds
for Death Heim, and 1.35 seconds for SIM.  Against the earlier legacy medians
of 1.74, 1.25, and 1.45 seconds, `runtime` is approximately 25%, 6%, and
7% faster respectively.  With `SNESRECOMP_ENABLE_SIMD=OFF`, the same replays
measured 1.49, 1.26, and 1.42-second medians: Aitos and SIM remain faster than
legacy while Death Heim is effectively tied.

Focused fast/reference tests cover aligned and clipped virtual tiles with
patterned pixels, both flips and priorities, main/subscreen resolution, and
the unused-band contract.  Aitos screenshots at frames 1000, 2000, 3000, and
4000 are byte-identical to the legacy runner; Death Heim and SIM frame-3000
screenshots and their final WRAM/SRAM are likewise exact.

## Synchronous DMA drain checkpoint (2026-08-26)

General DMA writes through `$420B` are synchronous in this runner.  The old
path nevertheless called the timer-stepped `dma_cycle` loop until every byte
completed.  Those timer iterations advanced no PPU, APU, beam, interrupt, or
master-clock state; they consumed host instructions and repeatedly searched
the eight channels.  The new portable `dma_run_to_idle` path walks selected
channels once and reuses the exact existing per-byte transfer primitive.
`dma_cycle` remains available as the reference/debug stepping path.

Differential tests cover every DMA mode in both directions, fixed/incrementing/
decrementing A-bus addresses, channel priority, 16-bit address wrapping,
zero-size 65,536-byte transfers, ordered bus effects, final channel state, and
DMA-to-PPU/APU-port routing.  Six warmed alternating forced-portable replay
pairs measured a 2.30-second median for the stepped path and 2.11 seconds for
the fast drain on the pinned 6,000-frame SIM workload, an 8.3% improvement.

The two paths produced byte-identical final WRAM, SRAM, state reports, and
dispatch histories for both the 6,000-frame SIM run and a 2,500-frame Fillmore
action run.  One paced 180-frame title pair also produced identical PCM, DSP
provenance, request, and suppression artifacts.  A repeated stepped-path
control demonstrated that host callback scheduling can change the retained
PCM length and individual song-event cycle stamps between otherwise identical
runs, so threaded audio artifacts remain a smoke check rather than a
deterministic acceptance oracle.  Future
cycle-accurate DMA should charge a central emulated master clock; reintroducing
host countdown spins would not supply that timing.

The forced-portable standalone suite passes 25/25, the application suite
passes 90/90 including its host-GPU tests, and all Go packages pass.

## Mode-7 locality and virtual-provider ABI checkpoint (2026-08-26)

Two remaining portable locality candidates were measured after the DMA
checkpoint.  A last-address/last-tile cache in both native Mode-7 resolvers was
an unambiguous rejection: six alternating 6,000-frame title/world-map pairs
measured a 1.735-second median before the cache and 1.875 seconds with it, an
8.1% regression.  Consecutive pixels do not reuse a Mode-7 tilemap cell often
enough on the affine workload to repay the comparisons and additional live
state.  The prototype was removed; do not retry that single-cell cache shape.

The virtual Mode-1 provider seam did have useful scanline locality, but only
after keeping it zero-copy.  An initial batch callback that copied tile words
and per-slot presence bytes into renderer-owned arrays reduced indirect calls
but slowed the 4,000-frame wide Aitos replay to 7.14 seconds.  The accepted
optional callback instead returns a borrowed immutable tile-word view, a word
stride, and a bounded run length.  A NULL view represents a finite-world gap;
returning zero preserves the original scalar callback.  Forward, reverse, row
wrap, and outside-world runs therefore require no tile copies or allocation,
and providers that cannot expose stable spans remain fully compatible.

Six adjacent alternating-order comparisons against the preserved post-DMA
binary measured baseline times of 6.78, 6.70, 6.69, 6.75, 6.72, and 6.76
seconds (6.735-second median), versus 6.58, 6.62, 6.54, 6.59, 6.57, and 6.56
seconds for the zero-copy span view (6.575-second median).  That is a 2.4%
end-to-end improvement.  The replay retained exactly 94,778,736 semantic tile
queries while reducing indirect provider invocations to 4,393,832, a 95.4%
reduction; tile, outside-world, and preflight counts were unchanged.

Fast/reference renderer tests now exercise forward and reverse spans,
finite-world gaps, mirror/repeat/clamp/raw margins, normal-scroll mirror wrap,
overlay bands, windows, priorities, and the unchanged scalar fallback.  Five
Aitos frame captures plus final state, WRAM, SRAM, and dispatch history are
byte-identical to the post-DMA baseline.  Native-SIMD, forced-portable, and
forced-32-bit standalone configurations each pass 25/25 tests.  The
application suite has 88 passes and the same two display-dependent GPU skips,
with no failures.  The legacy runner also builds with the optional ABI field
and ignores it.

## Hardware-shaped data structures

SNES state is small, bounded, and mostly addressed by integers.  Prefer dense,
fixed-capacity structures over general trees, heaps, and hash tables:

- VRAM, CGRAM, OAM, bus pages, tile rows, and generated dispatch slots support
  direct indexing or a shallow page table.
- Screen resolution has five possible source layers.  Rank-table lookup and an
  unrolled maximum are cheaper and more predictable than sorting or a heap.
- Window logic produces only a few endpoints.  Convert them to sorted spans or
  masks once per scanline, then consume the spans linearly.
- Sprite eligibility can be represented as two 64-bit masks per scanline.  A
  mask rebuilt when OAM/OBJ geometry becomes dirty can skip absent sprites
  while the evaluator still walks set bits in hardware OAM rotation order and
  enforces the 32-sprite/34-tile limits.
- Planar character data can use a direct-index decoded-tile-row cache keyed by
  VRAM word address and bpp.  VRAM writes and DMA mark the affected rows dirty;
  reads require no search and no allocation.
- Five-bit color components fit small add/subtract/half/brightness tables.
- Mode-7 affine coordinates should advance by fixed increments across a line
  rather than repeat the complete transform per pixel.

Trees or heaps only become attractive for large, sparse, dynamically changing
sets.  None of the measured PPU sets have that shape, and pointer chasing would
work against locality.  The sprite masks and decoded-row cache are candidates
after the tile-span renderer is established and reprofiled.

## ARMv6K low-power checkpoint (2026-08-25)

The first constrained-device target is a 32-bit ARM11-class build shaped like
the Nintendo 3DS homebrew toolchain: ARMv6K, MPCore tuning, hard-float VFP, and
no NEON.  The cached Zig compiler can use an ARM Linux ABI as a core compile
proxy on the Apple ARM64 development host.  All 34 standalone runner
translation units compile for that ISA when the two POSIX host-adapter units
receive the normal `_POSIX_C_SOURCE=200809L` declaration.  A real 3DS adapter
must replace those Linux launcher and monotonic-clock surfaces with libctru;
the devkitARM/libctru toolchain is not currently installed on this host.

The first accepted low-power specialization changes the PPU's OBJ eligibility
and opaque-pixel bitsets to use the target's native integer width.  ARM64 and
x86-64 retain the existing 64-bit representation and SIMD paths.  A 32-bit
target instead uses four 32-bit OBJ words and eight 32-bit pixel words, avoiding
wide de-Bruijn multiplication and wide bit iteration on ARM11.  The build can
force 32 or 64 bits with `SNESRECOMP_PPU_BIT_WORD_BITS`, allowing both layouts
to execute through tests on a 64-bit host.

Static ARMv6K code generation for the PPU reduced `UMULL` occurrences from
nine to two and reduced the translation-unit object from 51,376 to 50,992
bytes.  These are directional instruction/code-size results, not a device
performance claim.  Both the native-width and forced-32 configurations pass
the 25-test standalone suite.  In each 90-test application run, 88 tests pass
and the same two GPU tests skip because the test process has no SDL-visible
display; neither configuration reaches GPU initialization.  A complete replay
comparison matched all 39 captured screenshots, hardware snapshots,
final-state files, and diagnostic anomaly traces.  Twelve warmed alternating
desktop replay pairs measured 1.528550 seconds before the change and 1.527751
seconds after it, a neutral 0.05% difference in favor of the native-width
build; the 64-bit path therefore did not trade away desktop performance.

The next ARM11 candidates, in priority order, are:

1. measure decoded-row cache hit rates and test a small tagged cache profile;
   the ARMv6K `Ppu` is 486,832 bytes, of which the two decoded-row arrays alone
   consume 393,216 bytes, far beyond an ARM11 L1 working set;
2. replace remaining 64-bit packed-pixel work in non-NEON 8-bpp scanout with
   paired 32-bit words, retaining the 64-bit/NEON desktop implementation;
3. prototype fixed-point output resampling for 32-bit targets to remove the
   serial double-convert/multiply/convert sequence, accepting it only with
   exact PCM and phase parity; and
4. evaluate ARMv6 packed 8/16-bit media instructions only after native call and
   run-length counters show enough work to amortize packing and tail handling.

Native 3DS timing remains the release gate for all four candidates.  Cross
compilation and instruction counts can reject structurally expensive ideas,
but they cannot model the device's cache misses, VFP scheduling, memory bus,
or OS services accurately enough for a performance claim.

## Work already landed

- OBJ selection is cached per scanline and coordinate space instead of
  walking all 128 OAM entries for every output pixel.
- Main and subscreen winners are resolved together.
- A matching authentic surface is produced during the live resolve instead of
  rerendering or copying the complete frame.
- Native pixels bypass widescreen policy mapping.
- Layer priority uses a constant-time table.
- Disabled layers are rejected before tile sampling.
- Nonnegative native tile coordinates use shifts and masks.

Together these changes reduced the complete replay from about 295 seconds to
29.12 seconds without changing the parity artifacts.

A tile-coordinate cache was also tested and removed.  It retained the
per-pixel sampler and added cache bookkeeping and memory traffic, making the
run slower.  Do not retry that shape of optimization.

## Prioritized implementation

### P0: native tiled-mode span renderer — complete

The native scanline path computes each layer's vertical tile row once, walks
tilemap entries in visible spans, decodes each planar tile row once, and writes
directly to compact resolved main/sub lines.  Clipped edge tiles remain short
spans.  The implementation was generalized from Mode 1 to tiled BG Modes 0
through 6.  Per-layer mosaic, virtual tilemaps, capture exports, and margin
scanout now have specialized native or mixed-source paths; Mode 7 mosaic and
explicit diagnostic rendering retain the full reference renderer.

Acceptance gates:

1. render identical reference/fast lines for synthetic 2-bpp and 4-bpp tiles,
   both flips, both priorities, 8x8/16x16 tiles, scroll wraps, and window edges;
2. pass the standalone `runtime` suite and the root project suite;
3. preserve byte-exact complete-replay artifacts; and
4. improve the median of at least five matched headless replay runs.

### P1: scanline source and color-window runs — complete for tiled scanout

Uniform and non-uniform windows are represented as a small list of spans per
line.  The compact priority lines select main and subscreen sources, and each
color-window run chooses either the simple brightness loop or the full
color-math loop.  This removes repeated `window_inside`, `final_color`, and
layer-object construction from eligible tiled scanlines.

Overlay-export lines still use the reference capture path.  Moving those
exports onto the resolved source identity is P2; it must not force a second
background fetch.

### P2: capture-plan specialization — measured and deferred

Build a small capture plan at the start of a line.  Lines with no active
overlay capture skip capture tests entirely.  Active lines retain source
identity and semantic-band metadata in the resolved buffers so main-winner,
owning-screen, and full-add exports are single linear passes.

An exact implementation was benchmarked after P0/P1 and was neutral on the
acceptance replay, so it was removed.  Temporary fallback instrumentation also
confirmed that no active overlay capture intersected the authentic scanlines
in this workload; the apparent reference-renderer sample bucket was primarily
Mode 7.  Revisit this only with a representative workload that actively exports
overlay captures.

### P3: Mode-7 scanline stepping — complete

Mode 7 calculates the first affine coordinate once and increments it across
the scanline.  It shares resolved main/subscreen buffers, window spans, OBJ,
and color composition with tiled scanout while retaining the general sampler
for non-unit mosaic, active capture exports, margins, and diagnostic reference
rendering.

### P4: lower-copy host ABI and secondary systems

After the renderer no longer dominates, reprofile before changing CPU, DMA,
audio, or host presentation.  The component ABI should expose borrowed frame
and memory views, generation counters, and dirty ranges as described in
`ABI_ROADMAP.md`; asynchronous consumers explicitly pin or clone a snapshot.
That avoids future copies without compromising state ownership.

## Benchmark discipline

- Compare release builds with the same ROM, SRAM hash, settings, replay,
  headless/audio drivers, frame count, and diagnostics.
- Report median wall time and frames per second, not a single best run.
- Profile before and after; a faster microbenchmark does not override a slower
  game replay.
- Never accept a speedup with different WRAM, video, audio scheduling, or
  dispatch behavior.
- Keep the reference pixel path available through the migration so optimized
  scanlines can be checked directly in tests and replay diagnostics.
