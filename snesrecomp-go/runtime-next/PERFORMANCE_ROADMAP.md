# Runner performance roadmap

This document prioritizes `runtime-next` performance work from measured game
replays.  It is not a source-porting plan.  The historical runner is used as a
behavioral and performance oracle; replacement implementations remain
independently authored from public SNES behavior, this runner's own data
structures, and parity tests.

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
legacy runner at 1.90 seconds, so `runtime-next` is now about 3.8 times faster
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
1.91 seconds, leaving `runtime-next` 1.37 times slower instead of 1.80 times
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
color math, and authentic output.  Non-unit mosaic, active overlay exports,
presentation margins, and the explicitly requested reference renderer remain
on the general pixel path.  The complete replay is still byte-identical across
all 31 acceptance artifacts.

## Main-screen tile checkpoint (2026-08-25)

The full-tile BG loop now selects main-only versus general main/subscreen
output once per eight-pixel group.  Previously the common main-only scanline
retested both constant screen-visibility flags for every decoded pixel; the
generated code contained as many as 16 redundant conditional branches per
tile.  This is a mode- and game-independent optimization for tiled Modes 0
through 6.

Five interleaved runs measured `runtime-next` at 2.26 seconds while legacy
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
through 6; virtual tilemaps, active capture overrides, non-unit mosaic, Mode 7,
and presentation-margin policies deliberately retain the reference renderer.

Acceptance gates:

1. render identical reference/fast lines for synthetic 2-bpp and 4-bpp tiles,
   both flips, both priorities, 8x8/16x16 tiles, scroll wraps, and window edges;
2. pass the standalone `runtime-next` suite and the root project suite;
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
